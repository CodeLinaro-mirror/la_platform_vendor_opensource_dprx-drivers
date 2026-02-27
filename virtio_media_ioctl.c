/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+ */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
/*
 *
 * Copyright (c) 2023-2024 Google LLC.
 */
#define pr_fmt(fmt)	"[virtio_dprx:%s:%d] " fmt, __func__, __LINE__
#include "virtio_dprx.h"
/* Convert a V4L2 IOCTL into the IOCTL code we can give to the host */
#define VIRTIO_DPRX_IOCTL_CODE(IOCTL) ((IOCTL >> _IOC_NRSHIFT) & _IOC_NRMASK)

#define DPRX_NAME "virtio-dprx"

#define PRINT_IOCTL(cmd) \
	pr_debug("IOCTL: %s (raw=0x%lx type=%lu nr=%lu dir=%s size=%lu)\n", \
	ioctl_to_str(cmd), \
	(unsigned long)(cmd), \
	(unsigned long)_IOC_TYPE(cmd), \
	(unsigned long)_IOC_NR(cmd), \
	ioc_dir_to_str(_IOC_DIR(cmd)), \
	(unsigned long)_IOC_SIZE(cmd))

const char *ioctl_to_str(unsigned long cmd)
{
	switch (cmd) {
	case VIDIOC_QUERYCAP:            return "VIDIOC_QUERYCAP";
	case VIDIOC_ENUM_FMT:            return "VIDIOC_ENUM_FMT";
	case VIDIOC_G_FMT:               return "VIDIOC_G_FMT";
	case VIDIOC_S_FMT:               return "VIDIOC_S_FMT";
	case VIDIOC_STREAMON:            return "VIDIOC_STREAMON";
	case VIDIOC_STREAMOFF:           return "VIDIOC_STREAMOFF";
	case VIDIOC_REQBUFS:             return "VIDIOC_REQBUFS";
	case VIDIOC_QUERYBUF:            return "VIDIOC_QUERYBUF";
	case VIDIOC_QBUF:                return "VIDIOC_QBUF";
	case VIDIOC_DQBUF:               return "VIDIOC_DQBUF";
	case VIDIOC_EXPBUF:              return "VIDIOC_EXPBUF";
	case VIDIOC_SUBSCRIBE_EVENT:     return "VIDIOC_SUBSCRIBE_EVENT";
	case VIDIOC_UNSUBSCRIBE_EVENT:   return "VIDIOC_UNSUBSCRIBE_EVENT";
	case VIDIOC_DQEVENT:             return "VIDIOC_DQEVENT";
	default: return "UNKNOWN_IOCTL";
	}
}

const char *ioc_dir_to_str(unsigned int dir)
{
	switch (dir) {
	case _IOC_NONE:  return "NONE";
	case _IOC_READ:  return "READ";
	case _IOC_WRITE: return "WRITE";
	case (_IOC_READ | _IOC_WRITE): return "READ|WRITE";
	default: return "UNKNOWN";
	}
}


/**
 * virtio_dprx_send_r_ioctl() - Send a read-only ioctl to the device.
 * @fh: file handler of the session doing the ioctl.
 * @ioctl: ``VIDIOC_*`` ioctl code.
 * @ioctl_data: pointer to the ioctl payload.
 * @ioctl_data_len: length in bytes of the ioctl payload.
 *
 * Send an ioctl that has no driver payload, but expects a response from the
 * host (i.e. an ioctl specified with ``_IOR``).
 */

int virtio_dprx_send_r_ioctl(struct v4l2_fh *fh, u32 ioctl,
		void *ioctl_data, size_t ioctl_data_len)

