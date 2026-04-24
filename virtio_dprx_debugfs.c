// SPDX-License-Identifier: GPL-2.0-only
//Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#define pr_fmt(fmt) "[virtio_dprx:%s:%d] " fmt, __func__, __LINE__

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched/clock.h>
#include <linux/poll.h>
#include "virtio_dprx.h"

/* ---------------- common helpers ---------------- */

/*
Usage :
 Mount debugfs (if not already)
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null || true

# Enable tracing
echo 1 | sudo tee /sys/kernel/debug/vdprx/card0/trace_enable

# Watch IOCTLs live (blocks until new records)
cat /sys/kernel/debug/vdprx/card0/trace_all_live &

# Run your application; you'll see lines like:
# [12.345678] IOCTL pid=1234 cpu=2 raw=0xc0145608 type=86 nr=8 dir=READ|WRITE size=20 ret=0
# [12.346001] EVENT pid=1234 cpu=2 media_evt=2 session=1 extra=0

# Snapshot (current ring contents)
sudo cat /sys/kernel/debug/vdprx/card0/trace_all

# Clear buffers
echo 1 | sudo tee /sys/kernel/debug/vdprx/card0/trace_clear

# Disable (minimal overhead)
echo 0 | sudo tee /sys/kernel/debug/vdprx/card0/trace_enable

*/

static void vdprx_trace_setup(struct vdprx_trace *tr, unsigned int n)
{
	tr->size = n ? n : VDP_RX_TRACE_RING_SZ;
	tr->buf  = kcalloc(tr->size, sizeof(*tr->buf), GFP_KERNEL);
	spin_lock_init(&tr->lock);
	init_waitqueue_head(&tr->wq);
	atomic64_set(&tr->seq, 0);
	tr->enabled = false;
}

static void vdprx_trace_free(struct vdprx_trace *tr)
{
	kfree(tr->buf);
	tr->buf = NULL;
	tr->size = 0;
}

void vdprx_trace_init(struct virtual_dprx_dev *vdprx)
{
	vdprx_trace_setup(&vdprx->tr_all, VDP_RX_TRACE_RING_SZ);
}

void vdprx_trace_exit(struct virtual_dprx_dev *vdprx)
{
	vdprx_trace_free(&vdprx->tr_all);
}

static void vdprx_trace_push(struct vdprx_trace *tr,
                             const struct vdprx_trace_rec *rec)
{
	unsigned long flags;

	if (!tr->enabled ||  !tr->buf)
		return;
	spin_lock_irqsave(&tr->lock, flags);
	{
		u64 seq = atomic64_inc_return(&tr->seq);
		unsigned int i = tr->head++ % tr->size;
		struct vdprx_trace_rec *dst = &tr->buf[i];
		*dst = *rec;
		dst->seq = seq;
	}
	spin_unlock_irqrestore(&tr->lock, flags);
	wake_up_interruptible(&tr->wq);
}

/* ---------------- public recorders ---------------- */

void vdprx_trace_ioctl_record(struct virtual_dprx_dev *vdprx,
		unsigned long cmd, long ret)
{
	struct vdprx_trace_rec r = {
		.ts_ns = ktime_get_ns(),
		.type  = VTP_IOCTL,
		.cpu   = raw_smp_processor_id(),
		.pid   = current->pid,
	};
	r.u.ioctl.cmd_raw  = cmd;
	r.u.ioctl.cmd_type = _IOC_TYPE(cmd);
	r.u.ioctl.cmd_nr   = _IOC_NR(cmd);
	r.u.ioctl.cmd_dir  = _IOC_DIR(cmd);
	r.u.ioctl.cmd_size = _IOC_SIZE(cmd);
	r.u.ioctl.ret      = ret;
	vdprx_trace_push(&vdprx->tr_all, &r);
}

void vdprx_trace_event_record(struct virtual_dprx_dev *vdprx,
		u32 media_evt, u32 session_id, u32 extra)
{
	struct vdprx_trace_rec r = {
		.ts_ns = ktime_get_ns(),
		.type  = VTP_EVENT,
		.cpu   = raw_smp_processor_id(),
		.pid   = current->pid,
	};
	r.u.evt.media_evt  = media_evt;
	r.u.evt.session_id = session_id;
	r.u.evt.extra      = extra;
	vdprx_trace_push(&vdprx->tr_all, &r);
}

/* --------------- formatting helpers ---------------- */

