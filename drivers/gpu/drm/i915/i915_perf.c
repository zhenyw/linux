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

static u32 i915_perf_stream_paranoid = true;

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

#define SAMPLE_OA_REPORT      (1<<0)

struct perf_open_properties
{
	u32 sample_flags;

	u64 single_context:1;
	u64 ctx_handle;

	/* OA sampling state */
	int metrics_set;
	int oa_format;
	bool oa_periodic;
	u32 oa_period_exponent;
};

static int read_properties(struct drm_i915_private *dev_priv,
			   u64 __user *uprops,
			   u32 n_props,
			   struct perf_open_properties *props)
{
	u64 __user *uprop = uprops;
	int i;

	memset(props, 0, sizeof(struct perf_open_properties));

	if (!n_props) {
		DRM_ERROR("No i915 perf properties given");
		return -EINVAL;
	}

	if (n_props > DRM_I915_PERF_PROP_MAX) {
		DRM_ERROR("More i915 perf properties specified than exist");
		return -EINVAL;
	}

	for (i = 0; i < n_props; i++) {
		u64 id, value;
		int ret;

		ret = get_user(id, (u64 __user *)uprop);
		if (ret)
			return ret;

		if (id == 0 || id >= DRM_I915_PERF_PROP_MAX) {
			DRM_ERROR("Unknown i915 perf property ID");
			return -EINVAL;
		}

		ret = get_user(value, (u64 __user *)uprop + 1);
		if (ret)
			return ret;

		switch ((enum drm_i915_perf_property_id)id) {
		case DRM_I915_PERF_CTX_HANDLE_PROP:
			props->single_context = 1;
			props->ctx_handle = value;
			break;
		case DRM_I915_PERF_SAMPLE_OA_PROP:
			props->sample_flags |= SAMPLE_OA_REPORT;
			break;
		case DRM_I915_PERF_OA_METRICS_SET_PROP:
			if (value == 0 || value >= I915_OA_METRICS_SET_MAX) {
				DRM_ERROR("Unknown OA metric set ID");
				return -EINVAL;
			}
			props->metrics_set = value;
			break;
		case DRM_I915_PERF_OA_FORMAT_PROP:
			if (value == 0 || value >= I915_OA_FORMAT_MAX) {
				DRM_ERROR("Invalid OA report format\n");
				return -EINVAL;
			}
			if (!dev_priv->perf.oa.oa_formats[value].size) {
				DRM_ERROR("Invalid OA report format\n");
				return -EINVAL;
			}
			props->oa_format = value;
			break;
		case DRM_I915_PERF_OA_EXPONENT_PROP:
			if (value > OA_EXPONENT_MAX)
				return -EINVAL;

			/* NB: The exponent represents a period as follows:
			 *
			 *   80ns * 2^(period_exponent + 1)
			 *
			 * Theoretically we can program the OA unit to sample
			 * every 160ns but don't allow that by default unless
			 * root.
			 *
			 * Referring to perf's
			 * kernel.perf_event_max_sample_rate for a precedent
			 * (100000 by default); with an OA exponent of 6 we get
			 * a period of 10.240 microseconds -just under 100000Hz
			 */
			if (value < 6 && !capable(CAP_SYS_ADMIN)) {
				DRM_ERROR("Sampling period too high without root privileges\n");
				return -EACCES;
			}

			props->oa_periodic = true;
			props->oa_period_exponent = value;
			break;
		case DRM_I915_PERF_PROP_MAX:
			BUG();
		}

		uprop += 2;
	}

	return 0;
}

static bool gen7_oa_buffer_is_empty(struct drm_i915_private *dev_priv)
{
	u32 oastatus2 = I915_READ(GEN7_OASTATUS2);
	u32 oastatus1 = I915_READ(GEN7_OASTATUS1);
	u32 head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	u32 tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	return OA_TAKEN(tail, head) == 0;
}

static bool append_oa_status(struct i915_perf_stream *stream,
			     struct i915_perf_read_state *read_state,
			     enum drm_i915_perf_record_type type)
{
	struct drm_i915_perf_record_header header = { type, 0, sizeof(header) };

	if ((read_state->count - read_state->read) < header.size)
		return false;

	copy_to_user(read_state->buf, &header, sizeof(header));

	read_state->buf += sizeof(header);
	read_state->read += header.size;

	return true;
}

