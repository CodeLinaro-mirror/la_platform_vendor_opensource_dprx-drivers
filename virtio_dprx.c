/*Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *SPDX-License-Identifier: GPL-2.0-only  */

/* virtio_dprx.c - Virtual DPRx V4L2 Driver
 *
 * This is a virtual V4L2 driver that registers a video device
 * with a and uses virtio-media to communicate to the DPRx BE.
 */

#define pr_fmt(fmt)	"[virtio_dprx:%s:%d] " fmt, __func__, __LINE__
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>

#include <media/frame_vector.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-memops.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "virtio_dprx.h"

#define	MM_DPRX_CMD 1801
#define	MM_DPRX_EVENT 1802

static bool module_removed;

static void virtio_dprx_session_free(struct virtual_dprx_dev *vdprx,
				      struct virtio_dprx_session *session);

static char *event_id_to_string(struct virtio_media_event_header *hdr)
{
	switch (hdr->event) {

	case VIRTIO_MEDIA_EVT_ERROR:
		return "VIRTIO_MEDIA_EVT_ERROR";
	case VIRTIO_MEDIA_EVT_DQBUF:
		return "VIRTIO_MEDIA_EVT_DQBUF";
	case VIRTIO_MEDIA_EVT_EVENT:
		return "VIRTIO_MEDIA_EVT_EVENT";
	default :
		pr_err("unknown event %d", hdr->event);
		return "UNKNOWN";
	}
}

static char buff[1000];
struct virtio_media_event_header *virtio_dprx_get_event_buffer(struct virtual_dprx_dev *vdprx)
{
	uint32_t size = 1000;
	int rc = 0;
	struct virtio_media_event_header *hdr;
	uint32_t hab_socket =vdprx->hab_socket_event;

	memset(buff, 0x00, size);
	rc = habmm_socket_recv(hab_socket, buff, &size, (uint32_t)-1, 0);
	if (rc) {
		pr_err("virtio :socket_recv failed <%d>\n",rc);
		if (rc == -ENODEV)
			vdprx->stop = true;
		return NULL;
	}

	hdr = (struct virtio_media_event_header *)buff;
	return hdr;
}

int virtio_dprx_event_kthread(void *d)
{
	struct virtual_dprx_dev *vdprx = (struct virtual_dprx_dev *)d;
	int ret;

	ret = virtio_dprx_virtq_open(vdprx, vdprx->cell_index);
	if (ret)
		return 0;

start_thread:
	while(!vdprx->stop) {
		virtio_dprx_process_events(vdprx);
	}

	pr_err("Exiting event thread");
	if (!module_removed) {
		pr_err("module_removed access data");
		mutex_lock(&vdprx->sessions_lock);
		struct list_head *p;
		list_for_each(p, &vdprx->sessions) {
			struct virtio_dprx_session *s =
				list_entry(p, struct virtio_dprx_session, list);
			struct v4l2_event v4l2_err_evt = {
				.type = V4L2_EVENT_PRIVATE_START, // a custom error event type
				.u.data[0] = 1,    // pass error code
			};

			v4l2_event_queue_fh(&s->fh, &v4l2_err_evt);
			wake_up_interruptible(&s->fh.wait);
		}
		mutex_unlock(&vdprx->sessions_lock);

		virtio_dprx_virtq_close(vdprx);

		pr_info("virtio_dprx_virtq_open waiting");
		if (!virtio_dprx_virtq_open(vdprx, vdprx->cell_index)) {
			vdprx->stop = false;
			goto start_thread;
		}
		pr_info("virtio_dprx_virtq_open done");
	}
	else
		pr_info("module removed so nothing to be done");
	return 0;
}