{
	int rc = 0;
	struct video_device *video_dev = fh->vdev;
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *dprx_session = fh_to_session(fh);
	uint32_t req_size = sizeof(struct virtio_media_cmd_ioctl);
	void *req = kzalloc(req_size, GFP_KERNEL);
	uint32_t resp_size = sizeof(struct virtio_media_resp_ioctl) + ioctl_data_len;
	void *resp = kzalloc(resp_size, GFP_KERNEL);
	struct virtio_media_cmd_ioctl *cmd_ioctl = (struct virtio_media_cmd_ioctl *)req;
	void *ioctl_resp = resp + sizeof(struct virtio_media_resp_ioctl);

	if (!req || !resp) {
		rc = -ENOMEM;
		goto error;
	}
	PRINT_IOCTL(ioctl);
	cmd_ioctl->hdr.cmd = VIRTIO_MEDIA_CMD_IOCTL;
	cmd_ioctl->session_id = dprx_session->id;
	cmd_ioctl->code = VIRTIO_DPRX_IOCTL_CODE(ioctl);

	rc = virtio_dprx_virtq_send_command(vdprx, req, req_size, resp, resp_size);
	if(rc) {
		pr_err("virtio: ioctl 0x%x failed with rc=%d\n", ioctl, rc);
		mutex_unlock(&vdprx->vlock);
		goto error;
	}

	if (rc == 0)
		memcpy(ioctl_data, ioctl_resp, ioctl_data_len);

error:
	if (req)
		kfree(req);
	if (resp)
		kfree(resp);

	return rc;
}

/**
 * virtio_dprx_send_w_ioctl() - Send a write-only ioctl to the device.
 * @fh: file handler of the session doing the ioctl.
 * @ioctl: ``VIDIOC_*`` ioctl code.
 * @ioctl_data: pointer to the ioctl payload.
 * @ioctl_data_len: length in bytes of the ioctl payload.
 *
 * Send an ioctl that does not expect a reply beyond an error status (i.e. an
 * ioctl specified with ``_IOW``) to the host.
 */
int virtio_dprx_send_w_ioctl(struct v4l2_fh *fh, u32 ioctl,
		const void *ioctl_data,
		size_t ioctl_data_len)
{
	int rc = 0;
	struct video_device *video_dev = fh->vdev;
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *dprx_session = fh_to_session(fh);
	uint32_t req_size = sizeof(struct virtio_media_cmd_ioctl) + ioctl_data_len;
	void *req = kzalloc(req_size, GFP_KERNEL);
	uint32_t resp_size = sizeof(struct virtio_media_resp_ioctl);
	void *resp = kzalloc(resp_size, GFP_KERNEL);
	struct virtio_media_cmd_ioctl *cmd_ioctl = (struct virtio_media_cmd_ioctl *)req;
	void *ioctl_req = req + sizeof(struct virtio_media_cmd_ioctl);

	if (!req || !resp) {
		rc = -ENOMEM;
		goto error;
	}

	PRINT_IOCTL(ioctl);

	cmd_ioctl->hdr.cmd = VIRTIO_MEDIA_CMD_IOCTL;
	cmd_ioctl->session_id = dprx_session->id;
	cmd_ioctl->code = VIRTIO_DPRX_IOCTL_CODE(ioctl);

	memcpy(ioctl_req, ioctl_data, ioctl_data_len);
	rc = virtio_dprx_virtq_send_command(vdprx, req, req_size, resp, resp_size);
		if(rc) {
			pr_err("virtio: ioctl 0x%x failed with rc=%d\n", ioctl, rc);
			mutex_unlock(&vdprx->vlock);
			goto error;
		}

error:
	if (req)
		kfree(req);
	if (resp)
		kfree(resp);

	return rc;
}

/**
 * virtio_dprx_send_wr_ioctl() - Send a read-write ioctl to the device.
 * @fh: file handler of the session doing the ioctl.
 * @ioctl: ``VIDIOC_*`` ioctl code.
 * @ioctl_data: pointer to the ioctl payload.
 * @ioctl_data_len: length in bytes of the ioctl payload.
 * @minimum_resp_payload: minimum expected length of the response's payload.
 *
 * Sends an ioctl that expects a response of exactly the same size as the
 * input (i.e. an ioctl specified with ``_IOWR``) to the host.
 *
 * This corresponds to what most V4L2 ioctls do. For instance
 * ``VIDIOC_ENUM_FMT`` takes a partially-initialized ``struct v4l2_fmtdesc``
 * and returns its filled version.
 */
