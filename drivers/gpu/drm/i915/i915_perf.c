/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/perf_event.h>
#include <linux/anon_inodes.h>
#include <linux/sizes.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"
#include "intel_lrc.h"
#include "i915_oa_hsw.h"

/* Must be a power of two */
#define OA_BUFFER_SIZE	     SZ_16M
#define OA_TAKEN(tail, head) ((tail - head) & (OA_BUFFER_SIZE - 1))

/* frequency for forwarding samples from OA to perf buffer */
#define POLL_FREQUENCY 200
#define POLL_PERIOD max_t(u64, 10000, NSEC_PER_SEC / POLL_FREQUENCY)

#define OA_EXPONENT_MAX 0x3f

static struct i915_oa_format hsw_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A13]	    = { 0, 64 },
	[I915_OA_FORMAT_A29]	    = { 1, 128 },
	[I915_OA_FORMAT_A13_B8_C8]  = { 2, 128 },
	/* A29_B8_C8 Disallowed as 192 bytes doesn't factor into buffer size */
	[I915_OA_FORMAT_B4_C8]	    = { 4, 64 },
	[I915_OA_FORMAT_A45_B8_C8]  = { 5, 256 },
	[I915_OA_FORMAT_B4_C8_A16]  = { 6, 128 },
	[I915_OA_FORMAT_C4_B8]	    = { 7, 64 },
};


/**
 * i915_perf_copy_attr() - copy specific event attributes from userspace
 * @uattr:	The u64 __user attr of drm_i915_perf_open_param
 * @attr:	Destination for copied attributes
 * @v0_size:	The smallest, version 0 size of these attributes
 * @real_size:	The latest size expected by this kernel version
 *
 * Specific events can define a custom attributes structure and for
 * consistency should use this utility for reading the attributes from
 * userspace.
 *
 * Note: although this verifies any unknown members beyond the expected
 * struct size are zeroed it can't check for unused flags
 *
 * Return: 0 if successful, else an error code
 */
static int i915_perf_copy_attr(void __user *uattr,
			       void *attr,
			       u32 v0_size,
			       u32 real_size)
{
	u32 size;
	int ret;

	if (!access_ok(VERIFY_WRITE, uattr, v0_size))
		return -EFAULT;

	/*
	 * zero the full structure, so that a short copy will be nice.
	 */
	memset(attr, 0, real_size);

	ret = get_user(size, (u32 __user *)uattr);
	if (ret)
		return ret;

	if (size > PAGE_SIZE)   /* silly large */
		goto err_size;

	if (size < v0_size)
		goto err_size;

	/*
	 * If we're handed a bigger struct than we know of,
	 * ensure all the unknown bits are 0 - i.e. new
	 * user-space does not rely on any kernel feature
	 * extensions we dont know about yet.
	 */

	if (size > real_size) {
		unsigned char __user *addr;
		unsigned char __user *end;
		unsigned char val;

		addr = (void __user *)uattr + sizeof(*attr);
		end  = (void __user *)uattr + size;

		for (; addr < end; addr++) {
			ret = get_user(val, addr);
			if (ret)
				return ret;
			if (val)
				goto err_size;
		}
		size = sizeof(*attr);
	}

	ret = copy_from_user(attr, uattr, size);
	if (ret)
		return -EFAULT;

out:
	return ret;

err_size:
	put_user(real_size, (u32 __user *)uattr);
	ret = -E2BIG;
	goto out;
}


static bool gen7_oa_buffer_is_empty(struct drm_i915_private *dev_priv)
{
	u32 oastatus2 = I915_READ(GEN7_OASTATUS2);
	u32 oastatus1 = I915_READ(GEN7_OASTATUS1);
	u32 head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	u32 tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	return OA_TAKEN(tail, head) == 0;
}

static bool append_oa_status(struct i915_perf_event *event,
			     struct i915_perf_read_state *read_state,
			     enum drm_i915_perf_record_type type)
{
	struct drm_i915_perf_event_header header = { type, 0, sizeof(header) };

	if ((read_state->count - read_state->read) < header.size)
		return false;

	copy_to_user(read_state->buf, &header, sizeof(header));

	read_state->buf += sizeof(header);
	read_state->read += header.size;

	return true;
}