static struct virtio_dprx_session *virtio_dprx_session_alloc(
		struct virtual_dprx_dev *vdprx,
		uint32_t id,
		bool nonblocking_dequeue)
{
	struct virtio_dprx_session *session;
	int i;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		goto err_session;

	session->id = id;
	session->nonblocking_dequeue = nonblocking_dequeue;

	INIT_LIST_HEAD(&session->list);

	v4l2_fh_init(&session->fh, &vdprx->video_dev);
	v4l2_fh_add(&session->fh);

	for (i = 0; i <= V4L2_BUF_TYPE_VIDEO_CAPTURE; i++)
		INIT_LIST_HEAD(&session->queues[i].pending_dqbufs);

	mutex_init(&session->queues_lock);
	mutex_init(&session->dqbufs_lock);

	init_waitqueue_head(&session->dqbuf_wait);

	mutex_lock(&vdprx->sessions_lock);
	list_add_tail(&session->list, &vdprx->sessions);
	mutex_unlock(&vdprx->sessions_lock);

	return session;

err_session:
	return ERR_PTR(-ENOMEM);
}

static void virtio_dprx_session_free(struct virtual_dprx_dev *vdprx,
				      struct virtio_dprx_session *session)
{
	int i;

	mutex_lock(&vdprx->sessions_lock);
	list_del(&session->list);
	mutex_unlock(&vdprx->sessions_lock);

	v4l2_fh_del(&session->fh);
	v4l2_fh_exit(&session->fh);

	for (i = 0; i <= V4L2_BUF_TYPE_VIDEO_CAPTURE; i++)
		vfree(session->queues[i].buffers);

	kfree(session);
}

static struct virtio_dprx_session *virtio_dprx_find_session(
		struct virtual_dprx_dev *vdprx,
		uint32_t id)
{
	struct list_head *p;
	struct virtio_dprx_session *session = NULL;

	mutex_lock(&vdprx->sessions_lock);
	list_for_each(p, &vdprx->sessions) {
		struct virtio_dprx_session *s =
			list_entry(p, struct virtio_dprx_session, list);
		if (s->id == id) {
			session = s;
			break;
		}
	}
	mutex_unlock(&vdprx->sessions_lock);

	return session;
}

static int virtio_dprx_device_open(struct file *file)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	int rc = 0;
	struct virtio_dprx_session *session;
	uint32_t session_id;
	uint32_t req_size = sizeof(struct virtio_media_cmd_open);
	void *req = kzalloc(req_size, GFP_KERNEL);
	struct virtio_media_cmd_open *cmd_open = (struct virtio_media_cmd_open *)req;
	uint32_t resp_size = sizeof(struct virtio_media_resp_open);
	void *resp = kzalloc(resp_size, GFP_KERNEL);
	struct virtio_media_resp_open *resp_open= (struct virtio_media_resp_open *)resp;

	if (!req || !resp) {
		rc = -ENOMEM;
		goto error;
	}

	mutex_lock(&vdprx->vlock);

	cmd_open->hdr.cmd = VIRTIO_MEDIA_CMD_OPEN;

	rc = virtio_dprx_virtq_send_command(vdprx, req, req_size, resp, resp_size);
	if(rc) {
		pr_err("(%s): open failed with rc=%d\n", dev_name(&video_dev->dev), rc);
		mutex_unlock(&vdprx->vlock);
		rc = -ENODEV;
		goto error;
	}

	session_id = resp_open->session_id;

	mutex_unlock(&vdprx->vlock);

	session = virtio_dprx_session_alloc(vdprx, session_id, (file->f_flags & O_NONBLOCK));

	if (IS_ERR(session))
		return PTR_ERR(session);

	file->private_data = &session->fh;
error:
	if (req)
		kfree(req);
	if (resp)
		kfree(resp);

	return rc;
}