int virtio_dprx_send_wr_ioctl(struct v4l2_fh *fh, u32 ioctl,
		void *ioctl_data, size_t ioctl_data_len,
		size_t minimum_resp_payload)
{
	int rc = 0;
	struct video_device *video_dev = fh->vdev;
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *dprx_session = fh_to_session(fh);
	uint32_t req_size = sizeof(struct virtio_media_cmd_ioctl) + ioctl_data_len;
	void *req = kzalloc(req_size, GFP_KERNEL);
	uint32_t resp_size = sizeof(struct virtio_media_resp_ioctl) + ioctl_data_len;
	void *resp = kzalloc(resp_size, GFP_KERNEL);
	struct virtio_media_cmd_ioctl *cmd_ioctl = (struct virtio_media_cmd_ioctl *)req;
	void *ioctl_req = req + sizeof(struct virtio_media_cmd_ioctl);
	void *ioctl_resp = resp + sizeof(struct virtio_media_resp_ioctl);

	if (!req || !resp) {
		rc = -ENOMEM;
		goto error;
	}

	PRINT_IOCTL(ioctl);

	cmd_ioctl->hdr.cmd = VIRTIO_MEDIA_CMD_IOCTL;
	cmd_ioctl->session_id = dprx_session->id;
	cmd_ioctl->code = VIRTIO_DPRX_IOCTL_CODE(ioctl);

	memcpy(ioctl_req, ioctl_data, ioctl_data_len);
	rc = virtio_dprx_virtq_send_command(vdprx, req, req_size, resp, resp_size);
	if(rc) {
		pr_err("virtio: ioctl 0x%x failed with rc=%d\n", ioctl, rc);
		mutex_unlock(&vdprx->vlock);
		goto error;
	}

	if (rc == 0)
		memcpy(ioctl_data, ioctl_resp, ioctl_data_len);

error:
	if (req)
		kfree(req);
	if (resp)
		kfree(resp);
	return rc;
}

/*
 * buffer IOCTL layout
+-------------------------------------+
| struct virtio_media_cmd_ioctl       |
+-------------------------------------+
| struct v4l2_buffer                  |
+-------------------------------------+
| struct v4l2_plane for plane 0       |
| struct v4l2_plane for plane 1       |
| struct v4l2_plane for plane 2       |
+-------------------------------------+
| 16 byte UUID for plane 0            |
+-------------------------------------+
| 16 byte UUID for plane 1            |
+-------------------------------------+
| 16 byte UUID for plane 2            |
+-------------------------------------+
NO plane in the IOCTL command as MULTI planar not supported.
UUID will hold the Share memory ID.
*/
/**
 * virtio_dprx_send_buffer_ioctl() - Send an ioctl taking a buffer as
 * parameter to the device.
 * @fh: file handler of the session doing the ioctl.
 * @ioctl: ``VIDIOC_*`` ioctl code.
 * @b: ``v4l2_buffer`` to be sent as the ioctl payload.
 *
 * Buffers can require an additional descriptor to send their planes array, and
 * can have pointers to userspace memory hence this dedicated function.
 */

#define UUID_SIZE 16

static int virtio_dprx_send_buffer_ioctl(struct v4l2_fh *fh, u32 ioctl,
		struct v4l2_buffer *b)
{
	int rc = 0;
	struct video_device *video_dev = fh->vdev;
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *session = fh_to_session(fh);
	struct virtio_dprx_queue_state *queue = &session->queues[b->type];
	struct virtio_dprx_buffer *buffer = &queue->buffers[b->index];
	uint32_t cmd_size = sizeof(struct virtio_media_cmd_ioctl) +
        	            sizeof(struct v4l2_buffer) + UUID_SIZE;
	struct virtio_media_cmd_ioctl *cmd_ioctl =kzalloc(cmd_size, GFP_KERNEL);

	if (!cmd_ioctl)
		return -ENOMEM;

	void *resp = kzalloc(sizeof(struct virtio_media_resp_ioctl), GFP_KERNEL);
	if (!resp) {
		kfree(cmd_ioctl);
		return -ENOMEM;
	}

	PRINT_IOCTL(ioctl);

	char *uuid = (char *)cmd_ioctl + sizeof(struct virtio_media_cmd_ioctl) + sizeof(struct v4l2_buffer);

	cmd_ioctl->hdr.cmd = VIRTIO_MEDIA_CMD_IOCTL;
	cmd_ioctl->session_id = session->id;
	cmd_ioctl->code = VIRTIO_DPRX_IOCTL_CODE(ioctl);