static const char *ioc_dir_to_str(unsigned int dir)
{
	switch (dir) {
	case _IOC_NONE:        return "NONE";
	case _IOC_READ:        return "READ";
	case _IOC_WRITE:       return "WRITE";
	case _IOC_READ|_IOC_WRITE: return "READ|WRITE";
	}
	return "UNKNOWN";
}

static void vdprx_trace_format_rec(struct vdprx_trace_rec *rec, char *buf, size_t bufsz)
{
	u64 t = rec->ts_ns;
	do_div(t, 1000); /* ns -> us for compactness */
	if (rec->type == VTP_IOCTL) {
		scnprintf(buf, bufsz,
			"[%llu.%06llu] IOCTL pid=%u cpu=%u raw=0x%lx type=%u nr=%u dir=%s size=%u ret=%ld\n",
			(unsigned long long)(t / 1000000ULL),
			(unsigned long long)(t % 1000000ULL),
			rec->pid, rec->cpu,
			rec->u.ioctl.cmd_raw,
			rec->u.ioctl.cmd_type,
			rec->u.ioctl.cmd_nr,
			ioc_dir_to_str(rec->u.ioctl.cmd_dir),
			rec->u.ioctl.cmd_size,
			rec->u.ioctl.ret);
	} else {
		scnprintf(buf, bufsz,
			"[%llu.%06llu] EVENT pid=%u cpu=%u media_evt=%u session=%u extra=%u\n",
			(unsigned long long)(t / 1000000ULL),
			(unsigned long long)(t % 1000000ULL),
			rec->pid, rec->cpu,
			rec->u.evt.media_evt,
			rec->u.evt.session_id,
			rec->u.evt.extra);
	}
}

/* ---------------- seq_file snapshot readers ---------------- */

static int vdprx_seq_show(struct seq_file *m, void *v)
{
	struct vdprx_trace *tr = m->private;
	unsigned long flags;
	unsigned int n, start;
	if (!tr->buf)
		return 0;

	spin_lock_irqsave(&tr->lock, flags);
	n = min(tr->head, tr->size);
	start = (tr->head >= tr->size) ? (tr->head - tr->size) : 0;
	for (unsigned int i = 0; i < n; ++i) {
		struct vdprx_trace_rec rec = tr->buf[(start + i) % tr->size];
		char line[256];
		vdprx_trace_format_rec(&rec, line, sizeof(line));
		seq_puts(m, line);
	}
	spin_unlock_irqrestore(&tr->lock, flags);
	return 0;
}

static int vdprx_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, vdprx_seq_show, inode->i_private);
}

static const struct file_operations vdprx_seq_fops = {
	.owner   = THIS_MODULE,
	.open    = vdprx_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---------------- live stream readers (+poll) ---------------- */

struct vdprx_live_ctx {
	struct vdprx_trace *tr;
	u64 last_seq;
};

static ssize_t vdprx_live_read(struct file *file, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct vdprx_live_ctx *ctx = file->private_data;
	struct vdprx_trace *tr = ctx->tr;
	char line[256];
	struct vdprx_trace_rec rec;
	unsigned long flags;
	u64 want = ctx->last_seq + 1;

	if (!tr->buf)
		return 0;

	/* Wait for next record if none available */
	while (atomic64_read(&tr->seq) < want) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(tr->wq, atomic64_read(&tr->seq) >= want))
			return -ERESTARTSYS;
	}

	spin_lock_irqsave(&tr->lock, flags);
	{
		/* Find record with sequence 'want' (or the closest newer one) */
		u64 cur = atomic64_read(&tr->seq);
		if (cur - want >= tr->size) {
			/* We overran the ring: jump to the oldest in-ring */
			want = cur - tr->size + 1;
		}
		/* Map seq -> index; last 'head' holds cur, so index is (want-1) */
		rec = tr->buf[(want - 1) % tr->size];
	}
	spin_unlock_irqrestore(&tr->lock, flags);

	vdprx_trace_format_rec(&rec, line, sizeof(line));
	if (count < strlen(line))
		return -EINVAL;

	if (copy_to_user(ubuf, line, strlen(line)))
		return -EFAULT;

	ctx->last_seq = want;
	return strlen(line);
}