static int virtio_dprx_device_close(struct file *file)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *session = fh_to_session(file->private_data);
	uint32_t req_size = sizeof(struct virtio_media_cmd_close);
	void *req = kzalloc(req_size, GFP_KERNEL);
	struct virtio_media_cmd_close *cmd_close = (struct virtio_media_cmd_close *)req;
	uint32_t resp_size = sizeof(struct virtio_media_resp_header);
	void *resp = kzalloc(resp_size, GFP_KERNEL);
	int rc;
	pr_info("virtio_dprx_device_close called \n");
	if (!req || !resp) {
		rc = -ENOMEM;
		goto error;
	}

	mutex_lock(&vdprx->vlock);

	cmd_close->hdr.cmd = VIRTIO_MEDIA_CMD_CLOSE;
	cmd_close->session_id = session->id;

	rc = virtio_dprx_virtq_send_command(vdprx, req, req_size, resp, resp_size);
	if(rc)
		pr_err("virtio: closed failed with rc=%d\n", rc);

	mutex_unlock(&vdprx->vlock);

	virtio_dprx_session_free(vdprx, session);

error:
	if (req)
		kfree(req);
	if (resp)
		kfree(resp);
	pr_info("virtio_dprx_device_close returned \n");
	return rc;
}


static void virtio_dprx_process_dqbuf_event(struct virtual_dprx_dev *vdprx,
				 struct virtio_dprx_session *session,
				 struct virtio_media_event_dqbuf *dqbuf_evt)
{
	struct virtio_dprx_buffer *dqbuf;
	const enum v4l2_buf_type queue_type = dqbuf_evt->buffer.type;
	struct virtio_dprx_queue_state *queue;
	typeof(dqbuf->buffer.m) buffer_m;

	if (queue_type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		pr_err("(%s) unmanaged queue %d passed to dqbuf event",
				dev_name(&vdprx->video_dev.dev),
				dqbuf_evt->buffer.type);
		return;
	}
	queue = &session->queues[queue_type];

	if (dqbuf_evt->buffer.index >= queue->allocated_bufs) {
		pr_err(" (%s) invalid buffer ID %d for queue %d in dqbuf event",
				dev_name(&vdprx->video_dev.dev),
				dqbuf_evt->buffer.index, dqbuf_evt->buffer.type);
		return;
	}

	dqbuf = &queue->buffers[dqbuf_evt->buffer.index];

	/*
	 * Preserve the 'm' union that was passed to us during QBUF so userspace
	 * gets back the information it submitted.
	 */
	buffer_m = dqbuf->buffer.m;
	memcpy(&dqbuf->buffer, &dqbuf_evt->buffer, sizeof(dqbuf->buffer));
	dqbuf->buffer.m = buffer_m;

	/* Handle DMA-BUF memory type */
	if (dqbuf->buffer.memory == V4L2_MEMORY_DMABUF) {
		dqbuf->buffer.m.fd = buffer_m.fd;
	}

	/* Set the DONE flag as the buffer is waiting for being dequeued. */
	dqbuf->buffer.flags |= V4L2_BUF_FLAG_DONE;

	mutex_lock(&session->dqbufs_lock);
	list_add_tail(&dqbuf->list, &queue->pending_dqbufs);
	queue->queued_bufs -= 1;
	mutex_unlock(&session->dqbufs_lock);
	wake_up(&session->dqbuf_wait);
}