	memcpy((char *)cmd_ioctl + sizeof(struct virtio_media_cmd_ioctl), b,
				sizeof(struct v4l2_buffer));

	memcpy(uuid, &buffer->shmem_id, 4);

	rc = virtio_dprx_virtq_send_command(vdprx, cmd_ioctl, cmd_size, resp, sizeof(struct virtio_media_resp_ioctl));
	if (rc)
		printk("virtio: ioctl 0x%x failed with rc=%d\n", ioctl, rc);

	kfree(cmd_ioctl);
	kfree(resp);

	return rc;
}

/*
 * Macros suitable for defining ioctls with a constant size payload.
 */

#define DPRX_WR_IOCTL(name, ioctl, payload_t)                       \
	static int virtio_dprx_##name(struct file *file, void *fh,   \
				       payload_t *payload)            \
	{                                                             \
		return virtio_dprx_send_wr_ioctl(fh, ioctl, payload, \
						  sizeof(*payload),   \
						  sizeof(*payload));  \
	}
#define DPRX_R_IOCTL(name, ioctl, payload_t)                       \
	static int virtio_dprx_##name(struct file *file, void *fh,  \
				       payload_t *payload)           \
	{                                                            \
		return virtio_dprx_send_r_ioctl(fh, ioctl, payload, \
						 sizeof(*payload));  \
	}
#define DPRX_W_IOCTL(name, ioctl, payload_t)                       \
	static int virtio_dprx_##name(struct file *file, void *fh,  \
				       payload_t *payload)           \
	{                                                            \
		return virtio_dprx_send_w_ioctl(fh, ioctl, payload, \
						 sizeof(*payload));  \
	}

/*
 * V4L2 ioctl handlers.
 *
 * Most of these functions just forward the ioctl to the host, for these we can
 * use one of the SIMPLE_*_IOCTL macros. Exceptions that have their own
 * standalone function follow.
 */

//DPRX_WR_IOCTL(enum_fmt, VIDIOC_ENUM_FMT, struct v4l2_fmtdesc)
DPRX_WR_IOCTL(g_fmt, VIDIOC_G_FMT, struct v4l2_format)
DPRX_WR_IOCTL(s_fmt, VIDIOC_S_FMT, struct v4l2_format)

static char *event_id_to_string(uint32_t type)
{
	if (type == V4L2_EVENT_SOURCE_CHANGE)
		return "V4L2_EVENT_SOURCE_CHANGE";
	if (type == V4L2_EVENT_PRIVATE_START)
		return "V4L2_EVENT_PRIVATE_START";

	return "UNKNOWN";
}
/*
 * Subscribe/unsubscribe from an event.
 */

static int virtio_dprx_subscribe_event(struct v4l2_fh *fh,
		const struct v4l2_event_subscription *sub)
{
	//struct video_device *video_dev = fh->vdev;
	//struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	int ret;
	pr_info("Event subscribed  %s\n", event_id_to_string(sub->type));
	/* First subscribe to the event in the guest. */
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		ret = v4l2_src_change_event_subscribe(fh, sub);
		break;
	default:
		ret = v4l2_event_subscribe(fh, sub, 1, NULL);
		break;
	}
	if (ret)
		return ret;

	/*
	 * Subscribing to an event may result in that event being signaled
	 * immediately. Process all pending events to make sure we don't miss it.
	 */
//	if (sub->flags & V4L2_EVENT_SUB_FL_SEND_INITIAL)
//		virtio_dprx_process_events(vdprx);

	return 0;
}

static int virtio_dprx_unsubscribe_event(struct v4l2_fh *fh,
		const struct v4l2_event_subscription *sub)
{
	int ret;

	pr_info("Event unsubscribed %s\n", event_id_to_string(sub->type));

	ret = v4l2_event_unsubscribe(fh, sub);
	if (ret)
		return ret;

	return 0;
}

static void virtio_dprx_clear_queue( struct virtual_dprx_dev *vdprx,
		struct virtio_dprx_session *session,
		struct virtio_dprx_queue_state *queue)
{
	struct list_head *p, *n;
	int i;
	pr_info("virtio_dprx_clear_queue called \n");
	mutex_lock(&session->dqbufs_lock);

