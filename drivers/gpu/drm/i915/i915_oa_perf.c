#include <linux/perf_event.h>
#include <linux/sizes.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"
#include "i915_oa_hsw.h"

/* Must be a power of two */
#define OA_BUFFER_SIZE	     SZ_16M
#define OA_TAKEN(tail, head) ((tail - head) & (OA_BUFFER_SIZE - 1))

/* frequency for forwarding samples from OA to perf buffer */
#define FWD_FREQUENCY 200
#define FWD_PERIOD max_t(u64, 10000, NSEC_PER_SEC / FWD_FREQUENCY)

static u32 i915_oa_event_paranoid = true;

#define OA_EXPONENT_MAX 0x3f

/* for sysctl proc_dointvec_minmax of i915_oa_event_min_timer_exponent */
static int zero;
static int oa_exponent_max OA_EXPONENT_MAX;

/* Theoretically we can program the OA unit to sample every 160ns but don't
 * allow that by default unless root...
 *
 * The period is derived from the exponent as:
 *
 *   period = 80ns * 2^(exponent + 1)
 *
 * With an exponent of 6 we get a period of 10.240 microseconds which is the
 * minumum that can avoid the default kernel.perf_event_max_sample_rate
 * threshold of 100000 samples/s
 */
static u32 i915_oa_event_min_timer_exponent = 6;

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

static void log_oa_status(struct drm_i915_private *dev_priv,
			  enum drm_i915_oa_event_type status)
{
	struct {
		struct perf_event_header header;
		drm_i915_oa_event_header_t i915_oa_header;
	} oa_event;
	struct perf_output_handle handle;
	struct perf_sample_data sample_data;
	struct perf_event *event = dev_priv->oa_pmu.exclusive_event;
	int ret;

	oa_event.header.size = sizeof(oa_event);
	oa_event.header.type = PERF_RECORD_DEVICE;
	oa_event.i915_oa_header.type = status;
	oa_event.i915_oa_header.__reserved_1 = 0;

	perf_event_header__init_id(&oa_event.header, &sample_data, event);

	ret = perf_output_begin(&handle, event, oa_event.header.size);
	if (ret)
		return;

	perf_output_put(&handle, oa_event);
	perf_event__output_id_sample(event, &handle, &sample_data);
	perf_output_end(&handle);
}
static u32 gen7_forward_oa_snapshots(struct drm_i915_private *dev_priv,
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

static void gen7_flush_oa_snapshots(struct drm_i915_private *dev_priv,
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

	if (unlikely(oastatus1 & (GEN7_OASTATUS1_OABUFFER_OVERFLOW |
				  GEN7_OASTATUS1_REPORT_LOST))) {

		if (oastatus1 & GEN7_OASTATUS1_OABUFFER_OVERFLOW)
			log_oa_status(dev_priv, I915_OA_RECORD_BUFFER_OVERFLOW);

		if (oastatus1 & GEN7_OASTATUS1_REPORT_LOST)
			log_oa_status(dev_priv, I915_OA_RECORD_REPORT_LOST);

		I915_WRITE(GEN7_OASTATUS1, oastatus1 &
			   ~(GEN7_OASTATUS1_OABUFFER_OVERFLOW |
			     GEN7_OASTATUS1_REPORT_LOST));
	}

	head = gen7_forward_oa_snapshots(dev_priv, head, tail);

	I915_WRITE(GEN7_OASTATUS2, (head & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);

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
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;

	WARN_ON(event->parent);

	/* Stop updating oacontrol via _oa_context_pin_[un]notify()... */
	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);
	dev_priv->oa_pmu.specific_ctx = NULL;
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	/* Don't let the compiler start resetting OA, PM and clock gating
	 * state before we've stopped update_oacontrol()
	 */
	barrier();

	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));

	I915_WRITE(GDT_CHICKEN_BITS, (I915_READ(GDT_CHICKEN_BITS) &
				      ~GT_NOA_ENABLE));

	oa_buffer_destroy(dev_priv);

	BUG_ON(dev_priv->oa_pmu.exclusive_event != event);
	dev_priv->oa_pmu.exclusive_event = NULL;

	intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	intel_runtime_pm_put(dev_priv);
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