static bool append_oa_sample(struct i915_perf_stream *stream,
			     struct i915_perf_read_state *read_state,
			     const u8 *report)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	struct drm_i915_perf_record_header header;
	u32 sample_flags = stream->sample_flags;

	header.type = DRM_I915_PERF_RECORD_SAMPLE;
	header.pad = 0;
	header.size = stream->sample_size;

	if ((read_state->count - read_state->read) < header.size)
		return false;

	copy_to_user(read_state->buf, &header, sizeof(header));
	read_state->buf += sizeof(header);

	if (sample_flags & SAMPLE_OA_REPORT) {
		copy_to_user(read_state->buf, report, report_size);
		read_state->buf += report_size;
	}

	read_state->read += header.size;

	return true;
}

static u32 gen7_append_oa_reports(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state,
				  u32 head,
				  u32 tail)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
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

		if (dev_priv->perf.oa.exclusive_stream->enabled) {
			if (!append_oa_sample(stream, read_state, report))
				break;
		}

		/* If append_oa_sample() returns false we shouldn't progress
		 * head so we update it afterwards... */
		head += report_size;
	}

	return dev_priv->perf.oa.oa_buffer.gtt_offset + head;
}

static void gen7_oa_read(struct i915_perf_stream *stream,
			 struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
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
			if (append_oa_status(stream, read_state,
					     DRM_I915_PERF_RECORD_OA_BUFFER_OVERFLOW))
				oastatus1 &= ~GEN7_OASTATUS1_OABUFFER_OVERFLOW;
		}

		if (oastatus1 & GEN7_OASTATUS1_REPORT_LOST) {
			if (append_oa_status(stream, read_state,
					     DRM_I915_PERF_RECORD_OA_REPORT_LOST))
				oastatus1 &= ~GEN7_OASTATUS1_REPORT_LOST;
		}

		I915_WRITE(GEN7_OASTATUS1, oastatus1);
	}

	head = gen7_append_oa_reports(stream, read_state, head, tail);

	I915_WRITE(GEN7_OASTATUS2, (head & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);
}

static bool i915_oa_can_read(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	return !dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv);
}

static int i915_oa_wait_unlocked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	/* Note: the oa_buffer_is_empty() condition is ok to run unlocked as it
	 * just performs mmio reads of the OA buffer head + tail pointers and
	 * it's assumed we're handling some operation that implies the stream
	 * can't be destroyed until completion (such as a read()) that ensures
	 * the device + OA buffer can't disappear
	 */
	return wait_event_interruptible(dev_priv->perf.oa.poll_wq,
					!dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv));
}

static void i915_oa_poll_wait(struct i915_perf_stream *stream,
			      struct file *file,
			      poll_table *wait)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	poll_wait(file, &dev_priv->perf.oa.poll_wq, wait);
}