	list_for_each_safe(p, n, &queue->pending_dqbufs) {
		struct virtio_dprx_buffer *dqbuf =
			list_entry(p, struct virtio_dprx_buffer, list);

		list_del(&dqbuf->list);
	}

	mutex_unlock(&session->dqbufs_lock);

	pr_info("virtio_dprx_clear_queue called list del\n");
	/* All buffers are now dequeued. */
	atomic_set(&queue->pending_cnt, 0);
	for (i = 0; i < queue->allocated_bufs; i++) {
		queue->buffers[i].buffer.flags = 0;
		virtio_dprx_unexport_memory(vdprx, &queue->buffers[i]);
	}

	queue->queued_bufs = 0;
	queue->streaming = false;
	queue->is_capture_last = false;
	pr_info("virtio_dprx_clear_queue done \n");
}

/*
 * Streamon/off affect the local queue state.
 */

static int virtio_dprx_streamon(struct file *file, void *fh,
		enum v4l2_buf_type i)
{
	struct virtio_dprx_session *session = fh_to_session(fh);
	int ret;

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (session->queues[i].queued_bufs == 0) {
		pr_err("STREAMON called with no buffers queued");
		return -EINVAL;
	}

	ret = virtio_dprx_send_w_ioctl(fh, VIDIOC_STREAMON, &i, sizeof(i));
	if (ret < 0)
		return ret;

	session->queues[i].streaming = true;

	return 0;
}

static int virtio_dprx_streamoff(struct file *file, void *fh,
		enum v4l2_buf_type i)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *dprx_session = fh_to_session(fh);
	int ret;

	if (i != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	ret = virtio_dprx_send_w_ioctl(fh, VIDIOC_STREAMOFF, &i, sizeof(i));
	if (ret < 0)
		return ret;

	virtio_dprx_clear_queue(vdprx, dprx_session, &dprx_session->queues[i]);

	return 0;
}

/*
 * Buffer creation/queuing functions deal with the local driver state.
 */

static int virtio_dprx_reqbufs(struct file *file, void *fh,
		struct v4l2_requestbuffers *b)
{
	struct virtio_dprx_session *dprx_session = fh_to_session(fh);
	struct virtio_dprx_queue_state *queue;
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;


	if (b->memory != V4L2_MEMORY_DMABUF)
		return -EINVAL;

	ret = virtio_dprx_send_wr_ioctl(fh, VIDIOC_REQBUFS, b, sizeof(*b),
					 sizeof(*b));
	if (ret)
		return ret;

	queue = &dprx_session->queues[b->type];

	/* REQBUFS(0) is an implicit STREAMOFF. */
	if (b->count == 0) {
		return 0;
	}

	vfree(queue->buffers);
	queue->buffers = NULL;

	if (b->count > 0) {
		queue->buffers =
			vzalloc(sizeof(struct virtio_dprx_buffer) * b->count);
		if (!queue->buffers)
			return -ENOMEM;
	}

	queue->allocated_bufs = b->count;

	b->capabilities |= V4L2_BUF_CAP_SUPPORTS_DMABUF;

	return 0;
}

static int virtio_dprx_querybuf(struct file *file, void *fh,
		struct v4l2_buffer *b)
{
	struct virtio_dprx_session *session = fh_to_session(fh);
	struct virtio_dprx_queue_state *queue;
	struct virtio_dprx_buffer *buffer;
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	ret = virtio_dprx_send_buffer_ioctl(fh, VIDIOC_QUERYBUF, b);
	if (ret)
		return ret;

	queue = &session->queues[b->type];
	if (b->index >= queue->allocated_bufs)
		return -EINVAL;

	buffer = &queue->buffers[b->index];
	/* Set the DONE flag if the buffer is waiting in our own dequeue queue. */
	b->flags |= (buffer->buffer.flags & V4L2_BUF_FLAG_DONE);

	return 0;
}
//Needed only if application want to add more or change format after the QBUF
#ifdef V4L2_CREATE_BUFFER
static int virtio_dprx_create_bufs(struct file *file, void *fh,
		struct v4l2_create_buffers *b)
{
	struct virtio_dprx_session *session = fh_to_session(fh);
	struct virtio_dprx_queue_state *queue;
	struct virtio_dprx_buffer *buffers;
	u32 type = b->format.type;
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	queue = &session->queues[type];