static bool append_oa_sample(struct i915_perf_event *event,
			     struct i915_perf_read_state *read_state,
			     const u8 *report)
{
	struct drm_i915_private *dev_priv = event->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	struct drm_i915_perf_event_header header;
	u32 sample_flags = event->sample_flags;
	u32 dummy_ctx_id = 0;
	u32 dummy_timestamp = 0;

	header.type = DRM_I915_PERF_RECORD_SAMPLE;
	header.misc = 0;
	header.size = sizeof(header);


	/* XXX: could pre-compute this when opening the event... */

	if (sample_flags & I915_PERF_SAMPLE_CTXID)
		header.size += 4;

	if (sample_flags & I915_PERF_SAMPLE_TIMESTAMP)
		header.size += 4;

	if (sample_flags & I915_PERF_SAMPLE_OA_REPORT)
		header.size += report_size;


	if ((read_state->count - read_state->read) < header.size)
		return false;


	copy_to_user(read_state->buf, &header, sizeof(header));
	read_state->buf += sizeof(header);

	if (sample_flags & I915_PERF_SAMPLE_CTXID) {
#warning "fixme: extract context ID from OA reports"
		copy_to_user(read_state->buf, &dummy_ctx_id, 4);
		read_state->buf += 4;
	}

	if (sample_flags & I915_PERF_SAMPLE_TIMESTAMP) {
#warning "fixme: extract timestamp from OA reports"
		copy_to_user(read_state->buf, &dummy_timestamp, 4);
		read_state->buf += 4;
	}

	if (sample_flags & I915_PERF_SAMPLE_OA_REPORT) {
		copy_to_user(read_state->buf, report, report_size);
		read_state->buf += report_size;
	}


	read_state->read += header.size;

	return true;
}

static u32 gen7_append_oa_reports(struct i915_perf_event *event,
				  struct i915_perf_read_state *read_state,
				  u32 head,
				  u32 tail)
{
	struct drm_i915_private *dev_priv = event->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u8 *report;
	u32 taken;

	head -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;

	/* Note: the gpu doesn't wrap the tail according to the OA buffer size
	 * so when we need to make sure our head/tail values are in-bounds we
	 * use the above mask.
	 */

	while ((taken = OA_TAKEN(tail, head))) {
		/* The tail increases in 64 byte increments, not in
		 * format_size steps. */
		if (taken < report_size)
			break;

		report = oa_buf_base + (head & mask);

		if (dev_priv->perf.oa.exclusive_event->enabled) {
			if (!append_oa_sample(event, read_state, report))
				break;
		}

		/* If append_oa_sample() returns false we shouldn't progress
		 * head so we update it afterwards... */
		head += report_size;
	}

	return dev_priv->perf.oa.oa_buffer.gtt_offset + head;
}

static void gen7_oa_read(struct i915_perf_event *event,
			 struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = event->dev_priv;
	u32 oastatus2;
	u32 oastatus1;
	u32 head;
	u32 tail;

	WARN_ON(!dev_priv->perf.oa.oa_buffer.addr);

	oastatus2 = I915_READ(GEN7_OASTATUS2);
	oastatus1 = I915_READ(GEN7_OASTATUS1);

	head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	if (unlikely(oastatus1 & (GEN7_OASTATUS1_OABUFFER_OVERFLOW |
				  GEN7_OASTATUS1_REPORT_LOST))) {

		if (oastatus1 & GEN7_OASTATUS1_OABUFFER_OVERFLOW) {
			if (append_oa_status(event, read_state,
					     DRM_I915_PERF_RECORD_OA_BUFFER_OVERFLOW))
				oastatus1 &= ~GEN7_OASTATUS1_OABUFFER_OVERFLOW;
		}

		if (oastatus1 & GEN7_OASTATUS1_REPORT_LOST) {
			if (append_oa_status(event, read_state,
					     DRM_I915_PERF_RECORD_OA_REPORT_LOST))
				oastatus1 &= ~GEN7_OASTATUS1_REPORT_LOST;
		}

		I915_WRITE(GEN7_OASTATUS1, oastatus1);
	}

	head = gen7_append_oa_reports(event, read_state, head, tail);

	I915_WRITE(GEN7_OASTATUS2, (head & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);
}

static bool i915_oa_can_read(struct i915_perf_event *event)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	return !dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv);
}