void virtio_dprx_process_events(struct virtual_dprx_dev *vdprx)
{
	struct virtio_media_event_error *error_evt;
	struct virtio_media_event_dqbuf *dqbuf_evt;
	struct virtio_media_event_event *event_evt;
	struct virtio_dprx_session *session;
	struct virtio_media_event_header *evt;
	unsigned int len;

	mutex_lock(&vdprx->events_lock);
	if ((evt = virtio_dprx_get_event_buffer(vdprx))) {
		pr_debug("event received %s \n", event_id_to_string(evt));

		session = virtio_dprx_find_session(vdprx, evt->session_id);
		if (session == NULL) {
			pr_err("cannot find session %d\n",
			  evt->session_id);
			goto end_of_event;
		}
		switch (evt->event) {
		case VIRTIO_MEDIA_EVT_ERROR:
			error_evt = (struct virtio_media_event_error *)evt;
			pr_err("received error %d for session %d \n",
				error_evt->errno, error_evt->hdr.session_id);
			struct v4l2_event v4l2_err_evt = {
				.type = V4L2_EVENT_PRIVATE_START, // a custom error event type
				.u.data[0] = error_evt->errno,    // pass error code
			};
			v4l2_event_queue_fh(&session->fh, &v4l2_err_evt);
			wake_up_interruptible(&session->fh.wait);
			len = sizeof(struct virtio_media_event_error);
			memset((char *)evt, 0x00, len);
			break;
		case VIRTIO_MEDIA_EVT_DQBUF:
			dqbuf_evt = (struct virtio_media_event_dqbuf *)evt;
			virtio_dprx_process_dqbuf_event(vdprx, session, dqbuf_evt);
			len = sizeof(struct virtio_media_event_dqbuf);
			memset((char *)evt, 0x00, len);
			break;
		case VIRTIO_MEDIA_EVT_EVENT:
			event_evt = (struct virtio_media_event_event *)evt;
			struct v4l2_event event = {
				.type = V4L2_EVENT_SOURCE_CHANGE,
				.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
			};
			v4l2_event_queue_fh(&session->fh, &event);
			wake_up_interruptible(&session->fh.wait);
			len = sizeof(struct virtio_media_event_event);
			memset((char *)evt, 0x00, len);
			break;
		default:
			pr_info("unknown event type %d\n", evt->event);
			break;
		}
	}
end_of_event:
	mutex_unlock(&vdprx->events_lock);
}

/**
* Event callback. This processes the event
*/
static void virtio_dprx_event_work(struct work_struct *work)
{
	struct virtual_dprx_dev *vdprx = container_of(work, struct virtual_dprx_dev, eventq_work);

	virtio_dprx_process_events(vdprx);
}

/*
 * poll for a virtio-dprx device.
*/

static __poll_t virtio_dprx_device_poll(struct file *file, poll_table *wait)
{
	struct virtio_dprx_session *session = fh_to_session(file->private_data);
	enum v4l2_buf_type capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct virtio_dprx_queue_state *capture_queue = &session->queues[capture_type];

	__poll_t req_events = poll_requested_events(wait);
	__poll_t rc = 0;

	pr_debug("virtio_dprx_device_poll waiting for event %x", req_events);

	poll_wait(file, &session->dqbuf_wait, wait);
	poll_wait(file, &session->fh.wait, wait);

	mutex_lock(&session->dqbufs_lock);

	if (req_events & (EPOLLIN | EPOLLRDNORM)) {
		if (!capture_queue->streaming) {
			// Streaming not started: do NOT signal error, just return 0
			pr_debug("poll: streaming not active, blocking...");
		} else if (!list_empty(&capture_queue->pending_dqbufs)) {
			rc |= EPOLLIN | EPOLLRDNORM;
		}
		// If streaming is active but no buffers ready, return 0 (block)
	}

	mutex_unlock(&session->dqbufs_lock);

	if (v4l2_event_pending(&session->fh)) {
		pr_info("Event in the Queue");
		rc |= EPOLLPRI;
	}
	else
		pr_debug("no event present or subscribed \n");

	return rc; // If rc == 0, poll() will block
}