static void i915_oa_read(struct i915_perf_stream *stream,
			 struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	dev_priv->perf.oa.ops.read(stream, read_state);
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

static void i915_oa_stream_destroy(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	BUG_ON(stream != dev_priv->perf.oa.exclusive_stream);

	dev_priv->perf.oa.ops.disable_metric_set(dev_priv);

	free_oa_buffer(dev_priv);

	intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	intel_runtime_pm_put(dev_priv);

	dev_priv->perf.oa.exclusive_stream = NULL;
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

static int hsw_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_hsw(dev_priv);

	if (ret)
		return ret;

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

	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	return 0;
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
	if (dev_priv->perf.oa.exclusive_stream->enabled) {
		unsigned long ctx_id = 0;
		bool pinning_ok = false;

		if (dev_priv->perf.oa.exclusive_stream->ctx &&
		    dev_priv->perf.oa.specific_ctx_id) {
			ctx_id = dev_priv->perf.oa.specific_ctx_id;
			pinning_ok = true;
		}

		if (dev_priv->perf.oa.exclusive_stream->ctx == NULL ||
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

static void i915_oa_stream_enable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

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

static void i915_oa_stream_disable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	dev_priv->perf.oa.ops.oa_disable(dev_priv);

	if (dev_priv->perf.oa.periodic)
		hrtimer_cancel(&dev_priv->perf.oa.poll_check_timer);
}

static int i915_oa_stream_init(struct i915_perf_stream *stream,
			      struct drm_i915_perf_open_param *param,
			      struct perf_open_properties *props)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int ret;

	if (!dev_priv->perf.oa.ops.init_oa_buffer) {
		DRM_ERROR("OA unit not supported\n");
		return -ENODEV;
	}

	/* To avoid the complexity of having to accurately filter
	 * counter reports and marshal to the appropriate client
	 * we currently only allow exclusive access */
	if (dev_priv->perf.oa.exclusive_stream) {
		DRM_ERROR("OA unit already in use\n");
		return -EBUSY;
	}

	dev_priv->perf.oa.oa_buffer.format_size =
		dev_priv->perf.oa.oa_formats[props->oa_format].size;
	BUG_ON(dev_priv->perf.oa.oa_buffer.format_size == 0);

	dev_priv->perf.oa.oa_buffer.format =
		dev_priv->perf.oa.oa_formats[props->oa_format].format;

	dev_priv->perf.oa.metrics_set = props->metrics_set;

	dev_priv->perf.oa.periodic = props->oa_periodic;
	if (dev_priv->perf.oa.periodic)
		dev_priv->perf.oa.period_exponent = props->oa_period_exponent;

	ret = alloc_oa_buffer(dev_priv);
	if (ret)
		return ret;

	dev_priv->perf.oa.exclusive_stream = stream;

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

	stream->destroy = i915_oa_stream_destroy;
	stream->enable = i915_oa_stream_enable;
	stream->disable = i915_oa_stream_disable;
	stream->can_read = i915_oa_can_read;
	stream->wait_unlocked = i915_oa_wait_unlocked;
	stream->poll_wait = i915_oa_poll_wait;
	stream->read = i915_oa_read;

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

	if (dev_priv->perf.oa.exclusive_stream &&
	    dev_priv->perf.oa.exclusive_stream->ctx == context) {
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

static ssize_t i915_perf_read_locked(struct i915_perf_stream *stream,
				     struct file *file,
				     char __user *buf,
				     size_t count,
				     loff_t *ppos)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	struct i915_perf_read_state state = { count, 0, buf };
	int ret;

	if (file->f_flags & O_NONBLOCK) {
		if (!stream->can_read(stream))
			return -EAGAIN;
	} else {
		mutex_unlock(&dev_priv->perf.lock);
		ret = stream->wait_unlocked(stream);
		mutex_lock(&dev_priv->perf.lock);

		if (ret)
			return ret;
	}

	stream->read(stream, &state);
	if (state.read == 0)
		return -ENOSPC;

	return state.read;
}

static ssize_t i915_perf_read(struct file *file,
			      char __user *buf,
			      size_t count,
			      loff_t *ppos)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	ssize_t ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_read_locked(stream, file, buf, count, ppos);
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

static unsigned int i915_perf_poll_locked(struct i915_perf_stream *stream,
					  struct file *file,
					  poll_table *wait)
{
	unsigned int streams = 0;

	stream->poll_wait(stream, file, wait);

	if (stream->can_read(stream))
		streams |= POLLIN;

	return streams;
}

static unsigned int i915_perf_poll(struct file *file, poll_table *wait)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_poll_locked(stream, file, wait);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static void i915_perf_enable_locked(struct i915_perf_stream *stream)
{
	if (stream->enabled)
		return;

	/* Allow stream->enable() to refer to this */
	stream->enabled = true;

	if (stream->enable)
		stream->enable(stream);
}

static void i915_perf_disable_locked(struct i915_perf_stream *stream)
{
	if (!stream->enabled)
		return;

	/* Allow stream->disable() to refer to this */
	stream->enabled = false;

	if (stream->disable)
		stream->disable(stream);
}

static long i915_perf_ioctl_locked(struct i915_perf_stream *stream,
				   unsigned int cmd,
				   unsigned long arg)
{
	switch (cmd) {
	case I915_PERF_IOCTL_ENABLE:
		i915_perf_enable_locked(stream);
		return 0;
	case I915_PERF_IOCTL_DISABLE:
		i915_perf_disable_locked(stream);
		return 0;
	}

	return -EINVAL;
}

static long i915_perf_ioctl(struct file *file,
			    unsigned int cmd,
			    unsigned long arg)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	long ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_ioctl_locked(stream, cmd, arg);
	mutex_unlock(&dev_priv->perf.lock);

	return ret;
}

static void i915_perf_destroy_locked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (stream->enabled)
		i915_perf_disable_locked(stream);

	if (stream->destroy)
		stream->destroy(stream);

	list_del(&stream->link);

	if (stream->ctx) {
		mutex_lock(&dev_priv->dev->struct_mutex);
		i915_gem_context_unreference(stream->ctx);
		mutex_unlock(&dev_priv->dev->struct_mutex);
	}

	kfree(stream);
}

static int i915_perf_release(struct inode *inode, struct file *file)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;

	mutex_lock(&dev_priv->perf.lock);
	i915_perf_destroy_locked(stream);
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
	struct perf_open_properties props;
	u32 known_open_flags = 0;
	struct intel_context *specific_ctx = NULL;
	struct i915_perf_stream *stream = NULL;
	unsigned long f_flags = 0;
	int stream_fd;
	int ret = 0;

	known_open_flags = I915_PERF_FLAG_FD_CLOEXEC |
			   I915_PERF_FLAG_FD_NONBLOCK |
			   I915_PERF_FLAG_DISABLED;
	if (param->flags & ~known_open_flags) {
		DRM_ERROR("Unknown drm_i915_perf_open_param flag\n");
		ret = -EINVAL;
		goto err;
	}

	ret = read_properties(dev_priv,
			      to_user_ptr(param->properties),
			      param->n_properties,
			      &props);
	if (ret)
		goto err;

	if (props.single_context) {
		u32 ctx_handle = props.ctx_handle;

		specific_ctx = lookup_context(dev_priv, file->filp, ctx_handle);
		if (!specific_ctx) {
			DRM_ERROR("Failed to look up context with ID %u for opening perf stream\n",
				  ctx_handle);
			ret = -EINVAL;
			goto err;
		}
	}

	/* Similar to perf's kernel.perf_paranoid_cpu sysctl option
	 * we check a dev.i915.perf_stream_paranoid sysctl option
	 * to determine if it's ok to access system wide OA counters
	 * without CAP_SYS_ADMIN privileges.
	 */
	if (!specific_ctx &&
	    i915_perf_stream_paranoid && !capable(CAP_SYS_ADMIN)) {
		DRM_ERROR("Insufficient privileges to open system-wide i915 perf stream\n");
		ret = -EACCES;
		goto err_ctx;
	}

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	stream->sample_flags = props.sample_flags;

	stream->sample_size = sizeof(struct drm_i915_perf_record_header);

	if (props.sample_flags & SAMPLE_OA_REPORT) {
		int report_size =
			dev_priv->perf.oa.oa_formats[props.oa_format].size;

		stream->sample_size += report_size;
	}

	stream->dev_priv = dev_priv;
	stream->ctx = specific_ctx;

	if (stream->sample_flags & SAMPLE_OA_REPORT) {
		ret = i915_oa_stream_init(stream, param, &props);
		if (ret)
			goto err_alloc;
	} else {
		DRM_ERROR("Unsupported i915 perf stream configuration\n");
		ret = -EINVAL;
		goto err_alloc;
	}

	stream->ctx = specific_ctx;
	list_add(&stream->link, &dev_priv->perf.streams);

	if (param->flags & I915_PERF_FLAG_FD_CLOEXEC)
		f_flags |= O_CLOEXEC;
	if (param->flags & I915_PERF_FLAG_FD_NONBLOCK)
		f_flags |= O_NONBLOCK;

	stream_fd = anon_inode_getfd("[i915_perf]", &fops, stream, f_flags);
	if (stream_fd < 0) {
		ret = stream_fd;
		goto err_open;
	}

	param->fd = stream_fd;

	if (!(param->flags & I915_PERF_FLAG_DISABLED))
		i915_perf_enable_locked(stream);

	return 0;

err_open:
	list_del(&stream->link);
	if (stream->destroy)
		stream->destroy(stream);
err_alloc:
	kfree(stream);
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


static struct ctl_table oa_table[] = {
	{
	 .procname = "perf_stream_paranoid",
	 .data = &i915_perf_stream_paranoid,
	 .maxlen = sizeof(i915_perf_stream_paranoid),
	 .mode = 0644,
	 .proc_handler = proc_dointvec,
	 },
	{}
};

static struct ctl_table i915_root[] = {
	{
	 .procname = "i915",
	 .maxlen = 0,
	 .mode = 0555,
	 .child = oa_table,
	 },
	{}
};

static struct ctl_table dev_root[] = {
	{
	 .procname = "dev",
	 .maxlen = 0,
	 .mode = 0555,
	 .child = i915_root,
	 },
	{}
};

void i915_perf_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!IS_HASWELL(dev))
		return;

	dev_priv->perf.sysctl_header = register_sysctl_table(dev_root);

	hrtimer_init(&dev_priv->perf.oa.poll_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_priv->perf.oa.poll_check_timer.function = poll_check_timer_cb;
	init_waitqueue_head(&dev_priv->perf.oa.poll_wq);

	INIT_LIST_HEAD(&dev_priv->perf.streams);
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

	unregister_sysctl_table(dev_priv->perf.sysctl_header);

	dev_priv->perf.oa.ops.init_oa_buffer = NULL;

	dev_priv->perf.initialized = false;
}