static int i915_oa_wait_unlocked(struct i915_perf_event *event)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	/* Note: the oa_buffer_is_empty() condition is ok to run unlocked as it
	 * just performs mmio reads of the OA buffer head + tail pointers and
	 * it's assumed we're handling some operation that implies the event
	 * can't be destroyed until completion (such as a read()) that ensures
	 * the device + OA buffer can't disappear
	 */
	return wait_event_interruptible(dev_priv->perf.oa.poll_wq,
					!dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv));
}

static void i915_oa_poll_wait(struct i915_perf_event *event,
			      struct file *file,
			      poll_table *wait)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	poll_wait(file, &dev_priv->perf.oa.poll_wq, wait);
}

static void i915_oa_read(struct i915_perf_event *event,
			 struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	dev_priv->perf.oa.ops.read(event, read_state);
}

static void
free_oa_buffer(struct drm_i915_private *i915)
{
	mutex_lock(&i915->dev->struct_mutex);

	vunmap(i915->perf.oa.oa_buffer.addr);
	i915_gem_object_ggtt_unpin(i915->perf.oa.oa_buffer.obj);
	drm_gem_object_unreference(&i915->perf.oa.oa_buffer.obj->base);

	i915->perf.oa.oa_buffer.obj = NULL;
	i915->perf.oa.oa_buffer.gtt_offset = 0;
	i915->perf.oa.oa_buffer.addr = NULL;

	mutex_unlock(&i915->dev->struct_mutex);
}

static void i915_oa_event_destroy(struct i915_perf_event *event)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	BUG_ON(event != dev_priv->perf.oa.exclusive_event);

	dev_priv->perf.oa.ops.disable_metric_set(dev_priv);

	free_oa_buffer(dev_priv);

	intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	intel_runtime_pm_put(dev_priv);

	dev_priv->perf.oa.exclusive_event = NULL;
}

static void *vmap_oa_buffer(struct drm_i915_gem_object *obj)
{
	int i;
	void *addr = NULL;
	struct sg_page_iter sg_iter;
	struct page **pages;

	pages = drm_malloc_ab(obj->base.size >> PAGE_SHIFT, sizeof(*pages));
	if (pages == NULL) {
		DRM_DEBUG_DRIVER("Failed to get space for pages\n");
		goto finish;
	}

	i = 0;
	for_each_sg_page(obj->pages->sgl, &sg_iter, obj->pages->nents, 0) {
		pages[i] = sg_page_iter_page(&sg_iter);
		i++;
	}

	addr = vmap(pages, i, 0, PAGE_KERNEL);
	if (addr == NULL) {
		DRM_DEBUG_DRIVER("Failed to vmap pages\n");
		goto finish;
	}

finish:
	if (pages)
		drm_free_large(pages);
	return addr;
}

static void gen7_init_oa_buffer(struct drm_i915_private *dev_priv)
{
	/* Pre-DevBDW: OABUFFER must be set with counters off,
	 * before OASTATUS1, but after OASTATUS2 */
	I915_WRITE(GEN7_OASTATUS2, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OA_MEM_SELECT_GGTT); /* head */
	I915_WRITE(GEN7_OABUFFER, dev_priv->perf.oa.oa_buffer.gtt_offset);
	I915_WRITE(GEN7_OASTATUS1, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OABUFFER_SIZE_16M); /* tail */
}

