#include <linux/perf_event.h>
#include <linux/sizes.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"

/* Must be a power of two */
#define OA_BUFFER_SIZE	     SZ_16M
#define OA_TAKEN(tail, head) ((tail - head) & (OA_BUFFER_SIZE - 1))

#define FREQUENCY 200
#define PERIOD max_t(u64, 10000, NSEC_PER_SEC / FREQUENCY)

static int hsw_perf_format_sizes[] = {
	64,  /* A13_HSW */
	128, /* A29_HSW */
	128, /* A13_B8_C8_HSW */
	-1,  /* Disallowed since 192 bytes doesn't factor into buffer size
		(A29_B8_C8_HSW) */
	64,  /* B4_C8_HSW */
	256, /* A45_B8_C8_HSW */
	128, /* B4_C8_A16_HSW */
	64   /* C4_B8_HSW */
};

static void forward_one_oa_snapshot_to_event(struct drm_i915_private *dev_priv,
					     u8 *snapshot,
					     struct perf_event *event)
{
	struct perf_sample_data data;
	int snapshot_size = dev_priv->oa_pmu.oa_buffer.format_size;
	struct perf_raw_record raw;

	WARN_ON(snapshot_size == 0);

	perf_sample_data_init(&data, 0, event->hw.last_period);

	/* Note: the combined u32 raw->size member + raw data itself must be 8
	 * byte aligned. (See note in init_oa_buffer for more details) */
	raw.size = snapshot_size + 4;
	raw.data = snapshot;

	data.raw = &raw;

	perf_event_overflow(event, &data, &dev_priv->oa_pmu.dummy_regs);
}

static u32 forward_oa_snapshots(struct drm_i915_private *dev_priv,
				u32 head,
				u32 tail)
{
	struct perf_event *exclusive_event = dev_priv->oa_pmu.exclusive_event;
	int snapshot_size = dev_priv->oa_pmu.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->oa_pmu.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u8 *snapshot;
	u32 taken;

	head -= dev_priv->oa_pmu.oa_buffer.gtt_offset;
	tail -= dev_priv->oa_pmu.oa_buffer.gtt_offset;

	/* Note: the gpu doesn't wrap the tail according to the OA buffer size
	 * so when we need to make sure our head/tail values are in-bounds we
	 * use the above mask.
	 */

	while ((taken = OA_TAKEN(tail, head))) {
		/* The tail increases in 64 byte increments, not in
		 * format_size steps. */
		if (taken < snapshot_size)
			break;

		snapshot = oa_buf_base + (head & mask);
		head += snapshot_size;

		/* We currently only allow exclusive access to the counters
		 * so only have one event to forward too... */
		if (dev_priv->oa_pmu.event_active)
			forward_one_oa_snapshot_to_event(dev_priv, snapshot,
							 exclusive_event);
	}

	return dev_priv->oa_pmu.oa_buffer.gtt_offset + head;
}