static __poll_t vdprx_live_poll(struct file *file, poll_table *wait)
{
	struct vdprx_live_ctx *ctx = file->private_data;
	struct vdprx_trace *tr = ctx->tr;
	__poll_t mask = 0;

	poll_wait(file, &tr->wq, wait);
	if (atomic64_read(&tr->seq) > ctx->last_seq)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static int vdprx_live_open(struct inode *inode, struct file *file)
{
	struct vdprx_trace *tr = inode->i_private;
	struct vdprx_live_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->tr = tr;
	ctx->last_seq = atomic64_read(&tr->seq);
	file->private_data = ctx;
	nonseekable_open(inode, file);
	return 0;
}

static int vdprx_live_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static const struct file_operations vdprx_live_fops = {
	.owner   = THIS_MODULE,
	.open    = vdprx_live_open,
	.read    = vdprx_live_read,
	.poll    = vdprx_live_poll,
	.release = vdprx_live_release,
	.llseek  = noop_llseek,
};


/* ---------------- enable/clear knobs (unified) ---------------- */
static int vdprx_trace_enable_all_set(void *data, u64 v)
{
	struct virtual_dprx_dev *vdprx = data;
	vdprx->tr_all.enabled = !!v;
	return 0;
}

static int vdprx_trace_enable_all_get(void *data, u64 *v)
{
	struct virtual_dprx_dev *vdprx = data;
	*v = vdprx->tr_all.enabled ? 1 : 0;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(vdprx_enable_all_fops, vdprx_trace_enable_all_get,
                         vdprx_trace_enable_all_set, "%llu\n");

static ssize_t vdprx_trace_clear_write(struct file *f, const char __user *b,
		size_t len, loff_t *ppos)
{
	struct virtual_dprx_dev *vdprx = f->private_data;
	unsigned long flags;

	spin_lock_irqsave(&vdprx->tr_all.lock, flags);
	vdprx->tr_all.head = 0;
	atomic64_set(&vdprx->tr_all.seq, 0);
	spin_unlock_irqrestore(&vdprx->tr_all.lock, flags);

	return len;
}
static int vdprx_trace_clear_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private; /* vdprx */
	return 0;
}
static const struct file_operations vdprx_clear_fops = {
	.owner  = THIS_MODULE,
	.open   = vdprx_trace_clear_open,
	.write  = vdprx_trace_clear_write,
	.llseek = noop_llseek,
};

/* ---------------- public create/remove ---------------- */

int vdprx_debugfs_create(struct virtual_dprx_dev *vdprx)
{
	char card_dir[16];

	vdprx_trace_init(vdprx);

	if (!debugfs_initialized())
		return -ENODEV;

	/* Top-level "vdprx" dir may already exist (shared by multi-cards) */
	vdprx->dbg_root = debugfs_lookup("vdprx", NULL);
	if (!vdprx->dbg_root)
		vdprx->dbg_root = debugfs_create_dir("vdprx", NULL);
	if (IS_ERR_OR_NULL(vdprx->dbg_root))
		return vdprx->dbg_root ? PTR_ERR(vdprx->dbg_root) : -ENOMEM;

	snprintf(card_dir, sizeof(card_dir), "card%u", vdprx->cell_index);
	vdprx->dbg_dir_card = debugfs_create_dir(card_dir, vdprx->dbg_root);
	if (IS_ERR_OR_NULL(vdprx->dbg_dir_card))
		return vdprx->dbg_dir_card ? PTR_ERR(vdprx->dbg_dir_card) : -ENOMEM;

	/* enable knobs per stream */
	vdprx->dbg_trace_enable = debugfs_create_file("trace_enable", 0644, vdprx->dbg_dir_card,
			vdprx, &vdprx_enable_all_fops);
	/* A second enable file can be added for events if you want separate toggles.
	   For simplicity we reuse tr_ioctls knob for both streams: */
	if (vdprx->dbg_trace_enable)
		vdprx->tr_all.enabled = vdprx->tr_all.enabled;

	vdprx->dbg_trace_clear = debugfs_create_file("trace_clear", 0200, vdprx->dbg_dir_card,
			vdprx, &vdprx_clear_fops);

	/* snapshot seq_files */

	vdprx->dbg_trace_all = debugfs_create_file("trace_all", 0444, vdprx->dbg_dir_card,
			&vdprx->tr_all, &vdprx_seq_fops);


	/* live streamers with poll() */

	vdprx->dbg_trace_all_live = debugfs_create_file("trace_all_live", 0444, vdprx->dbg_dir_card,
			&vdprx->tr_all, &vdprx_live_fops);

	return 0;
}

void vdprx_debugfs_remove(struct virtual_dprx_dev *vdprx)
{
	if (vdprx->dbg_dir_card) {
		debugfs_remove_recursive(vdprx->dbg_dir_card);
		vdprx->dbg_dir_card = NULL;
	}
	vdprx_trace_exit(vdprx);
}