static int alloc_oa_buffer(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *bo;
	int ret;

	BUG_ON(dev_priv->perf.oa.oa_buffer.obj);

	ret = i915_mutex_lock_interruptible(dev_priv->dev);
	if (ret)
		return ret;

	bo = i915_gem_alloc_object(dev_priv->dev, OA_BUFFER_SIZE);
	if (bo == NULL) {
		DRM_ERROR("Failed to allocate OA buffer\n");
		ret = -ENOMEM;
		goto unlock;
	}
	dev_priv->perf.oa.oa_buffer.obj = bo;

	ret = i915_gem_object_set_cache_level(bo, I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	/* PreHSW required 512K alignment, HSW requires 16M */
	ret = i915_gem_obj_ggtt_pin(bo, SZ_16M, 0);
	if (ret)
		goto err_unref;

	dev_priv->perf.oa.oa_buffer.gtt_offset = i915_gem_obj_ggtt_offset(bo);
	dev_priv->perf.oa.oa_buffer.addr = vmap_oa_buffer(bo);

	dev_priv->perf.oa.ops.init_oa_buffer(dev_priv);

	DRM_DEBUG_DRIVER("OA Buffer initialized, gtt offset = 0x%x, vaddr = %p",
			 dev_priv->perf.oa.oa_buffer.gtt_offset,
			 dev_priv->perf.oa.oa_buffer.addr);

	goto unlock;

err_unref:
	drm_gem_object_unreference(&bo->base);

unlock:
	mutex_unlock(&dev_priv->dev->struct_mutex);
	return ret;
}

static void config_oa_regs(struct drm_i915_private *dev_priv,
			   const struct i915_oa_reg *regs,
			   int n_regs)
{
	int i;

	for (i = 0; i < n_regs; i++) {
		const struct i915_oa_reg *reg = regs + i;

		I915_WRITE(reg->addr, reg->value);
	}
}

static void hsw_enable_metric_set(struct drm_i915_private *dev_priv)
{
	dev_priv->perf.oa.mux_regs = NULL;
	dev_priv->perf.oa.mux_regs_len = 0;
	dev_priv->perf.oa.b_counter_regs = NULL;
	dev_priv->perf.oa.b_counter_regs_len = 0;

	I915_WRITE(GDT_CHICKEN_BITS, GT_NOA_ENABLE);

	/* PRM:
	 *
	 * OA unit is using “crclk” for its functionality. When trunk
	 * level clock gating takes place, OA clock would be gated,
	 * unable to count the events from non-render clock domain.
	 * Render clock gating must be disabled when OA is enabled to
	 * count the events from non-render domain. Unit level clock
	 * gating for RCS should also be disabled.
	 */
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) &
				    ~GEN7_DOP_CLOCK_GATE_ENABLE));
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) |
				  GEN6_CSUNIT_CLOCK_GATE_DISABLE));

	switch (dev_priv->perf.oa.metrics_set) {
	case I915_OA_METRICS_SET_3D:
		config_oa_regs(dev_priv, i915_oa_3d_mux_config_hsw,
			       i915_oa_3d_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_3d_b_counter_config_hsw,
			       i915_oa_3d_b_counter_config_hsw_len);
		break;
	default:
		BUG();
	}
}

static void hsw_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));

	I915_WRITE(GDT_CHICKEN_BITS, (I915_READ(GDT_CHICKEN_BITS) &
				      ~GT_NOA_ENABLE));
}

static void gen7_update_oacontrol(struct drm_i915_private *dev_priv)
{
	if (dev_priv->perf.oa.exclusive_event->enabled) {
		unsigned long ctx_id = 0;
		bool pinning_ok = false;

		if (dev_priv->perf.oa.exclusive_event->ctx &&
		    dev_priv->perf.oa.specific_ctx_id) {
			ctx_id = dev_priv->perf.oa.specific_ctx_id;
			pinning_ok = true;
		}

		if (dev_priv->perf.oa.exclusive_event->ctx == NULL ||
		    pinning_ok) {
			bool periodic = dev_priv->perf.oa.periodic;
			u32 period_exponent = dev_priv->perf.oa.period_exponent;
			u32 report_format = dev_priv->perf.oa.oa_buffer.format;

			I915_WRITE(GEN7_OACONTROL,
				   (ctx_id & GEN7_OACONTROL_CTX_MASK) |
				   (period_exponent <<
				    GEN7_OACONTROL_TIMER_PERIOD_SHIFT) |
				   (periodic ?
				    GEN7_OACONTROL_TIMER_ENABLE : 0) |
				   (report_format <<
				    GEN7_OACONTROL_FORMAT_SHIFT) |
				   (ctx_id ?
				    GEN7_OACONTROL_PER_CTX_ENABLE : 0) |
				   GEN7_OACONTROL_ENABLE);
			return;
		}
	}

	I915_WRITE(GEN7_OACONTROL, 0);
}

