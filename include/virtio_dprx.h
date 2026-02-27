/*Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *SPDX-License-Identifier: GPL-2.0-only */
#ifndef __VIRTIO_DPRX_H
#define __VIRTIO_DPRX_H
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/atomic.h>
#include <linux/dma-buf.h>
#include <linux/habmm.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>

#include "virtio_media.h"

#define DRIVER_NAME "v4l2_virtual_dprx"
#define MAX_DPRX_DEVICES 2

#define PIXEL_FORMAT_RGB24 V4L2_PIX_FMT_RGB24
#define PIXEL_FORMAT_RGB101010 v4l2_fourcc('R', '1', '0', ' ') // Define RGB101010 format
#define PIXEL_FORMAT_RGB888_UBWC v4l2_fourcc('Q', 'R', 'U', '3') // RGB888 UBWC format

struct virtual_dprx_dev;
extern const struct v4l2_ioctl_ops virtio_dprx_ioctl_ops;

struct virq_shmem_t {
	dma_addr_t dma_handle;
	void *vaddr;
	size_t size;
	uint32_t hab_export_id;
};

struct virq_info_t {
	void *vdprx;
	uint32_t device_id;
	int32_t dbl_handle;
};

struct virtio_mem_info {
	void *buffer;
	uint32_t size;
	uint64_t shmem_id;
};

struct virtio_dprx_buffer {
	struct v4l2_buffer buffer;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct virtio_mem_info mem;
	uint32_t shmem_id;
	struct dma_buf *dbuf;
	void *vaddr;
	struct list_head list;
};

struct virtio_dprx_queue_state {
	bool streaming;
	size_t allocated_bufs;
	bool is_capture_last;
	struct virtio_dprx_buffer *buffers;
	size_t queued_bufs;
	struct list_head pending_dqbufs;
	atomic_t pending_cnt;
};

struct virtio_dprx_session {
	struct v4l2_fh fh;
	uint32_t id;
	bool nonblocking_dequeue;
	struct mutex queues_lock;
	struct virtio_dprx_queue_state queues[V4L2_BUF_TYPE_META_OUTPUT + 1];
	struct mutex dqbufs_lock;
	wait_queue_head_t dqbuf_wait;
	struct list_head list;
};

#define VDP_RX_TRACE_RING_SZ   1024

enum vdprx_trace_type {
    VTP_IOCTL = 1,
    VTP_EVENT = 2,
};

struct vdprx_trace_rec {
    u64 ts_ns;
    u64 seq;
    u16 type;       /* vdprx_trace_type */
    u16 cpu;
    u32 pid;

    union {
        struct {
            unsigned long cmd_raw;
            unsigned int  cmd_type; /* _IOC_TYPE */
            unsigned int  cmd_nr;   /* _IOC_NR   */
            unsigned int  cmd_dir;  /* _IOC_DIR  */
            unsigned int  cmd_size; /* _IOC_SIZE */
            long          ret;      /* ioctl return */
        } ioctl;

        struct {
            u32 media_evt;    /* virtio_media_event_header.event or V4L2 type */
            u32 session_id;   /* as seen in events */
            u32 extra;        /* optional */
        } evt;
    } u;
};

struct vdprx_trace {
    struct vdprx_trace_rec *buf;
    unsigned int size;   /* ring size */
    unsigned int head;   /* next write index */
    spinlock_t   lock;
    wait_queue_head_t wq;
    atomic64_t   seq;    /* monotonically increasing seqno */
    bool         enabled;
};


struct virtual_dprx_dev {
	uint32_t device_id;
	struct v4l2_device v4l2_dev;
	struct video_device video_dev;
	struct mutex vlock;
	struct v4l2_format format;
	int32_t hab_socket_cmd;
	int32_t hab_socket_event;
	struct list_head sessions;
	struct mutex sessions_lock;
	struct mutex events_lock;
	wait_queue_head_t workq;
	struct work_struct eventq_work;
	uint32_t mmid_cmd;
	uint32_t mmid_event;
	struct virq_shmem_t virq_shmem[MAX_DPRX_DEVICES];
	struct virq_info_t virq_info[MAX_DPRX_DEVICES];
	char sysfs_value[64];
	uint32_t property_id;
	bool stop;
	uint32_t cell_index;

	struct dentry          *dbg_root;      /* /sys/kernel/debug/vdprx/cardX */
	struct dentry          *dbg_dir_card;  /* vdprx/cardX */

	struct vdprx_trace tr_all;

	/* specific debugfs nodes for tracing */
	struct dentry *dbg_trace_enable;
	struct dentry *dbg_trace_clear;
	struct dentry *dbg_trace_all;
	struct dentry *dbg_trace_all_live;
};

static inline struct virtio_dprx_session *fh_to_session(struct v4l2_fh *fh)
{
	return container_of(fh, struct virtio_dprx_session, fh);
}
static inline struct virtual_dprx_dev *to_virtio_dprx(struct video_device *video_dev)
{
	return container_of(video_dev, struct virtual_dprx_dev, video_dev);
}

struct virtio_media_event_header *virtio_dprx_get_event_buffer(struct virtual_dprx_dev *vdprx);
int virtio_dprx_create_shmem(struct device *dev, struct virtual_dprx_dev *vdprx, uint32_t device_id);
void virtio_dprx_destroy_shmem(struct device *dev, struct virtual_dprx_dev *vdprx, uint32_t device_id);
int virtio_dprx_hab_virq_cb(int irq, void *irq_data, uint32_t flags);
int virtio_dprx_hab_register_virq(struct virtual_dprx_dev *vdprx, uint32_t device_id);
void virtio_dprx_hab_unregister_virq(struct virtual_dprx_dev *vdprx, uint32_t device_id);
int virtio_dprx_export_memory(struct virtual_dprx_dev *vdprx, struct virtio_dprx_buffer *buffer);
int virtio_dprx_unexport_memory(struct virtual_dprx_dev *vdprx, struct virtio_dprx_buffer *buffer);
int virtio_dprx_virtq_open(struct virtual_dprx_dev *dev, uint32_t cell_index);
int virtio_dprx_virtq_close (struct virtual_dprx_dev *vdprx);
int virtio_dprx_virtq_send_command(struct virtual_dprx_dev *dev, void *req,
		uint32_t req_size, void *resp, uint32_t resp_size);
void virtio_dprx_process_events(struct virtual_dprx_dev *vdprx);
long virtio_dprx_device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* trace API (used from virtio_dprx.c and virtio_media_ioctl.c) */
void vdprx_trace_init(struct virtual_dprx_dev *vdprx);
void vdprx_trace_exit(struct virtual_dprx_dev *vdprx);
void vdprx_trace_ioctl_record(struct virtual_dprx_dev *vdprx,
                              unsigned long cmd, long ret);
void vdprx_trace_event_record(struct virtual_dprx_dev *vdprx,
                              u32 media_evt, u32 session_id, u32 extra);
int  vdprx_debugfs_create(struct virtual_dprx_dev *vdprx);
void vdprx_debugfs_remove(struct virtual_dprx_dev *vdprx);

#endif //__VIRTIO_DPRX_H