static void flush_oa_snapshots(struct drm_i915_private *dev_priv,
			       bool skip_if_flushing)
{
	unsigned long flags;
	u32 oastatus2;
	u32 oastatus1;
	u32 head;
	u32 tail;

	/* Can either flush via hrtimer callback or pmu methods/fops */
	if (skip_if_flushing) {

		/* If the hrtimer triggers at the same time that we are
		 * responding to a userspace initiated flush then we can
		 * just bail out...
		 */
		if (!spin_trylock_irqsave(&dev_priv->oa_pmu.oa_buffer.flush_lock,
					  flags))
			return;
	} else
		spin_lock_irqsave(&dev_priv->oa_pmu.oa_buffer.flush_lock, flags);

	WARN_ON(!dev_priv->oa_pmu.oa_buffer.addr);

	oastatus2 = I915_READ(GEN7_OASTATUS2);
	oastatus1 = I915_READ(GEN7_OASTATUS1);

	head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	if (oastatus1 & (GEN7_OASTATUS1_OABUFFER_OVERFLOW |
			 GEN7_OASTATUS1_REPORT_LOST)) {

		/* XXX: How can we convey report-lost errors to userspace?  It
		 * doesn't look like perf's _REPORT_LOST mechanism is
		 * appropriate in this case; that's just for cases where we
		 * run out of space for samples in the perf circular buffer.
		 *
		 * Maybe we can claim a special report-id and use that to
		 * forward status flags?
		 */
		pr_debug("OA buffer read error: addr = %p, head = %u, offset = %u, tail = %u cnt o'flow = %d, buf o'flow = %d, rpt lost = %d\n",
			 dev_priv->oa_pmu.oa_buffer.addr,
			 head,
			 head - dev_priv->oa_pmu.oa_buffer.gtt_offset,
			 tail,
			 oastatus1 & GEN7_OASTATUS1_COUNTER_OVERFLOW ? 1 : 0,
			 oastatus1 & GEN7_OASTATUS1_OABUFFER_OVERFLOW ? 1 : 0,
			 oastatus1 & GEN7_OASTATUS1_REPORT_LOST ? 1 : 0);

		I915_WRITE(GEN7_OASTATUS1, oastatus1 &
			   ~(GEN7_OASTATUS1_OABUFFER_OVERFLOW |
			     GEN7_OASTATUS1_REPORT_LOST));
	}

	head = forward_oa_snapshots(dev_priv, head, tail);

	I915_WRITE(GEN7_OASTATUS2, (head & GEN7_OASTATUS2_HEAD_MASK) |
				    GEN7_OASTATUS2_GGTT);

	spin_unlock_irqrestore(&dev_priv->oa_pmu.oa_buffer.flush_lock, flags);
}

static void
oa_buffer_destroy(struct drm_i915_private *i915)
{
	mutex_lock(&i915->dev->struct_mutex);

	vunmap(i915->oa_pmu.oa_buffer.addr);
	i915_gem_object_ggtt_unpin(i915->oa_pmu.oa_buffer.obj);
	drm_gem_object_unreference(&i915->oa_pmu.oa_buffer.obj->base);

	i915->oa_pmu.oa_buffer.obj = NULL;
	i915->oa_pmu.oa_buffer.gtt_offset = 0;
	i915->oa_pmu.oa_buffer.addr = NULL;

	mutex_unlock(&i915->dev->struct_mutex);
}

static void i915_oa_event_destroy(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), oa_pmu.pmu);
	unsigned long lock_flags;

	WARN_ON(event->parent);

	/* Stop updating oacontrol via _oa_context_pin_[un]notify()... */
	spin_lock_irqsave(&i915->oa_pmu.lock, lock_flags);
	i915->oa_pmu.specific_ctx = NULL;
	spin_unlock_irqrestore(&i915->oa_pmu.lock, lock_flags);

	/* Don't let the compiler start resetting OA, PM and clock gating
	 * state before we've stopped update_oacontrol()
	 */
	barrier();

	oa_buffer_destroy(i915);

	BUG_ON(i915->oa_pmu.exclusive_event != event);
	i915->oa_pmu.exclusive_event = NULL;

	intel_uncore_forcewake_put(i915, FORCEWAKE_ALL);
	intel_runtime_pm_put(i915);
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