	ret = virtio_dprx_send_wr_ioctl(fh, VIDIOC_CREATE_BUFS, b, sizeof(*b),
					 sizeof(*b));
	if (ret)
		return ret;

	/* If count is zero, we were just checking for format. */
	if (b->count == 0)
		return 0;

	buffers = queue->buffers;

	queue->buffers =
		vzalloc(sizeof(*queue->buffers) * (b->index + b->count));
	if (!queue->buffers) {
		queue->buffers = buffers;
		return -ENOMEM;
	}

	memcpy(queue->buffers, buffers,
	       sizeof(*buffers) * queue->allocated_bufs);
	vfree(buffers);

	queue->allocated_bufs = b->index + b->count;

	return 0;
}

static int virtio_dprx_prepare_buf(struct file *file, void *fh,
		struct v4l2_buffer *b)
{
	struct virtio_dprx_session *session = fh_to_session(fh);
	struct virtio_dprx_queue_state *queue;
	struct virtio_dprx_buffer *buffer;
	int i, ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	queue = &session->queues[b->type];
  	if (b->index >= queue->allocated_bufs)
  		return -EINVAL;

	buffer = &queue->buffers[b->index];
 
	buffer->buffer.m = b->m;

	ret = dma_buf_vmap(buffer->dbuf, &map);
	if (ret) {
		pr_err("Failed to vmap DMA buffer\n");
		dma_buf_put(buffer->dbuf);
		return -ENOMEM;
	}

	buffer->vaddr = map.vaddr;

	//Buffer is already exported
	if (!buffer->shmem_id) {
		ret = virtio_dprx_export_memory(vdprx, buffer);
		if (ret) {
			printk("buffer memory export failed \n");
			dma_buf_put(buffer->dbuf);
			return ret;
		}
	}

	ret = virtio_media_send_buffer_ioctl(fh, VIDIOC_PREPARE_BUF, b);
	if (ret)
		return ret;

	buffer->buffer.flags = V4L2_BUF_FLAG_PREPARED;

	return 0;
}
void dump_data(void *data, uint32_t size)
{
	if (!data)
		return;

	char *c = (char *)data;
	char line[128]; // Enough for 8 bytes + spaces + newline
	int pos = 0;

	for (uint32_t i = 0; i < size; i++) {
		pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", (unsigned char)c[i]);

		if ((i % 8) == 7 || i == size - 1) {
			printk("%s\n", line);
			pos = 0; // Reset for next line
		}
	}
}

#endif
static int virtio_dprx_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *session = fh_to_session(fh);
	struct virtio_dprx_queue_state *queue;
	struct virtio_dprx_buffer *buffer;
	bool prepared;
	u32 old_flags;
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (V4L2_TYPE_IS_MULTIPLANAR(b->type)) {
		return -EINVAL;
	}
	queue = &session->queues[b->type];

	if (b->index >= queue->allocated_bufs)
		return -EINVAL;

	if (b->length == 0) {
		pr_err(" Buffer not allocated");
		return -EINVAL;
	}

	buffer = &queue->buffers[b->index];
	prepared = buffer->buffer.flags & V4L2_BUF_FLAG_PREPARED;
	/*
	 * Store the buffer and plane `m` information so we can retrieve it again
	 * when DQBUF occurs.
	 */
	if (!prepared ) {
		buffer->buffer.m = b->m;
		buffer->buffer.length = b->length;
		if (b->memory == V4L2_MEMORY_DMABUF) {
			buffer->dbuf = dma_buf_get(b->m.fd);
		}
	}

	//Buffer is already exported
	if (!buffer->shmem_id) {
		ret = virtio_dprx_export_memory(vdprx, buffer);
		if (ret) {
			pr_err(" virtio_dprx_qbuf : buffer memory export failed \n");
			dma_buf_put(buffer->dbuf);
			return ret;
		}
	}

	pr_debug(" virtio_dprx_qbuf shmem_id %d", buffer->shmem_id);

	mutex_lock(&session->dqbufs_lock);
	old_flags = buffer->buffer.flags;
	buffer->buffer.flags &= ~(V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_ERROR);
	buffer->buffer.flags |= (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_PREPARED);
	queue->queued_bufs++;
	mutex_unlock(&session->dqbufs_lock);


	ret = virtio_dprx_send_buffer_ioctl(fh, VIDIOC_QBUF, b);
	if (ret) {
		/* Rollback the previous flags as the buffer is not queued. */
		mutex_lock(&session->dqbufs_lock);
		buffer->buffer.flags = old_flags;
		if (queue->queued_bufs > 0)
			queue->queued_bufs--;
		mutex_unlock(&session->dqbufs_lock);
		return ret;
	}

	queue->queued_bufs += 1;
	return 0;
}