static void gen7_oa_enable(struct drm_i915_private *dev_priv)
{
	u32 oastatus1, tail;

	gen7_update_oacontrol(dev_priv);

	/* Reset the head ptr so we don't forward reports from before now. */
	oastatus1 = I915_READ(GEN7_OASTATUS1);
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	I915_WRITE(GEN7_OASTATUS2, (tail & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);
}

static void i915_oa_event_enable(struct i915_perf_event *event)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	dev_priv->perf.oa.ops.oa_enable(dev_priv);

	if (dev_priv->perf.oa.periodic)
		hrtimer_start(&dev_priv->perf.oa.poll_check_timer,
			      ns_to_ktime(POLL_PERIOD),
			      HRTIMER_MODE_REL_PINNED);
}

static void gen7_oa_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN7_OACONTROL, 0);
}

static void i915_oa_event_disable(struct i915_perf_event *event)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	dev_priv->perf.oa.ops.oa_disable(dev_priv);

	if (dev_priv->perf.oa.periodic)
		hrtimer_cancel(&dev_priv->perf.oa.poll_check_timer);
}

static int i915_oa_event_init(struct i915_perf_event *event,
			      struct drm_i915_perf_open_param *param)
{
	struct drm_i915_private *dev_priv = event->dev_priv;
	struct drm_i915_perf_oa_attr oa_attr;
	u32 known_flags = 0;
	int format_size;
	int ret;

	BUG_ON(param->type != I915_PERF_OA_EVENT);

	if (!dev_priv->perf.oa.ops.init_oa_buffer) {
		DRM_ERROR("OA unit not supported\n");
		return -ENODEV;
	}

	/* To avoid the complexity of having to accurately filter
	 * counter reports and marshal to the appropriate client
	 * we currently only allow exclusive access */
	if (dev_priv->perf.oa.exclusive_event) {
		DRM_ERROR("OA unit already in use\n");
		return -EBUSY;
	}

	ret = i915_perf_copy_attr(to_user_ptr(param->attr),
					      &oa_attr,
					      I915_OA_ATTR_SIZE_VER0,
					      sizeof(oa_attr));
	if (ret)
		return ret;

	known_flags = I915_OA_FLAG_PERIODIC;
	if (oa_attr.flags & ~known_flags) {
		DRM_ERROR("Unknown drm_i915_perf_oa_attr flag\n");
		return -EINVAL;
	}

	if (oa_attr.oa_format >= I915_OA_FORMAT_MAX) {
		DRM_ERROR("Invalid OA report format\n");
		return -EINVAL;
	}

	format_size = dev_priv->perf.oa.oa_formats[oa_attr.oa_format].size;
	if (!format_size) {
		DRM_ERROR("Invalid OA report format\n");
		return -EINVAL;
	}

	dev_priv->perf.oa.oa_buffer.format_size = format_size;

	dev_priv->perf.oa.oa_buffer.format =
		dev_priv->perf.oa.oa_formats[oa_attr.oa_format].format;

	if (IS_HASWELL(dev_priv->dev)) {
		if (oa_attr.metrics_set <= 0 ||
		    oa_attr.metrics_set > I915_OA_METRICS_SET_MAX) {
			DRM_ERROR("Metric set not available\n");
			return -EINVAL;
		}
	} else {
		BUG(); /* checked above */
		return -ENODEV;
	}

	dev_priv->perf.oa.metrics_set = oa_attr.metrics_set;

	dev_priv->perf.oa.periodic = !!(oa_attr.flags & I915_OA_FLAG_PERIODIC);

	/* NB: The exponent represents a period as follows:
	 *
	 *   80ns * 2^(period_exponent + 1)
	 */
	if (dev_priv->perf.oa.periodic) {
		u64 period_exponent = oa_attr.oa_timer_exponent;

		if (period_exponent > OA_EXPONENT_MAX)
			return -EINVAL;

		/* Theoretically we can program the OA unit to sample every
		 * 160ns but don't allow that by default unless root...
		 *
		 * Referring to perf's kernel.perf_event_max_sample_rate for
		 * a precedent (100000 by default); with an OA exponent of
		 * 6 we get a period of 10.240 microseconds -just under
		 * 100000Hz
		 */
		if (period_exponent < 6 && !capable(CAP_SYS_ADMIN)) {
			DRM_ERROR("Sampling period too high without root privileges\n");
			return -EACCES;
		}

		dev_priv->perf.oa.period_exponent = period_exponent;
	} else if (oa_attr.oa_timer_exponent) {
		DRM_ERROR("Sampling exponent specified without requesting periodic sampling");
		return -EINVAL;
	}