static int init_oa_buffer(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	struct drm_i915_gem_object *bo;
	int ret;

	BUG_ON(!IS_HASWELL(dev_priv->dev));
	BUG_ON(mutex_is_locked(&dev_priv->dev->struct_mutex));
	BUG_ON(dev_priv->oa_pmu.oa_buffer.obj);

	ret = i915_mutex_lock_interruptible(dev_priv->dev);
	if (ret)
		return ret;

	spin_lock_init(&dev_priv->oa_pmu.oa_buffer.flush_lock);

	/* NB: We over allocate the OA buffer due to the way raw sample data
	 * gets copied from the gpu mapped circular buffer into the perf
	 * circular buffer so that only one copy is required.
	 *
	 * For each perf sample (raw->size + 4) needs to be 8 byte aligned,
	 * where the 4 corresponds to the 32bit raw->size member that's
	 * added to the sample header that userspace sees.
	 *
	 * Due to the + 4 for the size member: when we copy a report to the
	 * userspace facing perf buffer we always copy an additional 4 bytes
	 * from the subsequent report to make up for the miss alignment, but
	 * when a report is at the end of the gpu mapped buffer we need to
	 * read 4 bytes past the end of the buffer.
	 */
	bo = i915_gem_alloc_object(dev_priv->dev, OA_BUFFER_SIZE + PAGE_SIZE);
	if (bo == NULL) {
		DRM_ERROR("Failed to allocate OA buffer\n");
		ret = -ENOMEM;
		goto unlock;
	}
	dev_priv->oa_pmu.oa_buffer.obj = bo;

	ret = i915_gem_object_set_cache_level(bo, I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	/* PreHSW required 512K alignment, HSW requires 16M */
	ret = i915_gem_obj_ggtt_pin(bo, SZ_16M, 0);
	if (ret)
		goto err_unref;

	dev_priv->oa_pmu.oa_buffer.gtt_offset = i915_gem_obj_ggtt_offset(bo);
	dev_priv->oa_pmu.oa_buffer.addr = vmap_oa_buffer(bo);

	/* Pre-DevBDW: OABUFFER must be set with counters off,
	 * before OASTATUS1, but after OASTATUS2 */
	I915_WRITE(GEN7_OASTATUS2, dev_priv->oa_pmu.oa_buffer.gtt_offset |
		   GEN7_OASTATUS2_GGTT); /* head */
	I915_WRITE(GEN7_OABUFFER, dev_priv->oa_pmu.oa_buffer.gtt_offset);
	I915_WRITE(GEN7_OASTATUS1, dev_priv->oa_pmu.oa_buffer.gtt_offset |
		   GEN7_OASTATUS1_OABUFFER_SIZE_16M); /* tail */

	DRM_DEBUG_DRIVER("OA Buffer initialized, gtt offset = 0x%x, vaddr = %p",
			 dev_priv->oa_pmu.oa_buffer.gtt_offset,
			 dev_priv->oa_pmu.oa_buffer.addr);

	goto unlock;

err_unref:
	drm_gem_object_unreference(&bo->base);

unlock:
	mutex_unlock(&dev_priv->dev->struct_mutex);
	return ret;
}

static enum hrtimer_restart hrtimer_sample(struct hrtimer *hrtimer)
{
	struct drm_i915_private *i915 =
		container_of(hrtimer, typeof(*i915), oa_pmu.timer);

	flush_oa_snapshots(i915, true);

	hrtimer_forward_now(hrtimer, ns_to_ktime(PERIOD));
	return HRTIMER_RESTART;
}

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
			mutex_unlock(&dev_priv->dev->struct_mutex);
			return ctx;
		}
	}
	mutex_unlock(&dev_priv->dev->struct_mutex);

	return NULL;
}