static void gen7_init_oa_buffer(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);

	/* Pre-DevBDW: OABUFFER must be set with counters off,
	 * before OASTATUS1, but after OASTATUS2 */
	I915_WRITE(GEN7_OASTATUS2, dev_priv->oa_pmu.oa_buffer.gtt_offset |
		   OA_MEM_SELECT_GGTT); /* head */
	I915_WRITE(GEN7_OABUFFER, dev_priv->oa_pmu.oa_buffer.gtt_offset);
	I915_WRITE(GEN7_OASTATUS1, dev_priv->oa_pmu.oa_buffer.gtt_offset |
		   OABUFFER_SIZE_16M); /* tail */
}

static int init_oa_buffer(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	struct drm_i915_gem_object *bo;
	int ret;

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

	dev_priv->oa_pmu.ops.init_oa_buffer(event);

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

	i915->oa_pmu.ops.flush_oa_snapshots(i915, true);

	hrtimer_forward_now(hrtimer, ns_to_ktime(FWD_PERIOD));
	return HRTIMER_RESTART;
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

static void gen7_configure_metric_set(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);

	dev_priv->oa_pmu.mux_regs = NULL;
	dev_priv->oa_pmu.mux_regs_len = 0;
	dev_priv->oa_pmu.b_counter_regs = NULL;
	dev_priv->oa_pmu.b_counter_regs_len = 0;

	switch (dev_priv->oa_pmu.metrics_set) {
	case I915_OA_METRICS_SET_3D:
		config_oa_regs(dev_priv, i915_oa_3d_mux_config_hsw,
			       i915_oa_3d_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_3d_b_counter_config_hsw,
			       i915_oa_3d_b_counter_config_hsw_len);
		break;
	case I915_OA_METRICS_SET_COMPUTE:
		config_oa_regs(dev_priv, i915_oa_compute_mux_config_hsw,
			       i915_oa_compute_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_compute_b_counter_config_hsw,
			       i915_oa_compute_b_counter_config_hsw_len);
		break;
	case I915_OA_METRICS_SET_COMPUTE_EXTENDED:
		config_oa_regs(dev_priv, i915_oa_compute_extended_mux_config_hsw,
			       i915_oa_compute_extended_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_compute_extended_b_counter_config_hsw,
			       i915_oa_compute_extended_b_counter_config_hsw_len);
		break;
	case I915_OA_METRICS_SET_MEMORY_READS:
		config_oa_regs(dev_priv, i915_oa_memory_reads_mux_config_hsw,
			       i915_oa_memory_reads_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_memory_reads_b_counter_config_hsw,
			       i915_oa_memory_reads_b_counter_config_hsw_len);
		break;
	case I915_OA_METRICS_SET_MEMORY_WRITES:
		config_oa_regs(dev_priv, i915_oa_memory_writes_mux_config_hsw,
			       i915_oa_memory_writes_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_memory_writes_b_counter_config_hsw,
			       i915_oa_memory_writes_b_counter_config_hsw_len);
		break;
	case I915_OA_METRICS_SET_SAMPLER_BALANCE:
		config_oa_regs(dev_priv, i915_oa_sampler_balance_mux_config_hsw,
			       i915_oa_sampler_balance_mux_config_hsw_len);
		config_oa_regs(dev_priv, i915_oa_sampler_balance_b_counter_config_hsw,
			       i915_oa_sampler_balance_b_counter_config_hsw_len);
		break;
	default:
		BUG();
	}
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

static int i915_oa_event_init(struct perf_event *event)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	drm_i915_oa_attr_t oa_attr;
	u64 report_format;
	int ret = 0;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	ret = perf_copy_attr(to_user_ptr(event->attr.config),
			     &oa_attr,
			     offsetof(drm_i915_oa_attr_t, size),
			     false, /* don't allow a size of zero */
			     I915_OA_ATTR_SIZE_VER0,
			     sizeof(drm_i915_oa_attr_t));
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

		if (oa_attr.metrics_set <= 0 ||
		    oa_attr.metric_set > I915_OA_METRICS_SET_MAX)
			return -EINVAL;
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
	 */
	if (dev_priv->oa_pmu.periodic) {
		u64 period_exponent = oa_attr.timer_exponent;

		if (period_exponent > OA_EXPONENT_MAX)
			return -EINVAL;

		if (period_exponent < i915_oa_event_min_timer_exponent &&
		    !capable(CAP_SYS_ADMIN))
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

	if (!dev_priv->oa_pmu.specific_ctx &&
	    i915_oa_event_paranoid && !capable(CAP_SYS_ADMIN))
		return -EACCES;

	ret = init_oa_buffer(event);
	if (ret)
		return ret;

	BUG_ON(dev_priv->oa_pmu.exclusive_event);
	dev_priv->oa_pmu.exclusive_event = event;


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

	dev_priv->oa_pmu.ops.configure_metric_set(event);

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
	 *   references will effectively disable RC6.
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
static void gen7_update_oacontrol(struct drm_i915_private *dev_priv)
{
	BUG_ON(!spin_is_locked(&dev_priv->oa_pmu.lock));

	if (dev_priv->oa_pmu.event_active) {
		unsigned long ctx_id = 0;
		bool pinning_ok = false;

		if (dev_priv->oa_pmu.specific_ctx &&
		    dev_priv->oa_pmu.specific_ctx_id) {
			ctx_id = dev_priv->oa_pmu.specific_ctx_id;
			pinning_ok = true;
		}

		if (dev_priv->oa_pmu.specific_ctx == NULL || pinning_ok) {
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

static void gen7_event_start(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	u32 oastatus1, tail;

	gen7_update_oacontrol(dev_priv);

	/* Reset the head ptr so we don't forward reports from before now. */
	oastatus1 = I915_READ(GEN7_OASTATUS1);
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	I915_WRITE(GEN7_OASTATUS2, (tail & GEN7_OASTATUS2_HEAD_MASK) |
				    OA_MEM_SELECT_GGTT);
}

static void i915_oa_event_start(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);

	dev_priv->oa_pmu.event_active = true;
	dev_priv->oa_pmu.ops.event_start(event, flags);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	if (event->attr.sample_period)
		__hrtimer_start_range_ns(&dev_priv->oa_pmu.timer,
					 ns_to_ktime(FWD_PERIOD), 0,
					 HRTIMER_MODE_REL_PINNED, 0);

	event->hw.state = 0;
}

static void gen7_event_stop(struct perf_event *event, int flags)
{
       struct drm_i915_private *dev_priv =
               container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);

       I915_WRITE(GEN7_OACONTROL, 0);
}

static void i915_oa_event_stop(struct perf_event *event, int flags)
{
	struct drm_i915_private *dev_priv =
		container_of(event->pmu, typeof(*dev_priv), oa_pmu.pmu);
	unsigned long lock_flags;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, lock_flags);

	dev_priv->oa_pmu.event_active = false;
	dev_priv->oa_pmu.ops.event_stop(event, flags);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, lock_flags);

	if (event->attr.sample_period) {
		hrtimer_cancel(&dev_priv->oa_pmu.timer);
		dev_priv->oa_pmu.ops.flush_oa_snapshots(dev_priv, false);
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

		i915->oa_pmu.ops.flush_oa_snapshots(i915, true);
	}

	return 0;
}

static int i915_oa_event_event_idx(struct perf_event *event)
{
	return 0;
}


static void gen7_context_pin_notify(struct drm_i915_private *dev_priv,
                                    struct intel_context *context)
{
	if (dev_priv->oa_pmu.specific_ctx == context)
		gen7_update_oacontrol(dev_priv);
}

void i915_oa_context_pin_notify(struct drm_i915_private *dev_priv,
				struct intel_context *context)
{
	unsigned long flags;

	if (dev_priv->oa_pmu.pmu.event_init == NULL ||
	    dev_priv->oa_pmu.ops.context_pin_notify == NULL)
		return;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, flags);

	if (dev_priv->oa_pmu.specific_ctx == context) {
		struct intel_context *ctx = dev_priv->oa_pmu.specific_ctx;
		struct drm_i915_gem_object *obj = ctx->legacy_hw_ctx.rcs_state;

		dev_priv->oa_pmu.specific_ctx_id = i915_gem_obj_ggtt_offset(obj);
	}

	dev_priv->oa_pmu.ops.context_pin_notify(dev_priv, context);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, flags);
}