static int virtio_dprx_dqbuf(struct file *file, void *fh,
			      struct v4l2_buffer *b)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx  = to_virtio_dprx(video_dev);
	struct virtio_dprx_session *session = fh_to_session(file->private_data);
	struct virtio_dprx_buffer *dqbuf;
	struct virtio_dprx_queue_state *queue;
	struct list_head *buffer_queue;
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	queue = &session->queues[b->type];

	/*
	 * If a buffer with the LAST flag has been returned, subsequent calls to DQBUF
	 * must return -EPIPE until the queue is cleared.
	 */
	if (queue->is_capture_last)
		return -EPIPE;

	buffer_queue = &queue->pending_dqbufs;
	if (session->nonblocking_dequeue) {
		if (atomic_read(&queue->pending_cnt) == 0)
			return -EAGAIN;
	} else if (queue->allocated_bufs == 0) {
		return -EINVAL;
	} else if (!queue->streaming) {
		return -EINVAL;
	}

	PRINT_IOCTL(VIDIOC_DQBUF);
	/*
	 * vd->lock has been acquired by virtio_dprx_device_ioctl. Release it
	 * while we want to other ioctls for this session can be processed and
	 * potentially trigger dqbuf_wait.
	 */
	mutex_unlock(&vdprx->vlock);
	ret = wait_event_interruptible(session->dqbuf_wait,
			atomic_read(&queue->pending_cnt) > 0);
	if (ret)
		goto relock_vlock_eintr;

	/* Pop exactly one buffer from the pending list under the producer's lock */
	mutex_lock(&session->dqbufs_lock);
	if (list_empty(buffer_queue)) {
		mutex_unlock(&session->dqbufs_lock);
		/* Rare race: condition flipped between wake and pop. */
		goto relock_vlock_eagain;
	}
	dqbuf = list_first_entry(buffer_queue, struct virtio_dprx_buffer,
			list);
	list_del(&dqbuf->list);
	atomic_dec(&queue->pending_cnt);
	mutex_unlock(&session->dqbufs_lock);

	/* Reacquire vlock after list ops to avoid lock ordering issues */
	mutex_lock(&vdprx->vlock);

	/* Clear the DONE flag as the buffer is now being dequeued. */
	dqbuf->buffer.flags &= ~V4L2_BUF_FLAG_DONE;

	memcpy(b, &dqbuf->buffer, sizeof(*b));

	if (V4L2_TYPE_IS_CAPTURE(b->type) && b->flags & V4L2_BUF_FLAG_LAST)
		queue->is_capture_last = true;

	return 0;

relock_vlock_eagain:
	mutex_lock(&vdprx->vlock);
	return -EAGAIN;
relock_vlock_eintr:
	mutex_lock(&vdprx->vlock);
	return -EINTR;
}