int dprx_send_properties(struct virtual_dprx_dev *vdprx, bool to_write, u32 data[8])
{
	struct virtio_media_cmd_propblob prop_blob;
	struct virtio_media_resp_property_blob resp;
	int rc = 0;

	prop_blob.hdr.cmd = VIRTIO_MEDIA_PROPERTY_BLOB;
	prop_blob.session_id = 0; //Todo fix the session ID
	prop_blob.property_id = vdprx->property_id;
	prop_blob.write_or_read = to_write?1:0; //1 -> write 0 -> read

	if (to_write)
		memcpy(prop_blob.data, data, sizeof(prop_blob.data));

	rc = virtio_dprx_virtq_send_command(vdprx,
			&prop_blob,
			sizeof(struct virtio_media_cmd_propblob),
			&resp,
			sizeof(struct virtio_media_resp_property_blob));
	if (rc) {
		pr_err(" dprx_send_properties failed  with rc=%d\n", rc);
		goto error;
	}

	if (!resp.hdr.status)
		memcpy(data, resp.data, sizeof(resp.data));
error:
	return rc;
}

static ssize_t dprx_node_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct virtual_dprx_dev *vdprx = dev_get_drvdata(dev);
	ssize_t len;

	if (!vdprx)
		return -ENODEV;

	mutex_lock(&vdprx->vlock);
	// Report last stored value + parsed property_id
	len = scnprintf(buf, PAGE_SIZE, "value=\"%s\" property_id=%u\n",
			vdprx->sysfs_value, vdprx->property_id);
	mutex_unlock(&vdprx->vlock);

	return len;
}

static ssize_t dprx_node_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct virtual_dprx_dev *vdprx = dev_get_drvdata(dev);
	int rc;
	u32 property_id;

	if (!vdprx)
		return -ENODEV;

	mutex_lock(&vdprx->vlock);

	strscpy(vdprx->sysfs_value, buf, sizeof(vdprx->sysfs_value));

	rc = kstrtou32(buf, 10, &property_id); // safer than kstrtoint for u32
	if (rc) {
		dev_err(dev, "kstrtou32 failed: rc=%d for buf='%s'\n", rc, buf);
		mutex_unlock(&vdprx->vlock);
		return -EINVAL;
	}

	vdprx->property_id = property_id;

	mutex_unlock(&vdprx->vlock);
	return count;
}

static DEVICE_ATTR_RW(dprx_node);

static struct attribute *vdprx_attrs[] = {
	&dev_attr_dprx_node.attr,
	NULL,
};

static const struct attribute_group vdprx_attr_group = {
	.name  = NULL,
	.attrs = vdprx_attrs,
};

static const struct v4l2_file_operations fops = {
	.owner = THIS_MODULE,
	.open = virtio_dprx_device_open,
	.release = virtio_dprx_device_close,
	.unlocked_ioctl = virtio_dprx_device_ioctl,
	//No mmap support
	.poll = virtio_dprx_device_poll,
};

void dprx_device_release(struct video_device *video_dev)
{
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	pr_info(" (%s) v4l2 device release for %d\n",
			dev_name(&vdprx->video_dev.dev),
			vdprx->device_id);
}