static int i915_oa_copy_attr(drm_i915_oa_attr_t __user *uattr,
			     drm_i915_oa_attr_t *attr)
{
	u32 size;
	int ret;

	if (!access_ok(VERIFY_WRITE, uattr, I915_OA_ATTR_SIZE_VER0))
		return -EFAULT;

	/*
	 * zero the full structure, so that a short copy will be nice.
	 */
	memset(attr, 0, sizeof(*attr));

	ret = get_user(size, &uattr->size);
	if (ret)
		return ret;

	if (size > PAGE_SIZE)	/* silly large */
		goto err_size;

	if (size < I915_OA_ATTR_SIZE_VER0)
		goto err_size;

	/*
	 * If we're handed a bigger struct than we know of,
	 * ensure all the unknown bits are 0 - i.e. new
	 * user-space does not rely on any kernel feature
	 * extensions we dont know about yet.
	 */
	if (size > sizeof(*attr)) {
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

	if (attr->__reserved_1)
		return -EINVAL;

out:
	return ret;

err_size:
	put_user(sizeof(*attr), &uattr->size);
	ret = -E2BIG;
	goto out;
}

static int i915_oa_event_init(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	drm_i915_oa_attr_t oa_attr;
	u64 report_format;
	int ret = 0;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	ret = i915_oa_copy_attr(to_user_ptr(event->attr.config), &oa_attr);
	if (ret)
		return ret;

	/* To avoid the complexity of having to accurately filter
	 * counter snapshots and marshal to the appropriate client
	 * we currently only allow exclusive access */
	if (dev_priv->oa_pmu.oa_buffer.obj)
		return -EBUSY;

	report_format = oa_attr.format;
	dev_priv->oa_pmu.oa_buffer.format = report_format;
	dev_priv->oa_pmu.metrics_set = oa_attr.metrics_set;

	if (IS_HASWELL(dev_priv->dev)) {
		int snapshot_size;

		if (report_format >= ARRAY_SIZE(hsw_perf_format_sizes))
			return -EINVAL;

		snapshot_size = hsw_perf_format_sizes[report_format];
		if (snapshot_size < 0)
			return -EINVAL;

		dev_priv->oa_pmu.oa_buffer.format_size = snapshot_size;
	} else {
		BUG(); /* pmu shouldn't have been registered */
		return -ENODEV;
	}

	/* Since we are limited to an exponential scale for
	 * programming the OA sampling period we don't allow userspace
	 * to pass a precise attr.sample_period. */
	if (event->attr.freq ||
	    (event->attr.sample_period != 0 &&
	     event->attr.sample_period != 1))
		return -EINVAL;

	dev_priv->oa_pmu.periodic = event->attr.sample_period;

	/* Instead of allowing userspace to configure the period via
	 * attr.sample_period we instead accept an exponent whereby
	 * the sample_period will be:
	 *
	 *   80ns * 2^(period_exponent + 1)
	 *
	 * Programming a period of 160 nanoseconds would not be very
	 * polite, so higher frequencies are reserved for root.
	 */
	if (dev_priv->oa_pmu.periodic) {
		u64 period_exponent = oa_attr.timer_exponent;

		if (period_exponent > 63)
			return -EINVAL;

		if (period_exponent < 15 && !capable(CAP_SYS_ADMIN))
			return -EACCES;

		dev_priv->oa_pmu.period_exponent = period_exponent;
	} else if (oa_attr.timer_exponent)
		return -EINVAL;

	/* We bypass the default perf core perf_paranoid_cpu() ||
	 * CAP_SYS_ADMIN check by using the PERF_PMU_CAP_IS_DEVICE
	 * flag and instead authenticate based on whether the current
	 * pid owns the specified context, or require CAP_SYS_ADMIN
	 * when collecting cross-context metrics.
	 */
	dev_priv->oa_pmu.specific_ctx = NULL;
	if (oa_attr.single_context) {
		u32 ctx_id = oa_attr.ctx_id;
		unsigned int drm_fd = oa_attr.drm_fd;
		struct fd fd = fdget(drm_fd);

		if (!fd.file)
			return -EBADF;

		dev_priv->oa_pmu.specific_ctx =
			lookup_context(dev_priv, fd.file, ctx_id);
		fdput(fd);

		if (!dev_priv->oa_pmu.specific_ctx)
			return -EINVAL;
	}

	if (!dev_priv->oa_pmu.specific_ctx && !capable(CAP_SYS_ADMIN))
		return -EACCES;

	ret = init_oa_buffer(event);
	if (ret)
		return ret;

	BUG_ON(dev_priv->oa_pmu.exclusive_event);
	dev_priv->oa_pmu.exclusive_event = event;

	event->destroy = i915_oa_event_destroy;

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
	 *   references will effectively disable RC6 and trunk clock
	 *   gating.
	 */
	intel_runtime_pm_get(dev_priv);
	intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

	return 0;
}

/* Note: Although pmu methods are called with the corresponding
 * perf_event_context lock taken (so we don't need to worry about our pmu
 * methods contending with each other) update_oacontrol() may be called
 * asynchronously via the i915_oa_pmu_[un]register() hooks.
 */
static void update_oacontrol(struct drm_i915_private *dev_priv)
{
	BUG_ON(!spin_is_locked(&dev_priv->oa_pmu.lock));

	if (dev_priv->oa_pmu.event_active) {
		unsigned long ctx_id = 0;
		bool pinning_ok = false;

		if (dev_priv->oa_pmu.specific_ctx) {
			struct intel_context *ctx =
				dev_priv->oa_pmu.specific_ctx;
			struct drm_i915_gem_object *obj =
				ctx->legacy_hw_ctx.rcs_state;

			if (i915_gem_obj_is_pinned(obj)) {
				ctx_id = i915_gem_obj_ggtt_offset(obj);
				pinning_ok = true;
			}
		}

		if ((ctx_id == 0 || pinning_ok)) {
			bool periodic = dev_priv->oa_pmu.periodic;
			u32 period_exponent = dev_priv->oa_pmu.period_exponent;
			u32 report_format = dev_priv->oa_pmu.oa_buffer.format;

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

static void i915_oa_event_start(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;
	u32 oastatus1, tail;

	/* PRM - observability performance counters:
	 *
	 *   OACONTROL, specific context enable:
	 *
	 *   "OA unit level clock gating must be ENABLED when using
	 *   specific ContextID feature."
	 *
	 * Assuming we don't ever disable OA unit level clock gating
	 * lets just assert that this condition is met...
	 */
	WARN_ONCE(I915_READ(GEN6_UCGCTL3) & GEN6_OACSUNIT_CLOCK_GATE_DISABLE,
		  "disabled OA unit level clock gating will result in incorrect per-context OA counters");

	/* XXX: On Haswell, when threshold disable mode is desired,
	 * instead of setting the threshold enable to '0', we need to
	 * program it to '1' and set OASTARTTRIG1 bits 15:0 to 0
	 * (threshold value of 0)
	 */
	I915_WRITE(OASTARTTRIG6, (OASTARTTRIG6_B4_TO_B7_THRESHOLD_ENABLE |
				  OASTARTTRIG6_B4_CUSTOM_EVENT_ENABLE));
	I915_WRITE(OASTARTTRIG5, 0); /* threshold value */

	I915_WRITE(OASTARTTRIG2, (OASTARTTRIG2_B0_TO_B3_THRESHOLD_ENABLE |
				  OASTARTTRIG2_B0_CUSTOM_EVENT_ENABLE));
	I915_WRITE(OASTARTTRIG1, 0); /* threshold value */

	/* Setup B0 as the gpu clock counter... */
	I915_WRITE(OACEC0_0, OACEC0_0_B0_COMPARE_GREATER_OR_EQUAL); /* to 0 */
	I915_WRITE(OACEC0_1, 0xfffe); /* Select NOA[0] */

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);

	dev_priv->oa_pmu.event_active = true;
	update_oacontrol(dev_priv);

	/* Reset the head ptr to ensure we don't forward reports relating
	 * to a previous perf event */
	oastatus1 = I915_READ(GEN7_OASTATUS1);
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	I915_WRITE(GEN7_OASTATUS2, (tail & GEN7_OASTATUS2_HEAD_MASK) |
				    GEN7_OASTATUS2_GGTT);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	if (event->attr.sample_period)
		__hrtimer_start_range_ns(&dev_priv->oa_pmu.timer,
					 ns_to_ktime(PERIOD), 0,
					 HRTIMER_MODE_REL_PINNED, 0);

	event->hw.state = 0;
}

static void i915_oa_event_stop(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);

	dev_priv->oa_pmu.event_active = false;
	update_oacontrol(dev_priv);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	if (event->attr.sample_period) {
		hrtimer_cancel(&dev_priv->oa_pmu.timer);
		flush_oa_snapshots(dev_priv, false);
	}

	event->hw.state = PERF_HES_STOPPED;
}

static int i915_oa_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		i915_oa_event_start(event, flags);

	return 0;
}

static void i915_oa_event_del(struct perf_event *event, int flags)
{
	i915_oa_event_stop(event, flags);
}

static void i915_oa_event_read(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), oa_pmu.pmu);

	/* XXX: What counter would be useful here? */
	local64_set(&event->count, 0);
}