	ret = alloc_oa_buffer(dev_priv);
	if (ret)
		return ret;

	dev_priv->perf.oa.exclusive_event = event;

	/* PRM - observability performance counters:
	 *
	 *   OACONTROL, performance counter enable, note:
	 *
	 *   "When this bit is set, in order to have coherent counts,
	 *   RC6 power state and trunk clock gating must be disabled.
	 *   This can be achieved by programming MMIO registers as
	 *   0xA094=0 and 0xA090[31]=1"
	 *
	 *   In our case we are expected that taking pm + FORCEWAKE
	 *   references will effectively disable RC6.
	 */
	intel_runtime_pm_get(dev_priv);
	intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

	dev_priv->perf.oa.ops.enable_metric_set(dev_priv);

	event->destroy = i915_oa_event_destroy;
	event->enable = i915_oa_event_enable;
	event->disable = i915_oa_event_disable;
	event->can_read = i915_oa_can_read;
	event->wait_unlocked = i915_oa_wait_unlocked;
	event->poll_wait = i915_oa_poll_wait;
	event->read = i915_oa_read;

	return 0;
}

static void gen7_update_specific_hw_ctx_id(struct drm_i915_private *dev_priv,
					   u32 ctx_id)
{
	dev_priv->perf.oa.specific_ctx_id = ctx_id;
	gen7_update_oacontrol(dev_priv);
}

static void i915_oa_context_pin_notify_locked(struct drm_i915_private *dev_priv,
					      struct intel_context *context)
{
	if (i915.enable_execlists ||
	    dev_priv->perf.oa.ops.update_specific_hw_ctx_id == NULL)
		return;

	if (dev_priv->perf.oa.exclusive_event &&
	    dev_priv->perf.oa.exclusive_event->ctx == context) {
		struct drm_i915_gem_object *obj =
			context->legacy_hw_ctx.rcs_state;
		u32 ctx_id = i915_gem_obj_ggtt_offset(obj);

		dev_priv->perf.oa.ops.update_specific_hw_ctx_id(dev_priv, ctx_id);
	}
}

void i915_oa_context_pin_notify(struct drm_i915_private *dev_priv,
				struct intel_context *context)
{
	if (!dev_priv->perf.initialized)
		return;

	mutex_lock(&dev_priv->perf.lock);
	i915_oa_context_pin_notify_locked(dev_priv, context);
	mutex_unlock(&dev_priv->perf.lock);
}

static ssize_t i915_perf_read_locked(struct i915_perf_event *event,
				     struct file *file,
				     char __user *buf,
				     size_t count,
				     loff_t *ppos)
{
	struct drm_i915_private *dev_priv = event->dev_priv;
	struct i915_perf_read_state state = { count, 0, buf };
	int ret;

	if (file->f_flags & O_NONBLOCK) {
		if (!event->can_read(event))
			return -EAGAIN;
	} else {
		mutex_unlock(&dev_priv->perf.lock);
		ret = event->wait_unlocked(event);
		mutex_lock(&dev_priv->perf.lock);

		if (ret)
			return ret;
	}

	event->read(event, &state);
	if (state.read == 0)
		return -ENOSPC;

	return state.read;
}

static ssize_t i915_perf_read(struct file *file,
			      char __user *buf,
			      size_t count,
			      loff_t *ppos)
{
	struct i915_perf_event *event = file->private_data;
	struct drm_i915_private *dev_priv = event->dev_priv;
	ssize_t ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_read_locked(event, file, buf, count, ppos);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static enum hrtimer_restart poll_check_timer_cb(struct hrtimer *hrtimer)
{
	struct drm_i915_private *dev_priv =
		container_of(hrtimer, typeof(*dev_priv),
			     perf.oa.poll_check_timer);

	if (!dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv))
		wake_up(&dev_priv->perf.oa.poll_wq);

	hrtimer_forward_now(hrtimer, ns_to_ktime(POLL_PERIOD));

	return HRTIMER_RESTART;
}