static void gen7_context_unpin_notify(struct drm_i915_private *dev_priv,
                                      struct intel_context *context)
{
       if (dev_priv->oa_pmu.specific_ctx == context) {
	       dev_priv->oa_pmu.specific_ctx_id = 0;
               gen7_update_oacontrol(dev_priv);
       }
}

void i915_oa_context_unpin_notify(struct drm_i915_private *dev_priv,
				  struct intel_context *context)
{
	unsigned long flags;

	if (dev_priv->oa_pmu.pmu.event_init == NULL ||
	    dev_priv->oa_pmu.ops.context_unpin_notify == NULL)
		return;

	spin_lock_irqsave(&dev_priv->oa_pmu.lock, flags);

	dev_priv->oa_pmu.ops.context_unpin_notify(dev_priv, context);

	mmiowb();
	spin_unlock_irqrestore(&dev_priv->oa_pmu.lock, flags);
}

static struct ctl_table oa_table[] = {
	{
	 .procname = "oa_event_paranoid",
	 .data = &i915_oa_event_paranoid,
	 .maxlen = sizeof(i915_oa_event_paranoid),
	 .mode = 0644,
	 .proc_handler = proc_dointvec,
	 },
	{
	 .procname = "oa_event_min_timer_exponent",
	 .data = &i915_oa_event_min_timer_exponent,
	 .maxlen = sizeof(i915_oa_event_min_timer_exponent),
	 .mode = 0644,
	 .proc_handler = proc_dointvec_minmax,
	 .extra1 = &zero,
	 .extra2 = &oa_exponent_max,
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

void i915_oa_pmu_register(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!IS_HASWELL(dev))
		return;

	dev_priv->oa_pmu.sysctl_header = register_sysctl_table(dev_root);

	/* We need to be careful about forwarding cpu metrics to
	 * userspace considering that PERF_PMU_CAP_IS_DEVICE bypasses
	 * the events/core security check that stops an unprivileged
	 * process collecting metrics for other processes.
	 */
	dev_priv->oa_pmu.dummy_regs = *task_pt_regs(current);

	hrtimer_init(&dev_priv->oa_pmu.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_priv->oa_pmu.timer.function = hrtimer_sample;

	spin_lock_init(&dev_priv->oa_pmu.lock);

	dev_priv->oa_pmu.pmu.capabilities  = PERF_PMU_CAP_IS_DEVICE;

	/* Effectively disallow opening an event with a specific pid
	 * since we aren't interested in processes running on the cpu...
	 */
	dev_priv->oa_pmu.pmu.task_ctx_nr   = perf_invalid_context;

	dev_priv->oa_pmu.pmu.event_init	= i915_oa_event_init;
	dev_priv->oa_pmu.pmu.add	= i915_oa_event_add;
	dev_priv->oa_pmu.pmu.del	= i915_oa_event_del;
	dev_priv->oa_pmu.pmu.start	= i915_oa_event_start;
	dev_priv->oa_pmu.pmu.stop	= i915_oa_event_stop;
	dev_priv->oa_pmu.pmu.read	= i915_oa_event_read;
	dev_priv->oa_pmu.pmu.flush	= i915_oa_event_flush;
	dev_priv->oa_pmu.pmu.event_idx	= i915_oa_event_event_idx;

	dev_priv->oa_pmu.ops.init_oa_buffer = gen7_init_oa_buffer;
	dev_priv->oa_pmu.ops.configure_metric_set = gen7_configure_metric_set;
	dev_priv->oa_pmu.ops.event_start = gen7_event_start;
	dev_priv->oa_pmu.ops.event_stop = gen7_event_stop;
	dev_priv->oa_pmu.ops.context_pin_notify = gen7_context_pin_notify;
	dev_priv->oa_pmu.ops.context_unpin_notify = gen7_context_unpin_notify;
	dev_priv->oa_pmu.ops.flush_oa_snapshots = gen7_flush_oa_snapshots;

	if (perf_pmu_register(&dev_priv->oa_pmu.pmu, "i915_oa", -1))
		dev_priv->oa_pmu.pmu.event_init = NULL;
}

void i915_oa_pmu_unregister(struct drm_device *dev)
{
	struct drm_i915_private *i915 = to_i915(dev);

	if (i915->oa_pmu.pmu.event_init == NULL)
		return;

	unregister_sysctl_table(i915->oa_pmu.sysctl_header);

	perf_pmu_unregister(&i915->oa_pmu.pmu);
	i915->oa_pmu.pmu.event_init = NULL;
}