static int i915_oa_event_flush(struct perf_event *event)
{
	if (event->attr.sample_period) {
		struct drm_i915_private *i915 =
			container_of(event->pmu, typeof(*i915), oa_pmu.pmu);

		flush_oa_snapshots(i915, true);
	}

	return 0;
}

static int i915_oa_event_event_idx(struct perf_event *event)
{
	return 0;
}

void i915_oa_context_pin_notify(struct drm_i915_private *dev_priv,
				struct intel_context *context)
{
	unsigned long flags;

	if (dev_priv->oa_pmu.pmu.event_init == NULL)
		return;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, flags);

	if (dev_priv->oa_pmu.specific_ctx == context)
		update_oacontrol(dev_priv);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, flags);
}

void i915_oa_context_unpin_notify(struct drm_i915_private *dev_priv,
				  struct intel_context *context)
{
	unsigned long flags;

	if (dev_priv->oa_pmu.pmu.event_init == NULL)
		return;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, flags);

	if (dev_priv->oa_pmu.specific_ctx == context)
		update_oacontrol(dev_priv);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, flags);
}

void i915_oa_pmu_register(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	if (!IS_HASWELL(dev))
		return;

	/* We need to be careful about forwarding cpu metrics to
	 * userspace considering that PERF_PMU_CAP_IS_DEVICE bypasses
	 * the events/core security check that stops an unprivileged
	 * process collecting metrics for other processes.
	 */
	i915->oa_pmu.dummy_regs = *task_pt_regs(current);

	hrtimer_init(&i915->oa_pmu.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	i915->oa_pmu.timer.function = hrtimer_sample;

	spin_lock_init(&i915->oa_pmu.lock);

	i915->oa_pmu.pmu.capabilities  = PERF_PMU_CAP_IS_DEVICE;

	/* Effectively disallow opening an event with a specific pid
	 * since we aren't interested in processes running on the cpu...
	 */
	i915->oa_pmu.pmu.task_ctx_nr   = perf_invalid_context;

	i915->oa_pmu.pmu.event_init    = i915_oa_event_init;
	i915->oa_pmu.pmu.add	       = i915_oa_event_add;
	i915->oa_pmu.pmu.del	       = i915_oa_event_del;
	i915->oa_pmu.pmu.start	       = i915_oa_event_start;
	i915->oa_pmu.pmu.stop	       = i915_oa_event_stop;
	i915->oa_pmu.pmu.read	       = i915_oa_event_read;
	i915->oa_pmu.pmu.flush	       = i915_oa_event_flush;
	i915->oa_pmu.pmu.event_idx     = i915_oa_event_event_idx;

	if (perf_pmu_register(&i915->oa_pmu.pmu, "i915_oa", -1))
		i915->oa_pmu.pmu.event_init = NULL;
}

void i915_oa_pmu_unregister(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	if (i915->oa_pmu.pmu.event_init == NULL)
		return;

	perf_pmu_unregister(&i915->oa_pmu.pmu);
	i915->oa_pmu.pmu.event_init = NULL;
}
