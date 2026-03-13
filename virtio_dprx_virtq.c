/*Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *SPDX-License-Identifier: GPL-2.0-only  */

#define pr_fmt(fmt)	"[virtio_dprx:%s:%d] " fmt, __func__, __LINE__
#include "virtio_dprx.h"

#define VIRQ_SHMEM_SIZE        4096
int virtio_dprx_virtq_open(struct virtual_dprx_dev *vdprx, uint32_t cellindex)
{
	int ret = 0;

	if (!vdprx) {
		printk("virtio : dev NULL\n");
		ret = -EINVAL;
		goto exit;
	}

	ret = habmm_socket_open(
			&vdprx->hab_socket_cmd,
			vdprx->mmid_cmd | (cellindex << 16),
			-1,
			0);
	if (ret) {
		pr_err("hab open failed mmid %d ret %d\n", vdprx->mmid_cmd, ret);
		goto exit;
	}

	ret = habmm_socket_open(
			&vdprx->hab_socket_event,
			vdprx->mmid_event | (cellindex << 16),
			-1,
			0);
	if (ret) {
		habmm_socket_close(vdprx->hab_socket_cmd);
		pr_err("hab open failed mmid %d ret %d\n", vdprx->mmid_event, ret);
	}

exit:
	return ret;
}


int virtio_dprx_virtq_close (struct virtual_dprx_dev *vdprx)
{
	 habmm_socket_close(vdprx->hab_socket_cmd);
	 habmm_socket_close(vdprx->hab_socket_event);
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

int virtio_dprx_virtq_send_command(struct virtual_dprx_dev *vdprx,
		void *req,
		uint32_t req_size,
		void *resp,
		uint32_t resp_size)
{
	int rc = 0;
	uint32_t flags = HABMM_SOCKET_RECV_FLAGS_TIMEOUT;
	uint32_t size = resp_size;
	uint32_t max_retries = 10;
	uint32_t hab_socket =vdprx->hab_socket_cmd;
	struct virtio_media_resp_header *resp_header;

	if (!req || !resp) {
		printk("req or resp failed");
		rc = -EINVAL;
		goto end;
	}
	pr_debug("habmm_socket_send send on %x size %d\n", hab_socket, req_size);

	rc = habmm_socket_send(hab_socket, req, req_size, 0x00);
	if (rc) {
		pr_err("virtio :habmm_socket_send failed <%d>\n", rc);
		goto end;
	}

retry:
	size = resp_size;
	rc = habmm_socket_recv(hab_socket, resp, &size,	2500, flags);
		if (rc && max_retries) {
			max_retries--;
			pr_info("virtio : recv timout retry %d\n", max_retries);
			goto retry;
		}
		else if (rc && !max_retries) {
			size = resp_size;
			pr_err("virtio : retries done waiting for reply\n");
			rc = habmm_socket_recv(hab_socket, resp, &size, (uint32_t)-1, 0);
			if (rc)
				pr_err("virtio :socket_recv failed <%d>\n",rc);
		}

		if (!rc) {
			resp_header = (struct virtio_media_resp_header *)resp;
			if (resp_header->status) {
				pr_err("command failed %dsize %d ", resp_header->status, size);
				dump_data(resp, size);
				/* Host returns a positive error code. */
				rc = -resp_header->status;
			}
		}
end:
	return rc;
}

#define VIRTIO_DPRX_CMD_ENABLE_VIRQ 0x1004

struct virtio_dprx_enable_virq {
	struct virtio_media_cmd_header hdr;
	u32 device_id;
	u32 shmem_id;
	u32 shmem_size;
	u32 padding;
};

struct virtio_dprx_resp_enable_virq {
	struct virtio_media_resp_header hdr;
	uint32_t shmem_id;
	u32 padding;
};

int virtio_dprx_send_shmem(struct virtual_dprx_dev *vdprx, bool enable)
{
	int ret = 0;
	int32_t hab_socket = vdprx->hab_socket_cmd;
	uint32_t size = sizeof(struct virtio_dprx_enable_virq);
	struct virtio_dprx_enable_virq *cmd_p =
		kzalloc(size, GFP_KERNEL);
	uint32_t resp_size = sizeof(struct virtio_dprx_resp_enable_virq);
	struct virtio_dprx_resp_enable_virq *resp =
		kzalloc(resp_size, GFP_KERNEL);

	cmd_p->hdr.cmd = cpu_to_le32(VIRTIO_DPRX_CMD_ENABLE_VIRQ);
	cmd_p->shmem_id = cpu_to_le32(vdprx->virq_shmem->hab_export_id);
	cmd_p->shmem_size = enable ? cpu_to_le32(vdprx->virq_shmem->size) : 0;
	cmd_p->device_id = cpu_to_le32(vdprx->device_id);
	ret = habmm_socket_send(hab_socket, cmd_p, size, 0x00);
	if (ret) {
		pr_err("virtio :habmm_socket_send failed <%d>\n", ret);
		ret = -1;
		goto end;
	}
	if (!resp)
		goto end;

	ret = habmm_socket_recv(hab_socket, resp, &size, -1, HABMM_SOCKET_RECV_FLAGS_NON_BLOCKING);
	if (ret) {
		pr_err("virtio : socket receive failed %d\n", ret);
		goto end;
	}
	if (resp->hdr.status)
		ret = -resp->hdr.status;
end:
	kfree(cmd_p);
	kfree(resp);
	return ret;
}

int virtio_dprx_create_shmem(struct device *dev, struct virtual_dprx_dev *vdprx, uint32_t device_id)
{
	int rc = -1;
	int32_t hab_socket = vdprx->hab_socket_cmd;
	struct virq_shmem_t *virq_shmem = &(vdprx->virq_shmem[device_id]);

	if (NULL != virq_shmem->vaddr) {
		pr_err("virq shmem already initialized for device %d\n", device_id);
		return -EINVAL;
	}

	rc = virtio_dprx_hab_register_virq(vdprx, device_id);
	if(rc) {
		pr_err("virtual Irq failed \n");
		return -EINVAL;
	}

	virq_shmem->vaddr = dma_alloc_coherent(dev, VIRQ_SHMEM_SIZE, &virq_shmem->dma_handle,
							GFP_KERNEL);

	if (virq_shmem->vaddr == NULL || virq_shmem->dma_handle < 0) {
		pr_err("error allocating memory for virq for device %d\n", device_id);
		return -ENOMEM;
	}

	pr_info("virq_shmem vaddr %p for device %d\n", virq_shmem->vaddr, device_id);

	virq_shmem->size = VIRQ_SHMEM_SIZE;
	memset(virq_shmem->vaddr, 0, virq_shmem->size);

	rc = habmm_export(
			hab_socket,
			virq_shmem->vaddr,
			virq_shmem->size,
			&virq_shmem->hab_export_id,
			0);

	if (rc) {
		pr_err("virq_shmem habmm export failed\n");
		dma_free_coherent(dev, virq_shmem->size, virq_shmem->vaddr, virq_shmem->dma_handle);
		virq_shmem->vaddr = NULL;
		virq_shmem->dma_handle = 0;
		virq_shmem->size = 0;
		virq_shmem->hab_export_id = 0;
	}

	rc = virtio_dprx_send_shmem(vdprx, true);

	return rc;
}

void virtio_dprx_destroy_shmem(struct device *dev, struct virtual_dprx_dev *vdprx, uint32_t device_id)
{
	int rc = -1;
	int32_t hab_socket = vdprx->hab_socket_cmd;
	struct virq_shmem_t *virq_shmem = &(vdprx->virq_shmem[device_id]);

	if (NULL == virq_shmem->vaddr) {
		pr_err("virq shmem not initialized for device %d\n", device_id);
		return;
	}

	rc = virtio_dprx_send_shmem(vdprx, false);
	if (rc) {
		pr_err(" virtio_dprx_send_shmem %d failedi %d\n", device_id, rc);
	}

	rc = habmm_unexport(hab_socket, virq_shmem->hab_export_id, 0);
	if (rc) {
		pr_err("virq_shmem habmm unexport for device %d failed\n", device_id);
	}

	pr_info("virq_shmem habmm unexport for device %d successful, export id %d\n",
						device_id, virq_shmem->hab_export_id);

	dma_free_coherent(dev, virq_shmem->size, virq_shmem->vaddr, virq_shmem->dma_handle);

	virq_shmem->vaddr = NULL;
	virq_shmem->dma_handle = 0;
	virq_shmem->size = 0;
	virq_shmem->hab_export_id = 0;
}

int virtio_dprx_hab_virq_cb(int irq, void *irq_data, uint32_t flags)
{
	struct virq_info_t *virq_info = (struct virq_info_t *) irq_data;
	struct virtual_dprx_dev *vdprx = (struct virtual_dprx_dev *)virq_info->vdprx;

	pr_info("Doorbell received for dbl_handle %d\n", virq_info->dbl_handle);

	schedule_work(&vdprx->eventq_work);

	return 0;
}

int virtio_dprx_hab_register_virq(struct virtual_dprx_dev *vdprx, uint32_t device_id)
{
	int32_t dbl_handle = -1;
	const uint32_t pvm_hab_vmid = 0x0;
	uint32_t virq_id[MAX_DPRX_DEVICES] = {3003,3004}; /* dprx id as defined by HAB */
	int ret = -1;
	struct virq_info_t *virq_info = &vdprx->virq_info[device_id];

	virq_info->dbl_handle = -1;

	ret = habmm_virq_register(&dbl_handle,
		pvm_hab_vmid,
		virq_id[device_id],
		virtio_dprx_hab_virq_cb,
		virq_info,
		HABMM_VIRQ_FLAGS_RX);
	if (ret != 0)
	{
		pr_err("Error registering for doorbell %d. Error Code %d\n", virq_id[device_id], ret);
		return ret;
	}

	virq_info->vdprx = (void *)vdprx;
	virq_info->dbl_handle = dbl_handle;
	virq_info->device_id = device_id;

	return 0;
}

void virtio_dprx_hab_unregister_virq(struct virtual_dprx_dev *vdprx, uint32_t device_id)
{
	int ret = -1;
	struct virq_info_t *virq_info = &vdprx->virq_info[device_id];

	if(virq_info == NULL) {
		return;
	}

	ret = habmm_virq_unregister(virq_info->dbl_handle, HABMM_VIRQ_FLAGS_RX);
	if (ret != 0)
	{
		pr_err("Error un-registering for doorbell %d. Error Code %d\n", virq_info->dbl_handle, ret);
	}

	virq_info->vdprx = NULL;
	virq_info->dbl_handle = -1;

	return;
}

int virtio_dprx_export_memory(struct virtual_dprx_dev *vdprx, struct virtio_dprx_buffer *buffer)
{
	int ret = -1;
	struct virtio_mem_info *mem = &buffer->mem;
	uint32_t export_id = 0;
	uint32_t export_flags = 0;

	memset((char *)mem, 0x00, sizeof(struct virtio_mem_info));
	pr_info("virtio_dprx_export_memory called ");
	mem->size = buffer->buffer.length; //Need to check the buffer size.
	mem->buffer = (void *)buffer->dbuf;
	export_flags |= HABMM_EXPIMP_FLAGS_DMABUF;
	ret = habmm_export(vdprx->hab_socket_cmd, mem->buffer, (uint32_t)mem->size, &export_id, export_flags);

	if (ret) {
		pr_err("virtio : export failed with ret=%d, size=%u\n", ret, mem->size);
		goto error;
	}

	buffer->shmem_id = export_id;

error:
	return ret;
}

int virtio_dprx_unexport_memory(struct virtual_dprx_dev *vdprx, struct virtio_dprx_buffer *buffer)
{
	int rc = -1;

	rc = habmm_unexport(vdprx->hab_socket_cmd,  buffer->shmem_id, HABMM_EXPIMP_FLAGS_DMABUF);
	if (rc)
		pr_err("habmm_unexport(id=%u) failed", buffer->shmem_id);
	return rc;
}