static int virtio_dprx_enum_fmt_vid_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	switch (f->index) {
	case 0:
		f->pixelformat = PIXEL_FORMAT_RGB24;
		strscpy(f->description, "RGB24", sizeof(f->description));
		break;
	case 1:
		f->pixelformat = PIXEL_FORMAT_RGB101010;
		strscpy(f->description, "RGB101010", sizeof(f->description));
		break;
	case 2:
		f->pixelformat = PIXEL_FORMAT_RGB888_UBWC;
		strscpy(f->description, "RGB888 UBWC", sizeof(f->description));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int virtio_dprx_g_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	int ret = 0;

	if(f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pr_info("virtio_dprx_g_fmt_vid_cap for %d", f->type);

	ret = virtio_dprx_g_fmt(file, priv, f);
	if (ret)
		return ret;
	pr_info("pixel %x width %d height %d, byterperline %d",
			f->fmt.pix.pixelformat,
			f->fmt.pix.width,
			f->fmt.pix.height,
			3 * f->fmt.pix.width);
	return 0;
}

static int virtio_dprx_s_fmt_vid_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx  = to_virtio_dprx(video_dev);
	int ret = 0;

	if (f->fmt.pix.pixelformat != PIXEL_FORMAT_RGB24 &&
		f->fmt.pix.pixelformat != PIXEL_FORMAT_RGB101010 &&
		f->fmt.pix.pixelformat != PIXEL_FORMAT_RGB888_UBWC)
		return -EINVAL;

	vdprx->format = *f;

	if (f->fmt.pix.pixelformat == PIXEL_FORMAT_RGB24) {
		vdprx->format.fmt.pix.bytesperline = f->fmt.pix.width * 3;
		vdprx->format.fmt.pix.sizeimage = f->fmt.pix.width * f->fmt.pix.height * 3;
	} else if (f->fmt.pix.pixelformat == PIXEL_FORMAT_RGB101010) {
		vdprx->format.fmt.pix.bytesperline = f->fmt.pix.width * 4;
		vdprx->format.fmt.pix.sizeimage = f->fmt.pix.width * f->fmt.pix.height * 4;
	} else if (f->fmt.pix.pixelformat == PIXEL_FORMAT_RGB888_UBWC) {
		vdprx->format.fmt.pix.bytesperline = f->fmt.pix.width * 3;
		vdprx->format.fmt.pix.sizeimage = (f->fmt.pix.width * f->fmt.pix.height * 3) * 3 / 4;
	}

	ret = virtio_dprx_s_fmt(file, priv, &vdprx->format);

	return ret;
}

static int virtio_dprx_querycap(struct file *file, void *priv,
		struct v4l2_capability *cap)
{
	struct video_device *video_dev = video_devdata(file);

	strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, DRIVER_NAME, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DPRX_NAME, sizeof(cap->bus_info));
	cap->device_caps = video_dev->device_caps;
	cap->capabilities = video_dev->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

const struct v4l2_ioctl_ops virtio_dprx_ioctl_ops = {
	/* VIDIOC_QUERYCAP handler */
	.vidioc_querycap = virtio_dprx_querycap,

	/* VIDIOC_ENUM_FMT handlers */
	.vidioc_enum_fmt_vid_cap = virtio_dprx_enum_fmt_vid_cap,

	/* VIDIOC_G_FMT handlers */
	.vidioc_g_fmt_vid_cap = virtio_dprx_g_fmt_vid_cap,

	/* VIDIOC_S_FMT handlers */
	.vidioc_s_fmt_vid_cap = virtio_dprx_s_fmt_vid_cap,
	/* Stream on/off */
	.vidioc_streamon = virtio_dprx_streamon,
	.vidioc_streamoff = virtio_dprx_streamoff,
	/* Buffer handlers */
	.vidioc_reqbufs = virtio_dprx_reqbufs,
	.vidioc_querybuf = virtio_dprx_querybuf,
	.vidioc_qbuf = virtio_dprx_qbuf,
	.vidioc_expbuf = NULL,
	.vidioc_dqbuf = virtio_dprx_dqbuf,
#ifdef V4L2_CREATE_BUFFER
	.vidioc_create_bufs = virtio_dprx_create_bufs,
#endif
	.vidioc_subscribe_event = virtio_dprx_subscribe_event,
	.vidioc_unsubscribe_event = virtio_dprx_unsubscribe_event,
};

long virtio_dprx_device_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtual_dprx_dev *vdprx = to_virtio_dprx(video_dev);
	struct v4l2_fh *vfh = NULL;
	int ret;

	if (test_bit(V4L2_FL_USES_V4L2_FH, &video_dev->flags))
		vfh = file->private_data;

	mutex_lock(&vdprx->vlock);

	ret = video_ioctl2(file, cmd, arg);

	mutex_unlock(&vdprx->vlock);

	vdprx_trace_ioctl_record(vdprx, cmd, ret);
	return ret;
}