static unsigned int i915_perf_poll_locked(struct i915_perf_event *event,
					  struct file *file,
					  poll_table *wait)
{
	unsigned int events = 0;

	event->poll_wait(event, file, wait);

	if (event->can_read(event))
		events |= POLLIN;

	return events;
}

static unsigned int i915_perf_poll(struct file *file, poll_table *wait)
{
	struct i915_perf_event *event = file->private_data;
	struct drm_i915_private *dev_priv = event->dev_priv;
	int ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_poll_locked(event, file, wait);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static void i915_perf_enable_locked(struct i915_perf_event *event)
{
	if (event->enabled)
		return;

	/* Allow event->enable() to refer to this */
	event->enabled = true;

	if (event->enable)
		event->enable(event);
}

static void i915_perf_disable_locked(struct i915_perf_event *event)
{
	if (!event->enabled)
		return;

	/* Allow event->disable() to refer to this */
	event->enabled = false;

	if (event->disable)
		event->disable(event);
}

static long i915_perf_ioctl_locked(struct i915_perf_event *event,
				   unsigned int cmd,
				   unsigned long arg)
{
	switch (cmd) {
	case I915_PERF_IOCTL_ENABLE:
		i915_perf_enable_locked(event);
		return 0;
	case I915_PERF_IOCTL_DISABLE:
		i915_perf_disable_locked(event);
		return 0;
	}

	return -EINVAL;
}

static long i915_perf_ioctl(struct file *file,
			    unsigned int cmd,
			    unsigned long arg)
{
	struct i915_perf_event *event = file->private_data;
	struct drm_i915_private *dev_priv = event->dev_priv;
	long ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_ioctl_locked(event, cmd, arg);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static void i915_perf_destroy_locked(struct i915_perf_event *event)
{
	struct drm_i915_private *dev_priv = event->dev_priv;

	if (event->enabled)
		i915_perf_disable_locked(event);

	if (event->destroy)
		event->destroy(event);

	list_del(&event->link);

	if (event->ctx) {
		mutex_lock(&dev_priv->dev->struct_mutex);
		i915_gem_context_unreference(event->ctx);
		mutex_unlock(&dev_priv->dev->struct_mutex);
	}

	kfree(event);
}

static int i915_perf_release(struct inode *inode, struct file *file)
{
	struct i915_perf_event *event = file->private_data;
	struct drm_i915_private *dev_priv = event->dev_priv;

	mutex_lock(&dev_priv->perf.lock);
	i915_perf_destroy_locked(event);
	mutex_unlock(&dev_priv->perf.lock);

	return 0;
}


static const struct file_operations fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.release	= i915_perf_release,
	.poll		= i915_perf_poll,
	.read		= i915_perf_read,
	.unlocked_ioctl	= i915_perf_ioctl,
};

static struct intel_context *
lookup_context(struct drm_i915_private *dev_priv,
	       struct file *user_filp,
	       u32 ctx_user_handle)
{
	struct intel_context *ctx;

	mutex_lock(&dev_priv->dev->struct_mutex);
	list_for_each_entry(ctx, &dev_priv->context_list, link) {
		struct drm_file *drm_file;

		if (!ctx->file_priv)
			continue;

		drm_file = ctx->file_priv->file;

		if (user_filp->private_data == drm_file &&
		    ctx->user_handle == ctx_user_handle) {
			i915_gem_context_reference(ctx);
			mutex_unlock(&dev_priv->dev->struct_mutex);

			return ctx;
		}
	}
	mutex_unlock(&dev_priv->dev->struct_mutex);

	return NULL;
}

int i915_perf_open_ioctl_locked(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_perf_open_param *param = data;
	u32 known_open_flags = 0;
	u64 known_sample_flags = 0;
	struct intel_context *specific_ctx = NULL;
	struct i915_perf_event *event = NULL;
	unsigned long f_flags = 0;
	int event_fd;
	int ret = 0;

	known_open_flags = I915_PERF_FLAG_FD_CLOEXEC |
			   I915_PERF_FLAG_FD_NONBLOCK |
			   I915_PERF_FLAG_SINGLE_CONTEXT |
			   I915_PERF_FLAG_DISABLED;
	if (param->flags & ~known_open_flags) {
		DRM_ERROR("Unknown drm_i915_perf_open_param flag\n");
		ret = -EINVAL;
		goto err;
	}

	known_sample_flags = I915_PERF_SAMPLE_OA_REPORT |
			     I915_PERF_SAMPLE_CTXID |
			     I915_PERF_SAMPLE_TIMESTAMP;
	if (param->sample_flags & ~known_sample_flags) {
		DRM_ERROR("Unknown drm_i915_perf_open_param sample_flag\n");
		ret = -EINVAL;
		goto err;
	}

	if (param->flags & I915_PERF_FLAG_SINGLE_CONTEXT) {
		u32 ctx_id = param->ctx_id;

		specific_ctx = lookup_context(dev_priv, file->filp, ctx_id);
		if (!specific_ctx) {
			DRM_ERROR("Failed to look up context with ID %u for opening perf event\n", ctx_id);
			ret = -EINVAL;
			goto err;
		}
	}

	if (!specific_ctx && !capable(CAP_SYS_ADMIN)) {
		DRM_ERROR("Insufficient privileges to open perf event\n");
		ret = -EACCES;
		goto err_ctx;
	}

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	event->sample_flags = param->sample_flags;
	event->dev_priv = dev_priv;
	event->ctx = specific_ctx;

	switch (param->type) {
	case I915_PERF_OA_EVENT:
		ret = i915_oa_event_init(event, param);
		if (ret)
			goto err_alloc;
		break;
	default:
		DRM_ERROR("Unknown perf event type\n");
		ret = -EINVAL;
		goto err_alloc;
	}

	event->ctx = specific_ctx;
	list_add(&event->link, &dev_priv->perf.events);

	if (param->flags & I915_PERF_FLAG_FD_CLOEXEC)
		f_flags |= O_CLOEXEC;
	if (param->flags & I915_PERF_FLAG_FD_NONBLOCK)
		f_flags |= O_NONBLOCK;

	event_fd = anon_inode_getfd("[i915_perf]", &fops, event, f_flags);
	if (event_fd < 0) {
		ret = event_fd;
		goto err_open;
	}

	param->fd = event_fd;

	if (!(param->flags & I915_PERF_FLAG_DISABLED))
		i915_perf_enable_locked(event);

	return 0;

err_open:
	list_del(&event->link);
	if (event->destroy)
		event->destroy(event);
err_alloc:
	kfree(event);
err_ctx:
	if (specific_ctx) {
		mutex_lock(&dev_priv->dev->struct_mutex);
		i915_gem_context_unreference(specific_ctx);
		mutex_unlock(&dev_priv->dev->struct_mutex);
	}
err:
	param->fd = -1;

	return ret;
}

int i915_perf_open_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	int ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_open_ioctl_locked(dev, data, file);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

void i915_perf_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!IS_HASWELL(dev))
		return;

	hrtimer_init(&dev_priv->perf.oa.poll_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_priv->perf.oa.poll_check_timer.function = poll_check_timer_cb;
	init_waitqueue_head(&dev_priv->perf.oa.poll_wq);

	INIT_LIST_HEAD(&dev_priv->perf.events);
	mutex_init(&dev_priv->perf.lock);

	dev_priv->perf.oa.ops.init_oa_buffer = gen7_init_oa_buffer;
	dev_priv->perf.oa.ops.enable_metric_set = hsw_enable_metric_set;
	dev_priv->perf.oa.ops.disable_metric_set = hsw_disable_metric_set;
	dev_priv->perf.oa.ops.oa_enable = gen7_oa_enable;
	dev_priv->perf.oa.ops.oa_disable = gen7_oa_disable;
	dev_priv->perf.oa.ops.update_specific_hw_ctx_id = gen7_update_specific_hw_ctx_id;
	dev_priv->perf.oa.ops.read = gen7_oa_read;
	dev_priv->perf.oa.ops.oa_buffer_is_empty = gen7_oa_buffer_is_empty;

	dev_priv->perf.oa.oa_formats = hsw_oa_formats;

	dev_priv->perf.initialized = true;
}

void i915_perf_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!dev_priv->perf.initialized)
		return;

	dev_priv->perf.oa.ops.init_oa_buffer = NULL;

	dev_priv->perf.initialized = false;
}