static int vdprx_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	uint32_t value;
	char link_name[32];
	struct virtual_dprx_dev *vdprx;

	dev_info(dev, "loading DPRx module \n");

	ret = of_property_read_u32(dev->of_node, "cell-index", &value);
	if (ret) {
		dev_warn(dev, "cell-index not found, defaulting to 0\n");
		value = 0;
	}
	snprintf(link_name, sizeof(link_name), "dprx_card%d", value);

	vdprx = devm_kzalloc(dev, sizeof(*vdprx), GFP_KERNEL);
	if (!vdprx)
		return -ENOMEM;
	vdprx->cell_index = value;
	INIT_LIST_HEAD(&vdprx->sessions);
	mutex_init(&vdprx->sessions_lock);

	mutex_init(&vdprx->events_lock);
	INIT_WORK(&vdprx->eventq_work, virtio_dprx_event_work);

	mutex_init(&vdprx->vlock);

	vdprx->mmid_cmd = MM_DPRX_CMD;
	vdprx->mmid_event = MM_DPRX_EVENT;

	ret = v4l2_device_register(dev, &vdprx->v4l2_dev);
	if (ret)
		goto close_vq;


	strscpy(vdprx->video_dev.name, "VirtualDPRx", sizeof(vdprx->video_dev.name));
	vdprx->video_dev.v4l2_dev = &vdprx->v4l2_dev;
	vdprx->video_dev.fops = &fops;
	vdprx->video_dev.ioctl_ops = &virtio_dprx_ioctl_ops;
	vdprx->video_dev.release = dprx_device_release;
	vdprx->video_dev.dev_parent = dev;
	vdprx->video_dev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;

	ret = video_register_device(&vdprx->video_dev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto unreg_dev;

	pr_info("%s: DPRx video device initialized\n", dev_name(&vdprx->video_dev.dev));

	vdprx->device_id = value;

	platform_set_drvdata(pdev, vdprx);

	// Create per-device sysfs attributes
	ret = sysfs_create_group(&pdev->dev.kobj, &vdprx_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "sysfs_create_group failed: %d\n", ret);
		return ret;
	}

	ret = sysfs_create_link(kernel_kobj, &vdprx->video_dev.dev.kobj, link_name);
	if(ret) {
		pr_err("(%s) symklink creation failed \n", dev_name(&pdev->dev));
		// Remove sysfs attributes
		sysfs_remove_group(&pdev->dev.kobj, &vdprx_attr_group);
		goto video_unreg;
	}
	module_removed = false;

	vdprx->stop = false;
	kthread_run(virtio_dprx_event_kthread, vdprx, "virtio dprx kthread");

	dev_info(dev, "Virtual DPRx probed\n");
	return 0;

video_unreg:
	video_unregister_device(&vdprx->video_dev);

unreg_dev:
	v4l2_device_unregister(&vdprx->v4l2_dev);

close_vq :
	virtio_dprx_virtq_close(vdprx);

	dev_err(dev, "Virtual DPRx probed Failed %d\n", ret);
	return ret;
}

static void vdprx_remove(struct platform_device *pdev)
{
	struct virtual_dprx_dev *vdprx = platform_get_drvdata(pdev);
	struct list_head *p, *n;
	char link_name[32];

	video_unregister_device(&vdprx->video_dev);
	v4l2_device_unregister(&vdprx->v4l2_dev);

	list_for_each_safe(p, n, &vdprx->sessions) {
		struct virtio_dprx_session *s =
			list_entry(p, struct virtio_dprx_session, list);

		virtio_dprx_session_free(vdprx, s);
	}

	snprintf(link_name, sizeof(link_name), "dprx_card%d", vdprx->device_id);
	sysfs_remove_link(kernel_kobj, link_name);

	// Remove sysfs attributes
	sysfs_remove_group(&pdev->dev.kobj, &vdprx_attr_group);

	module_removed = true;
	virtio_dprx_virtq_close(vdprx);

	pr_info("Virtual Dprx removed\n");
}

#ifdef CONFIG_PM_SLEEP
static int dprx_suspend(struct device *dev)
{
	return 0;
}

static int dprx_resume(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops dprx_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dprx_suspend, dprx_resume)
};

static const struct of_device_id dprx_of_match[] = {
	{ .compatible = "qcom,virtio-dprx" },
	{ }
};

MODULE_DEVICE_TABLE(of, dprx_of_match);

static struct platform_driver vdprx_platform_driver = {
	.probe = vdprx_probe,
	.remove = vdprx_remove,
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = dprx_of_match,
		.pm = &dprx_pm_ops,
	},
};

static int __init vdprx_init(void)
{
	platform_driver_register(&vdprx_platform_driver);
	return 0;
}

static void __exit vdprx_exit(void)
{
	platform_driver_unregister(&vdprx_platform_driver);
}

module_init(vdprx_init);
module_exit(vdprx_exit);
MODULE_DESCRIPTION("Virtual V4L2 dprx");
MODULE_AUTHOR("Satbir Singh");
MODULE_LICENSE("GPL");

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0))
MODULE_IMPORT_NS(DMA_BUF);
#endif
