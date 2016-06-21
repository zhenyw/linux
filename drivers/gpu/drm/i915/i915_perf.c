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
#include <linux/ktime.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"
#include "intel_lrc.h"
#include "i915_oa_hsw.h"
#include "i915_oa_bdw.h"
#include "i915_oa_chv.h"
#include "i915_oa_skl.h"
#include "i915_oa_bxt.h"

/* Must be a power of two */
#define OA_BUFFER_SIZE		SZ_16M
#define OA_TAKEN(tail, head)	((tail - head) & (OA_BUFFER_SIZE - 1))

/* There's a HW race condition between OA unit tail pointer register updates and
 * writes to memory whereby the tail pointer can sometimes get ahead of what's
 * been written out to the OA buffer so far.
 *
 * Although this can be observed explicitly by checking for a zeroed report-id
 * field in tail reports, it seems preferable to account for this earlier e.g.
 * as part of the _oa_buffer_is_empty checks to minimize -EAGAIN polling cycles
 * in this situation.
 *
 * To give time for the most recent reports to land before they may be copied to
 * userspace, the driver operates as if the tail pointer effectively lags behind
 * the HW tail pointer by 'tail_margin' bytes. The margin in bytes is calculated
 * based on this constant in nanoseconds, the current OA sampling exponent
 * and current report size.
 *
 * There is also a fallback check while reading to simply skip over reports with
 * a zeroed report-id.
 */
#define OA_TAIL_MARGIN_NSEC	100000ULL

/* frequency for checking whether the OA unit has written new reports to the
 * circular OA buffer...
 */
#define POLL_FREQUENCY 200
#define POLL_PERIOD (NSEC_PER_SEC / POLL_FREQUENCY)

#define MAX_CLK_SYNC_PERIOD (60*MSEC_PER_SEC)
#define INIT_CLK_SYNC_PERIOD (20) /* in msecs */

static u32 i915_perf_stream_paranoid = true;

/* The maximum exponent the hardware accepts is 63 (essentially it selects one
 * of the 64bit timestamp bits to trigger reports from) but there's currently
 * no known use case for sampling as infrequently as once per 47 thousand years.
 *
 * Since the timestamps included in OA reports are only 32bits it seems
 * reasonable to limit the OA exponent where it's still possible to account for
 * overflow in OA report timestamps.
 */
#define OA_EXPONENT_MAX 31

#define GEN8_OAREPORT_REASON_TIMER          (1<<19)
#define GEN8_OAREPORT_REASON_TRIGGER1       (1<<20)
#define GEN8_OAREPORT_REASON_TRIGGER2       (1<<21)
#define GEN8_OAREPORT_REASON_CTX_SWITCH     (1<<22)
#define GEN8_OAREPORT_REASON_GO_TRANSITION  (1<<23)
#define GEN9_OAREPORT_REASON_CLK_RATIO      (1<<24)

/* For determining the behavior on overflow of command stream samples */
#define CMD_STREAM_BUF_OVERFLOW_ALLOWED

#define OA_ADDR_ALIGN 64
#define TS_ADDR_ALIGN 8
#define I915_PERF_TS_SAMPLE_SIZE 8

/* Published frequency of GT command stream timestamp clock */
#define FREQUENCY_12_5_MHZ	(12500000)
#define FREQUENCY_12_0_MHZ	(12000000)
#define FREQUENCY_19_2_MHZ	(19200000)
#define GT_CS_TIMESTAMP_FREQUENCY(dev_priv) (IS_GEN9(dev_priv) ? \
				(IS_BROXTON(dev_priv) ? \
				FREQUENCY_19_2_MHZ : \
				FREQUENCY_12_0_MHZ) : \
				FREQUENCY_12_5_MHZ)

/* Data common to all samples (periodic OA / CS based OA / Timestamps) */
struct sample_data {
	u32 source;
	u32 ctx_id;
	u32 pid;
	u32 tag;
	u64 gpu_ts;
	u64 clk_monoraw;
	const u8 *report;
	const u8 *mmio;
};

/* for sysctl proc_dointvec_minmax of i915_oa_min_timer_exponent */
static int zero;
static int oa_exponent_max = OA_EXPONENT_MAX;

/* Theoretically we can program the OA unit to sample every 160ns but don't
 * allow that by default unless root...
 *
 * The period is derived from the exponent as:
 *
 *   period = 80ns * 2^(exponent + 1)
 *
 * Referring to perf's kernel.perf_event_max_sample_rate for a precedent
 * (100000 by default); with an OA exponent of 6 we get a period of 10.240
 * microseconds - just under 100000Hz
 */
static u32 i915_oa_min_timer_exponent = 6;

/* XXX: beware if future OA HW adds new report formats that the current
 * code assumes all reports have a power-of-two size and ~(size - 1) can
 * be used as a mask to align the OA tail pointer.
 */
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

static struct i915_oa_format gen8_plus_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A12]		    = { 0, 64 },
	[I915_OA_FORMAT_A12_B8_C8]	    = { 2, 128 },
	[I915_OA_FORMAT_A32u40_A4u32_B8_C8] = { 5, 256 },
	[I915_OA_FORMAT_C4_B8]		    = { 7, 64 },
};

/* Duplicated from similar static enum in i915_gem_execbuffer.c */
#define I915_USER_RINGS (4)
static const enum intel_engine_id user_ring_map[I915_USER_RINGS + 1] = {
	[I915_EXEC_DEFAULT]	= RCS,
	[I915_EXEC_RENDER]	= RCS,
	[I915_EXEC_BLT]		= BCS,
	[I915_EXEC_BSD]		= VCS,
	[I915_EXEC_VEBOX]	= VECS
};

#define SAMPLE_OA_REPORT	(1<<0)
#define SAMPLE_OA_SOURCE_INFO	(1<<1)
#define SAMPLE_CTX_ID		(1<<2)
#define SAMPLE_PID		(1<<3)
#define SAMPLE_TAG		(1<<4)
#define SAMPLE_TS		(1<<5)
#define SAMPLE_CLK_MONO_RAW	(1<<6)
#define SAMPLE_MMIO		(1<<7)

struct perf_open_properties {
	u32 sample_flags;

	u64 single_context:1;
	u64 ctx_handle;

	/* OA sampling state */
	int metrics_set;
	int oa_format;
	bool oa_periodic;
	int oa_period_exponent;

	/* Command stream mode */
	bool cs_mode;
	enum intel_engine_id engine;
};

/**
 * i915_tstamp_clk_cyclecounter_read - read raw cycle counter
 * @cc: cyclecounter structure
 **/
static cycle_t i915_tstamp_clk_cyclecounter_read(
				const struct cyclecounter *cc)
{
	struct drm_i915_private *dev_priv =
		container_of(cc, typeof(*dev_priv),
			     ts_clk_info.cc);

	return I915_READ64_2x32(GT_TIMESTAMP_COUNT,
					GT_TIMESTAMP_COUNT_UDW);
}

static void i915_tstamp_clk_calc_update_mult_shift(struct cyclecounter *cc,
					u32 frequency)
{
	clocks_calc_mult_shift(&cc->mult, &cc->shift,
			frequency, NSEC_PER_SEC, 3600);
}

static void i915_tstamp_clk_init_base_freq(struct drm_i915_private *dev_priv,
				struct cyclecounter *cc)
{
	cc->read = i915_tstamp_clk_cyclecounter_read;
	cc->mask = CYCLECOUNTER_MASK(64);
	i915_tstamp_clk_calc_update_mult_shift(cc,
			dev_priv->ts_clk_info.timestamp_frequency);
}

/**
 * i915_get_syncdevicetime - Callback given to timekeeping code to read
	device time and sytem clk counter value
 * @device_time: current device time
 * @system: system counter value read synchronously with device time
 * @ctx: context provided by timekeeping code
 *
 **/
static int i915_get_syncdevicetime(ktime_t *device_time,
					 struct system_counterval_t *system,
					 void *ctx)
{
	struct drm_i915_private *dev_priv = (struct drm_i915_private *)ctx;
	struct timecounter *tc = &dev_priv->ts_clk_info.tc;
	struct clocksource *curr_clksource;

	*device_time = ns_to_ktime(timecounter_read(tc));

	curr_clksource = get_current_clocksource();
	system->cycles = curr_clksource->read(curr_clksource);
	system->cs = curr_clksource;

	return 0;
}

static void i915_perf_clock_sync_work(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv),
		ts_clk_info.clk_sync_work.work);
	struct system_device_crosststamp *xtstamp =
			&dev_priv->ts_clk_info.xtstamp;
	ktime_t last_sys_time = xtstamp->sys_monoraw;
	ktime_t last_gpu_time = xtstamp->device;
	ktime_t clk_mono_offset, gpu_time_offset;
	s64 delta;
	u32 gpu_freq = dev_priv->ts_clk_info.timestamp_frequency;
	u32 freq_delta = 0;

	get_device_system_crosststamp(i915_get_syncdevicetime, dev_priv,
					NULL, xtstamp);

	clk_mono_offset = ktime_sub(xtstamp->sys_monoraw, last_sys_time);
	gpu_time_offset = ktime_sub(xtstamp->device, last_gpu_time);

	/* delta time in ns */
	delta = ktime_to_ns(ktime_sub(gpu_time_offset, clk_mono_offset));

	/* If time delta < 1 us, we can assume gpu frequency is correct */
	if (abs(delta) < NSEC_PER_USEC)
		goto out;

	/* The two clocks shouldn't deviate more than 1 second during the
	 * resync period. If this is the case (which may happen due to
	 * suspend/resume), then don't apply frequency correction, and
	 * fast forward/rewind the clocks to resync immediately
	 */
	if (abs(delta) > NSEC_PER_SEC)
		goto out;

	/* Calculate frequency delta */
	freq_delta = abs(delta)*gpu_freq;
	do_div(freq_delta, ktime_to_ns(clk_mono_offset));

	if (freq_delta == 0)
		goto out;

	if (delta < 0)
		freq_delta = -freq_delta;

	dev_priv->ts_clk_info.timestamp_frequency += freq_delta;
	i915_tstamp_clk_calc_update_mult_shift(&dev_priv->ts_clk_info.cc,
			dev_priv->ts_clk_info.timestamp_frequency);

	/*
	 * Get updated device/system times based on corrected frequency.
	 * Note that this may cause jumps in device time depending on whether
	 * frequency delta is positive or negative.
	 * NB: Take care that monotonicity of sample timestamps is maintained
	 * even with these jumps.
	 */
	get_device_system_crosststamp(i915_get_syncdevicetime, dev_priv,
					NULL, xtstamp);

out:
	dev_priv->ts_clk_info.clk_offset = ktime_sub(xtstamp->sys_monoraw,
						xtstamp->device);

	/* We can schedule next synchronization at incrementally higher
	 * durations, so that the accuracy of our calculated frequency
	 * can improve over time. The max resync period is arbitrarily
	 * set as one hour.
	 */
	dev_priv->ts_clk_info.resync_period *= 2;
	if (dev_priv->ts_clk_info.resync_period < MAX_CLK_SYNC_PERIOD)
		schedule_delayed_work(&dev_priv->ts_clk_info.clk_sync_work,
			msecs_to_jiffies(dev_priv->ts_clk_info.resync_period));
}

/*
 * Emit the commands to capture metrics, into the command stream. This function
 * can be called concurrently with the stream operations and doesn't require
 * perf mutex lock.
 */

void i915_perf_command_stream_hook(struct drm_i915_gem_request *request,
					u32 tag)
{
	struct intel_engine_cs *engine = request->engine;
	struct drm_i915_private *dev_priv = engine->dev->dev_private;
	struct i915_perf_stream *stream;

	if (!dev_priv->perf.initialized)
		return;

	mutex_lock(&dev_priv->perf.streams_lock);
	list_for_each_entry(stream, &dev_priv->perf.streams, link) {
		if ((stream->state == I915_PERF_STREAM_ENABLED) &&
				stream->cs_mode &&
				(stream->engine == engine->id))
			stream->ops->command_stream_hook(stream, request, tag);
	}
	mutex_unlock(&dev_priv->perf.streams_lock);
}

#ifdef CMD_STREAM_BUF_OVERFLOW_ALLOWED
/*
 * Release some perf entries to make space for a new entry data. We dereference
 * the associated request before deleting the entry. Also, no need to check for
 * gpu completion of commands, since, these entries are anyways going to be
 * replaced by a new entry, and gpu will overwrite the buffer contents
 * eventually, when the request associated with new entry completes.
 */
static void release_some_perf_entries(struct drm_i915_private *dev_priv,
				enum intel_engine_id id, u32 target_size)
{
	struct i915_perf_cs_data_node *entry, *next;
	u32 size = 0;

	list_for_each_entry_safe
		(entry, next, &dev_priv->perf.node_list[id], link) {

		size += entry->size;
		i915_gem_request_unreference(entry->request);
		list_del(&entry->link);
		kfree(entry);

		if (size >= target_size)
			break;
	}
}
#endif

/*
 * Insert the perf entry to the end of the list. If the overwrite of old entries
 * is allowed, the function always manages to insert the entry and returns 0.
 * If overwrite is not allowed, on detection of overflow condition, an
 * appropriate status flag is set, and function returns -ENOSPC.
 */
static int insert_perf_entry(struct drm_i915_private *dev_priv,
				struct i915_perf_stream *stream,
				struct i915_perf_cs_data_node *entry)
{
	struct i915_perf_cs_data_node *first_entry, *last_entry;
	u32 sample_flags = stream->sample_flags;
	enum intel_engine_id id = stream->engine;
	int max_offset = dev_priv->perf.command_stream_buf[id].obj->base.size;
	u32 offset, entry_size = 0;
	bool sample_ts = false;
	int ret = 0;

	if (stream->sample_flags & SAMPLE_OA_REPORT)
		entry_size += dev_priv->perf.oa.oa_buffer.format_size;
	else if (sample_flags & (SAMPLE_TS|SAMPLE_CLK_MONO_RAW)) {
		/*
		 * XXX: Since TS data can anyways be derived from OA report, so
		 * no need to capture it for RCS engine, if capture oa data is
		 * called already.
		 */
		entry_size += I915_PERF_TS_SAMPLE_SIZE;
		sample_ts = true;
	}

	if (sample_flags & SAMPLE_MMIO)
		entry_size += 4*dev_priv->perf.num_mmio;

	spin_lock(&dev_priv->perf.node_list_lock[id]);
	if (list_empty(&dev_priv->perf.node_list[id])) {
		offset = 0;
		goto out;
	}

	first_entry = list_first_entry(&dev_priv->perf.node_list[id],
				       typeof(*first_entry), link);
	last_entry = list_last_entry(&dev_priv->perf.node_list[id],
				     typeof(*last_entry), link);

	if (last_entry->start_offset >= first_entry->start_offset) {
		/* Sufficient space available at the end of buffer? */
		if (last_entry->start_offset + last_entry->size + entry_size
							< max_offset)
			offset = last_entry->start_offset + last_entry->size;
		/*
		 * Wraparound condition. Is sufficient space available at
		 * beginning of buffer?
		 */
		else if (entry_size < first_entry->start_offset)
			offset = 0;
		/* Insufficient space */
		else {
#ifdef CMD_STREAM_BUF_OVERFLOW_ALLOWED
			u32 target_size = entry_size -
						first_entry->start_offset;

			release_some_perf_entries(dev_priv, id, target_size);
			offset = 0;
#else
			dev_priv->perf.command_stream_buf[id].status |=
				I915_PERF_CMD_STREAM_BUF_STATUS_OVERFLOW;
			ret = -ENOSPC;
			goto out_unlock;
#endif
		}
	} else {
		/* Sufficient space available? */
		if (last_entry->start_offset + last_entry->size + entry_size
						< first_entry->start_offset)
			offset = last_entry->start_offset + last_entry->size;
		/* Insufficient space */
		else {
#ifdef CMD_STREAM_BUF_OVERFLOW_ALLOWED
			u32 target_size = entry_size -
				(first_entry->start_offset -
					last_entry->start_offset -
					last_entry->size);

			release_some_perf_entries(dev_priv, id, target_size);
			offset = last_entry->start_offset + last_entry->size;
#else
			dev_priv->perf.command_stream_buf[id].status |=
				I915_PERF_CMD_STREAM_BUF_STATUS_OVERFLOW;
			ret = -ENOSPC;
			goto out_unlock;
#endif
		}
	}

out:
	entry->start_offset = offset;
	entry->size = entry_size;
	if (stream->sample_flags & SAMPLE_OA_REPORT) {
		entry->oa_offset = offset;
		/* Ensure 64 byte alignment of oa_offset */
		entry->oa_offset = ALIGN(entry->oa_offset, OA_ADDR_ALIGN);
		offset = entry->oa_offset +
				dev_priv->perf.oa.oa_buffer.format_size;
	}
	if (sample_ts) {
		entry->ts_offset = offset;
		/* Ensure 8 byte alignment of ts_offset */
		entry->ts_offset = ALIGN(entry->ts_offset, TS_ADDR_ALIGN);
		offset = entry->ts_offset + I915_PERF_TS_SAMPLE_SIZE;
	}
	if (sample_flags & SAMPLE_MMIO) {
		entry->mmio_offset = offset;
		offset = entry->mmio_offset + 4*dev_priv->perf.num_mmio;
	}

	list_add_tail(&entry->link, &dev_priv->perf.node_list[id]);
#ifndef CMD_STREAM_BUF_OVERFLOW_ALLOWED
out_unlock:
#endif
	spin_unlock(&dev_priv->perf.node_list_lock[id]);
	return ret;
}

static int i915_ring_stream_capture_oa(struct drm_i915_gem_request *req,
				u32 offset)
{
	struct intel_engine_cs *engine = req->engine;
	struct intel_ringbuffer *ringbuf = req->ringbuf;
	struct drm_i915_private *dev_priv = engine->dev->dev_private;
	u32 addr = 0;
	int ret;

	/* OA counters are only supported on the render engine */
	BUG_ON(engine->id != RCS);

	ret = intel_ring_begin(req, 4);
	if (ret)
		return ret;

	addr = dev_priv->perf.command_stream_buf[RCS].vma->node.start + offset;

	/* addr should be 64 byte aligned */
	BUG_ON(addr & 0x3f);

	if (i915.enable_execlists) {
		intel_logical_ring_emit(ringbuf, MI_REPORT_PERF_COUNT | (2<<0));
		intel_logical_ring_emit(ringbuf,
					addr | MI_REPORT_PERF_COUNT_GGTT);
		intel_logical_ring_emit(ringbuf, 0);
		intel_logical_ring_emit(ringbuf,
			i915_gem_request_get_seqno(req));
		intel_logical_ring_advance(ringbuf);
	} else {
		if (INTEL_INFO(engine->dev)->gen >= 8) {
			intel_ring_emit(engine, MI_REPORT_PERF_COUNT | (2<<0));
			intel_ring_emit(engine, addr | MI_REPORT_PERF_COUNT_GGTT);
			intel_ring_emit(engine, 0);
			intel_ring_emit(engine,
				i915_gem_request_get_seqno(req));
		} else {
			intel_ring_emit(engine, MI_REPORT_PERF_COUNT | (1<<0));
			intel_ring_emit(engine, addr | MI_REPORT_PERF_COUNT_GGTT);
			intel_ring_emit(engine, i915_gem_request_get_seqno(req));
			intel_ring_emit(engine, MI_NOOP);
		}
		intel_ring_advance(engine);
	}
	return 0;
}

static int i915_ring_stream_capture_ts(struct drm_i915_gem_request *req,
						u32 offset)
{
	struct intel_engine_cs *engine = req->engine;
	struct intel_ringbuffer *ringbuf = req->ringbuf;
	struct drm_i915_private *dev_priv = engine->dev->dev_private;
	u32 addr = 0;
	int ret;

	ret = intel_ring_begin(req, 6);
	if (ret)
		return ret;

	addr = dev_priv->perf.command_stream_buf[engine->id].vma->node.start +
		offset;

	if (i915.enable_execlists) {
		if (engine->id == RCS) {
			intel_logical_ring_emit(ringbuf,
						GFX_OP_PIPE_CONTROL(6));
			intel_logical_ring_emit(ringbuf,
						PIPE_CONTROL_GLOBAL_GTT_IVB |
						PIPE_CONTROL_TIMESTAMP_WRITE);
			intel_logical_ring_emit(ringbuf, addr |
						PIPE_CONTROL_GLOBAL_GTT);
			intel_logical_ring_emit(ringbuf, 0);
			intel_logical_ring_emit(ringbuf, 0);
			intel_logical_ring_emit(ringbuf, 0);
		} else {
			uint32_t cmd;

			cmd = MI_FLUSH_DW + 2; /* Gen8+ */

			cmd |= MI_FLUSH_DW_OP_STAMP;

			intel_logical_ring_emit(ringbuf, cmd);
			intel_logical_ring_emit(ringbuf, addr |
						MI_FLUSH_DW_USE_GTT);
			intel_logical_ring_emit(ringbuf, 0);
			intel_logical_ring_emit(ringbuf, 0);
			intel_logical_ring_emit(ringbuf, 0);
			intel_logical_ring_emit(ringbuf, MI_NOOP);
		}
		intel_logical_ring_advance(ringbuf);
	} else {
		if (engine->id == RCS) {
			if (INTEL_INFO(engine->dev)->gen >= 8)
				intel_ring_emit(engine, GFX_OP_PIPE_CONTROL(6));
			else
				intel_ring_emit(engine, GFX_OP_PIPE_CONTROL(5));
			intel_ring_emit(engine,
					PIPE_CONTROL_GLOBAL_GTT_IVB |
					PIPE_CONTROL_TIMESTAMP_WRITE);
			intel_ring_emit(engine, addr | PIPE_CONTROL_GLOBAL_GTT);
			intel_ring_emit(engine, 0);
			if (INTEL_INFO(engine->dev)->gen >= 8) {
				intel_ring_emit(engine, 0);
				intel_ring_emit(engine, 0);
			} else {
				intel_ring_emit(engine, 0);
				intel_ring_emit(engine, MI_NOOP);
			}
		} else {
			uint32_t cmd;

			cmd = MI_FLUSH_DW + 1;
			if (INTEL_INFO(engine->dev)->gen >= 8)
				cmd += 1;

			cmd |= MI_FLUSH_DW_OP_STAMP;

			intel_ring_emit(engine, cmd);
			intel_ring_emit(engine, addr | MI_FLUSH_DW_USE_GTT);
			if (INTEL_INFO(engine->dev)->gen >= 8) {
				intel_ring_emit(engine, 0);
				intel_ring_emit(engine, 0);
				intel_ring_emit(engine, 0);
			} else {
				intel_ring_emit(engine, 0);
				intel_ring_emit(engine, 0);
				intel_ring_emit(engine, MI_NOOP);
			}
			intel_ring_emit(engine, MI_NOOP);
		}
		intel_ring_advance(engine);
	}
	return 0;
}

static int i915_ring_stream_capture_mmio(struct drm_i915_gem_request *req,
						u32 offset)
{
	struct intel_engine_cs *engine = req->engine;
	struct intel_ringbuffer *ringbuf = req->ringbuf;
	struct drm_i915_private *dev_priv = engine->dev->dev_private;
	int num_mmio = dev_priv->perf.num_mmio;
	u32 mmio_addr, addr = 0;
	int ret, i;

	ret = intel_ring_begin(req, 4*num_mmio);
	if (ret)
		return ret;

	mmio_addr =
		dev_priv->perf.command_stream_buf[engine->id].vma->node.start +
			offset;

	if (i915.enable_execlists) {
		for (i = 0; i < num_mmio; i++) {
			uint32_t cmd;

			addr = mmio_addr + 4*i;

			cmd = MI_STORE_REGISTER_MEM_GEN8 |
						MI_SRM_LRM_GLOBAL_GTT;

			intel_logical_ring_emit(ringbuf, cmd);
			intel_logical_ring_emit(ringbuf,
					dev_priv->perf.mmio_list[i]);
			intel_logical_ring_emit(ringbuf, addr);
			intel_logical_ring_emit(ringbuf, 0);
		}
		intel_logical_ring_advance(ringbuf);
	} else {
		for (i = 0; i < num_mmio; i++) {
			uint32_t cmd;

			addr = mmio_addr + 4*i;

			if (INTEL_INFO(engine->dev)->gen >= 8)
				cmd = MI_STORE_REGISTER_MEM_GEN8 |
					MI_SRM_LRM_GLOBAL_GTT;
			else
				cmd = MI_STORE_REGISTER_MEM |
					MI_SRM_LRM_GLOBAL_GTT;

			intel_ring_emit(engine, cmd);
			intel_ring_emit(engine, dev_priv->perf.mmio_list[i]);
			intel_ring_emit(engine, addr);
			if (INTEL_INFO(engine->dev)->gen >= 8)
				intel_ring_emit(engine, 0);
			else
				intel_ring_emit(engine, MI_NOOP);
		}
		intel_ring_advance(engine);
	}
	return 0;
}

static void i915_ring_stream_cs_hook(struct i915_perf_stream *stream,
				struct drm_i915_gem_request *req, u32 tag)
{
	struct intel_engine_cs *engine = req->engine;
	struct intel_context *ctx = req->ctx;
	struct drm_i915_private *dev_priv = engine->dev->dev_private;
	enum intel_engine_id id = stream->engine;
	u32 sample_flags = stream->sample_flags;
	struct i915_perf_cs_data_node *entry;
	int ret = 0;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL) {
		DRM_ERROR("alloc failed\n");
		return;
	}

	ret = insert_perf_entry(dev_priv, stream, entry);
	if (ret)
		goto err_free;

	entry->ctx_id = ctx->hw_id;
	entry->pid = current->pid;
	entry->tag = tag;
	i915_gem_request_assign(&entry->request, req);

	if (sample_flags & SAMPLE_OA_REPORT) {
		ret = i915_ring_stream_capture_oa(req, entry->oa_offset);
		if (ret)
			goto err_unref;
	} else if (sample_flags & (SAMPLE_TS|SAMPLE_CLK_MONO_RAW)) {
		/*
		 * XXX: Since TS data can anyways be derived from OA report, so
		 * no need to capture it for RCS engine, if capture oa data is
		 * called already.
		 */
		ret = i915_ring_stream_capture_ts(req, entry->ts_offset);
		if (ret)
			goto err_unref;
	}

	if (sample_flags & SAMPLE_MMIO) {
		ret = i915_ring_stream_capture_mmio(req,
				entry->mmio_offset);
		if (ret)
			goto err_unref;
	}

	i915_vma_move_to_active(dev_priv->perf.command_stream_buf[id].vma, req);
	return;

err_unref:
	i915_gem_request_unreference(entry->request);
	spin_lock(&dev_priv->perf.node_list_lock[id]);
	list_del(&entry->link);
	spin_unlock(&dev_priv->perf.node_list_lock[id]);
err_free:
	kfree(entry);
}

static int i915_ring_stream_wait_gpu(struct drm_i915_private *dev_priv,
				enum intel_engine_id id)
{
	struct i915_perf_cs_data_node *last_entry = NULL;
	struct drm_i915_gem_request *req = NULL;
	int ret;

	/*
	 * Wait for the last scheduled request to complete. This would
	 * implicitly wait for the prior submitted requests. The refcount
	 * of the requests is not decremented here.
	 */
	spin_lock(&dev_priv->perf.node_list_lock[id]);

	if (!list_empty(&dev_priv->perf.node_list[id])) {
		last_entry = list_last_entry(&dev_priv->perf.node_list[id],
			struct i915_perf_cs_data_node, link);
		req = last_entry->request;
	}
	spin_unlock(&dev_priv->perf.node_list_lock[id]);

	if (!req)
		return 0;

	ret = __i915_wait_request(req, true, NULL, NULL);
	if (ret) {
		DRM_ERROR("Failed to wait for request\n");
		return ret;
	}
	return 0;
}

static void i915_ring_stream_free_requests(struct drm_i915_private *dev_priv,
				enum intel_engine_id id)
{
	struct i915_perf_cs_data_node *entry, *next;

	list_for_each_entry_safe
		(entry, next, &dev_priv->perf.node_list[id], link) {
		i915_gem_request_unreference(entry->request);

		spin_lock(&dev_priv->perf.node_list_lock[id]);
		list_del(&entry->link);
		spin_unlock(&dev_priv->perf.node_list_lock[id]);
		kfree(entry);
	}
}

/* NB: This is either called via fops or the poll check hrtimer (atomic ctx)
 *
 * It's safe to read OA config state here unlocked, assuming that this is only
 * called while the stream is enabled, while the global OA configuration can't
 * be modified.
 *
 * Note: we don't lock around the head/tail reads even though there's the slim
 * possibility of read() fop errors forcing a re-init of the OA buffer
 * pointers.  A race here could result in a false positive !empty status which
 * is acceptable.
 */
static int
gen8_oa_buffer_num_samples_fop_unlocked(struct drm_i915_private *dev_priv,
					u32 *last_ts)
{
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 head = I915_READ(GEN8_OAHEADPTR) & GEN8_OAHEADPTR_MASK;
	u32 tail = I915_READ(GEN8_OATAILPTR) & GEN8_OATAILPTR_MASK;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u32 num_samples;
	u8 *report;

	head -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	num_samples = OA_TAKEN(tail, head) / report_size;

	/* read the timestamp of the last sample */
	if (num_samples) {
		head += report_size*(num_samples - 1);
		report = oa_buf_base + (head & mask);
		*last_ts = *(u32 *)(report + 4);
	}

	return num_samples;
}

/* NB: This is either called via fops or the poll check hrtimer (atomic ctx)
 *
 * It's safe to read OA config state here unlocked, assuming that this is only
 * called while the stream is enabled, while the global OA configuration can't
 * be modified.
 *
 * Note: we don't lock around the head/tail reads even though there's the slim
 * possibility of read() fop errors forcing a re-init of the OA buffer
 * pointers.  A race here could result in a false positive !empty status which
 * is acceptable.
 */
static int
gen7_oa_buffer_num_samples_fop_unlocked(struct drm_i915_private *dev_priv,
					u32 *last_ts)
{
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u32 oastatus2 = I915_READ(GEN7_OASTATUS2);
	u32 oastatus1 = I915_READ(GEN7_OASTATUS1);
	u32 head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	u32 tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	int available_size;
	u32 num_samples = 0;
	u8 *report;

	head -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;
	available_size = OA_TAKEN(tail, head) - dev_priv->perf.oa.tail_margin;
	if (available_size >= report_size) {
		num_samples = available_size / report_size;
		head += report_size*(num_samples - 1);
		report = oa_buf_base + (head & mask);
		*last_ts = *(u32 *)(report + 4);
	}

	return num_samples;
}

static u32 gen7_oa_buffer_get_ctx_id(struct i915_perf_stream *stream,
				    const u8 *report)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (!stream->cs_mode)
		WARN_ONCE(1,
			"CTX ID can't be retrieved if command stream mode not enabled");

	/*
	 * OA reports generated in Gen7 don't have the ctx ID information.
	 * Therefore, just rely on the ctx ID information from the last CS
	 * sample forwarded
	 */
	return dev_priv->perf.last_ctx_id;
}

static u32 gen8_oa_buffer_get_ctx_id(struct i915_perf_stream *stream,
				    const u8 *report)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	/* The ctx ID present in the OA reports have intel_context::global_id
	 * present, since this is programmed into the ELSP in execlist mode.
	 * In non-execlist mode, fall back to retrieving the ctx ID from the
	 * last saved ctx ID from command stream mode.
	 */
	if (i915.enable_execlists) {
		u32 ctx_id = *(u32 *)(report + 12);
		ctx_id &= 0xfffff;
		return ctx_id;
	} else {
		if (!stream->cs_mode)
		WARN_ONCE(1,
			"CTX ID can't be retrieved if command stream mode not enabled");

		return dev_priv->perf.last_ctx_id;
	}
}

/**
 * Appends a status record to a userspace read() buffer.
 */
static int append_oa_status(struct i915_perf_stream *stream,
			    struct i915_perf_read_state *read_state,
			    enum drm_i915_perf_record_type type)
{
	struct drm_i915_perf_record_header header = { type, 0, sizeof(header) };

	if ((read_state->count - read_state->read) < header.size)
		return -ENOSPC;

	if (copy_to_user(read_state->buf, &header, sizeof(header)))
		return -EFAULT;

	read_state->buf += header.size;
	read_state->read += header.size;

	return 0;
}

/**
 * Copies single sample into userspace read() buffer.
 */
static int append_sample(struct i915_perf_stream *stream,
			    struct i915_perf_read_state *read_state,
			    struct sample_data *data)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	struct drm_i915_perf_record_header header;
	u32 sample_flags = stream->sample_flags;
	char __user *buf = read_state->buf;

	header.type = DRM_I915_PERF_RECORD_SAMPLE;
	header.pad = 0;
	header.size = stream->sample_size;

	if ((read_state->count - read_state->read) < header.size)
		return -ENOSPC;

	if (copy_to_user(buf, &header, sizeof(header)))
		return -EFAULT;
	buf += sizeof(header);

	if (sample_flags & SAMPLE_OA_SOURCE_INFO) {
		if (copy_to_user(buf, &data->source, 4))
			return -EFAULT;
		buf += 4;
	}

	if (sample_flags & SAMPLE_CTX_ID) {
		if (copy_to_user(buf, &data->ctx_id, 4))
			return -EFAULT;
		buf += 4;
	}

	if (sample_flags & SAMPLE_PID) {
		if (copy_to_user(buf, &data->pid, 4))
			return -EFAULT;
		buf += 4;
	}

	if (sample_flags & SAMPLE_TAG) {
		if (copy_to_user(buf, &data->tag, 4))
			return -EFAULT;
		buf += 4;
	}

	if (sample_flags & SAMPLE_TS) {
		if (copy_to_user(buf, &data->gpu_ts, I915_PERF_TS_SAMPLE_SIZE))
			return -EFAULT;
		buf += I915_PERF_TS_SAMPLE_SIZE;
	}

	if (sample_flags & SAMPLE_CLK_MONO_RAW) {
		if (copy_to_user(buf, &data->clk_monoraw,
					I915_PERF_TS_SAMPLE_SIZE))
			return -EFAULT;
		buf += I915_PERF_TS_SAMPLE_SIZE;
	}

	if (sample_flags & SAMPLE_MMIO) {
		if (copy_to_user(buf, data->mmio, 4*dev_priv->perf.num_mmio))
			return -EFAULT;
		buf += 4*dev_priv->perf.num_mmio;
	}

	if (sample_flags & SAMPLE_OA_REPORT) {
		if (copy_to_user(buf, data->report, report_size))
			return -EFAULT;
		buf += report_size;
	}

	read_state->buf = buf;
	read_state->read += header.size;

	return 0;
}

static u64 get_clk_monoraw_from_gpu_ts(struct i915_perf_stream *stream,
					u64 gpu_ts)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	struct timecounter *tc = &dev_priv->ts_clk_info.tc;
	u64 gpu_time, clk_monoraw;

	gpu_time = cyclecounter_cyc2ns(tc->cc, gpu_ts, tc->mask, &tc->frac);

	clk_monoraw = gpu_time + ktime_to_ns(dev_priv->ts_clk_info.clk_offset);

	/* Ensure monotonicity by clamping the system time in case it goes
	 * backwards.
	 */
	if (clk_monoraw < stream->last_sample_ts)
		clk_monoraw = stream->last_sample_ts;

	stream->last_sample_ts = clk_monoraw;
	return clk_monoraw;
}

static u64 get_gpu_ts_from_oa_report(struct drm_i915_private *dev_priv,
					const u8 *report)
{
	u32 sample_ts = *(u32 *)(report + 4);
	u32 delta;

	/*
	 * NB: We have to assume we're updating last_gpu_ts frequently
	 * enough that it's never possible to see multiple overflows before
	 * we compare sample_ts to last_gpu_ts. Since this is significantly
	 * large duration (~6min for 80ns ts base), we can safely assume so.
	 */
	delta = sample_ts - (u32)dev_priv->perf.oa.last_gpu_ts;
	dev_priv->perf.oa.last_gpu_ts += delta;

	return dev_priv->perf.oa.last_gpu_ts;
}

static int append_oa_buffer_sample(struct i915_perf_stream *stream,
				    struct i915_perf_read_state *read_state,
				    const u8 *report)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	u32 sample_flags = stream->sample_flags;
	struct sample_data data = { 0 };
	u32 mmio_list_dummy[I915_PERF_MMIO_NUM_MAX] = { 0 };

	if (sample_flags & SAMPLE_OA_SOURCE_INFO) {
		enum drm_i915_perf_oa_event_source source;

		if (INTEL_INFO(dev_priv)->gen >= 8) {
			u32 reason = *(u32 *)report;

			if (reason & GEN8_OAREPORT_REASON_CTX_SWITCH)
				source =
				I915_PERF_OA_EVENT_SOURCE_CONTEXT_SWITCH;
			else if (reason & GEN8_OAREPORT_REASON_TIMER)
				source = I915_PERF_OA_EVENT_SOURCE_PERIODIC;
			else
				source = I915_PERF_OA_EVENT_SOURCE_UNDEFINED;
		} else
			source = I915_PERF_OA_EVENT_SOURCE_PERIODIC;

		data.source = source;
	}
	if (sample_flags & SAMPLE_CTX_ID)
		data.ctx_id = dev_priv->perf.oa.ops.oa_buffer_get_ctx_id(
						stream, report);

	if (sample_flags & SAMPLE_PID)
		data.pid = dev_priv->perf.last_pid;

	if (sample_flags & SAMPLE_TAG)
		data.tag = dev_priv->perf.last_tag;

	/* Derive timestamp from OA report */
	if (sample_flags & SAMPLE_TS)
		data.gpu_ts = get_gpu_ts_from_oa_report(dev_priv, report);

	if (sample_flags & SAMPLE_CLK_MONO_RAW) {
		u64 gpu_ts = get_gpu_ts_from_oa_report(dev_priv, report);

		data.clk_monoraw = get_clk_monoraw_from_gpu_ts(stream, gpu_ts);
	}

	/* Periodic OA samples don't have mmio associated with them */
	if (sample_flags & SAMPLE_MMIO)
		data.mmio = (u8 *)mmio_list_dummy;

	if (sample_flags & SAMPLE_OA_REPORT)
		data.report = report;

	return append_sample(stream, read_state, &data);
}

/**
 * Copies all buffered OA reports into userspace read() buffer.
 * @head_ptr: (inout): the head pointer before and after appending
 *
 * Returns 0 on success, negative error code on failure.
 *
 * Notably any error condition resulting in a short read (-ENOSPC or
 * -EFAULT) will be returned even though one or more records may
 * have been successfully copied. In this case the error may be
 * squashed before returning to userspace.
 *
 * NB: @head_ptr may be updated even if an error is returned
 * (e.g. for a short read resulting in -ENOSPC or -EFAULT.)
 */
static int gen8_append_oa_reports(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state,
				  u32 *head_ptr,
				  u32 tail, u32 ts, u32 max_records)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u32 head;
	u32 taken;
	int ret = 0;
	int n_records = 0;

	BUG_ON(stream->state != I915_PERF_STREAM_ENABLED);

	head = *head_ptr - dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;

	/* The OA unit is expected to wrap the tail pointer according to the OA
	 * buffer size and since we should never write a misaligned head
	 * pointer we don't expect to read one back either...
	 */
	if (tail > OA_BUFFER_SIZE || head > OA_BUFFER_SIZE ||
	    head % report_size) {
		DRM_ERROR("Inconsistent OA buffer pointer (head = %u, tail = %u): force restart",
			  head, tail);
		dev_priv->perf.oa.ops.oa_disable(dev_priv);
		dev_priv->perf.oa.ops.oa_enable(dev_priv);
		*head_ptr = I915_READ(GEN8_OAHEADPTR) & GEN8_OAHEADPTR_MASK;
		return -EIO;
	}

	/* The tail pointer increases in 64 byte increments, not in report_size
	 * steps...
	 */
	tail &= ~(report_size - 1);

	for (/* none */;
	     (taken = OA_TAKEN(tail, head)) && (n_records <= max_records);
	     head = (head + report_size) & mask) {
		u8 *report = oa_buf_base + head;
		u32 *report32 = (void *)report;
		u32 ctx_id = report32[2];
		u32 report_ts = report32[1];
		u32 report_id = report32[0];

		/* Report timestamp should not exceed passed in ts */
		if (report_ts > ts)
			break;

		/* All the report sizes factor neatly into the buffer
		 * size so we never expect to see a report split
		 * between the beginning and end of the buffer.
		 *
		 * Given the initial alignment check a misalignment
		 * here would imply a driver bug that would result
		 * in an overrun.
		 */
		BUG_ON((OA_BUFFER_SIZE - head) < report_size);

		if (i915.enable_execlists) {
			/* XXX: Just keep the lower 20 bits for now
			 * since I'm not entirely sure if the HW
			 * touches any of the higher bits
			 */
#warning "check if execlist ctx ID is actually 21 bits not 20?"
			ctx_id &= 0xfffff;
		}

		if ((report_id & (1 << dev_priv->perf.oa.ctx_valid_off)) &&
		    (ctx_id == 0)) {
			DRM_ERROR("Skipping spurious, invalid OA report\n");
			continue;
		}

		/* NB: For Gen 8 we handle per-context report filtering
		 * ourselves instead of programming the OA unit with a
		 * specific context id.
		 *
		 * NB: To allow userspace to calculate all counter
		 * deltas for a specific context we have to send the
		 * first report belonging to any subsequently
		 * switched-too context. In this case we set the ID to
		 * an invalid ID. It could be good to annotate these
		 * reports with a _CTX_SWITCH_AWAY reason later.
		 */
		if (!stream->ctx ||
		    dev_priv->perf.oa.specific_ctx_id == ctx_id ||
		    dev_priv->perf.oa.oa_buffer.last_ctx_id == ctx_id) {

			/* Note: we don't check the reason field to
			 * recognise context-switch reports because
			 * it's possible that the first report after a
			 * context switch is in fact periodic. We mark
			 * the switch-away reports with an invalid
			 * context id to be recognisable by userspace.
			 */
			if (stream->ctx &&
			    dev_priv->perf.oa.specific_ctx_id != ctx_id)
				report32[2] = 0x1fffff;

			ret = append_oa_buffer_sample(stream, read_state,
							report);
			if (ret)
				break;

				n_records++;
			dev_priv->perf.oa.oa_buffer.last_ctx_id = ctx_id;
		}
	}

	*head_ptr = dev_priv->perf.oa.oa_buffer.gtt_offset + head;

	return ret;
}

/**
 * Check OA status registers, appending status records if necessary
 * and copy as many buffered OA reports to userspace as possible.
 *
 * NB: some data may be successfully copied to the userspace buffer
 * even if an error is returned, and this is reflected in the
 * updated @read_state.
 */
static int gen8_oa_read(struct i915_perf_stream *stream,
			struct i915_perf_read_state *read_state,
			u32 ts, u32 max_records)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u32 oastatus;
	u32 head;
	u32 tail;
	int ret;

	BUG_ON(!dev_priv->perf.oa.oa_buffer.addr);

	head = I915_READ(GEN8_OAHEADPTR) & GEN8_OAHEADPTR_MASK;
	tail = I915_READ(GEN8_OATAILPTR) & GEN8_OATAILPTR_MASK;
	oastatus = I915_READ(GEN8_OASTATUS);

	if (unlikely(oastatus & (GEN8_OASTATUS_OABUFFER_OVERFLOW |
				 GEN8_OASTATUS_REPORT_LOST))) {

		if (oastatus & GEN8_OASTATUS_OABUFFER_OVERFLOW) {
			ret = append_oa_status(stream, read_state,
					       DRM_I915_PERF_RECORD_OA_BUFFER_LOST);
			if (ret)
				return ret;

			oastatus &= ~GEN8_OASTATUS_OABUFFER_OVERFLOW;
		}

		if (oastatus & GEN8_OASTATUS_REPORT_LOST) {
			ret = append_oa_status(stream, read_state,
					       DRM_I915_PERF_RECORD_OA_REPORT_LOST);
			if (ret == 0)
				oastatus &= ~GEN8_OASTATUS_REPORT_LOST;
		}

		I915_WRITE(GEN8_OASTATUS, oastatus);

		if (ret)
			return ret;
	}

	/* If there is still buffer space */

	ret = gen8_append_oa_reports(stream, read_state, &head, tail,
				     ts, max_records);

	/* All the report sizes are a power of two and the
	 * head should always be incremented by some multiple
	 * of the report size.
	 *
	 * A warning here, but notably if we later read back a
	 * misaligned pointer we will treat that as a bug since
	 * it could lead to a buffer overrun.
	 */
	WARN_ONCE(head & (report_size - 1),
		  "i915: Writing misaligned OA head pointer");

	/* Note: we update the head pointer here even if an error
	 * was returned since the error may represent a short read
	 * where some some reports were successfully copied.
	 */
	I915_WRITE(GEN8_OAHEADPTR, head & GEN8_OAHEADPTR_MASK);

	return ret;
}

/**
 * Copies all buffered OA reports into userspace read() buffer.
 * @head_ptr: (inout): the head pointer before and after appending
 *
 * Returns 0 on success, negative error code on failure.
 *
 * Notably any error condition resulting in a short read (-ENOSPC or
 * -EFAULT) will be returned even though one or more records may
 * have been successfully copied. In this case the error may be
 * squashed before returning to userspace.
 *
 * NB: @head_ptr may be updated even if an error is returned
 * (e.g. for a short read resulting in -ENOSPC or -EFAULT.)
 */
static int gen7_append_oa_reports(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state,
				  u32 *head_ptr,
				  u32 tail, u32 ts, u32 max_records)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	int tail_margin = dev_priv->perf.oa.tail_margin;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u32 head;
	u32 taken;
	int ret = 0;
	int n_records = 0;

	BUG_ON(stream->state != I915_PERF_STREAM_ENABLED);

	head = *head_ptr - dev_priv->perf.oa.oa_buffer.gtt_offset;
	tail -= dev_priv->perf.oa.oa_buffer.gtt_offset;

	/* The OA unit is expected to wrap the tail pointer according to the OA
	 * buffer size and since we should never write a misaligned head
	 * pointer we don't expect to read one back either...
	 */
	if (tail > OA_BUFFER_SIZE || head > OA_BUFFER_SIZE ||
	    head % report_size) {
		DRM_ERROR("Inconsistent OA buffer pointer (head = %u, tail = %u): force restart",
			  head, tail);
		dev_priv->perf.oa.ops.oa_disable(dev_priv);
		dev_priv->perf.oa.ops.oa_enable(dev_priv);
		*head_ptr = I915_READ(GEN7_OASTATUS2) &
			GEN7_OASTATUS2_HEAD_MASK;
		return -EIO;
	}


	/* The tail pointer increases in 64 byte increments, not in report_size
	 * steps...
	 */
	tail &= ~(report_size - 1);

	/* Move the tail pointer back by the current tail_margin to account for
	 * the possibility that the latest reports may not have really landed
	 * in memory yet...
	 */

	if (OA_TAKEN(tail, head) < report_size + tail_margin)
		return -EAGAIN;

	tail -= tail_margin;
	tail &= mask;

	for (/* none */;
	     (taken = OA_TAKEN(tail, head)) && (n_records <= max_records);
	     head = (head + report_size) & mask) {
		u8 *report = oa_buf_base + head;
		u32 *report32 = (void *)report;

		/* All the report sizes factor neatly into the buffer
		 * size so we never expect to see a report split
		 * between the beginning and end of the buffer.
		 *
		 * Given the initial alignment check a misalignment
		 * here would imply a driver bug that would result
		 * in an overrun.
		 */
		BUG_ON((OA_BUFFER_SIZE - head) < report_size);

		/* The report-ID field for periodic samples includes
		 * some undocumented flags related to what triggered
		 * the report and is never expected to be zero so we
		 * can check that the report isn't invalid before
		 * copying it to userspace...
		 */
		if (report32[0] == 0) {
			DRM_ERROR("Skipping spurious, invalid OA report\n");
			continue;
		}

		/* Report timestamp should not exceed passed in ts */
		if (report32[1] > ts)
			break;

		ret = append_oa_buffer_sample(stream, read_state, report);
		if (ret)
			break;
		n_records++;

		/* The above report-id field sanity check is based on
		 * the assumption that the OA buffer is initially
		 * zeroed and we reset the field after copying so the
		 * check is still meaningful once old reports start
		 * being overwritten.
		 */
		report32[0] = 0;
	}

	*head_ptr = dev_priv->perf.oa.oa_buffer.gtt_offset + head;

	return ret;
}

/**
 * Check OA status registers, appending status records if necessary
 * and copy as many buffered OA reports to userspace as possible.
 *
 * NB: some data may be successfully copied to the userspace buffer
 * even if an error is returned, and this is reflected in the
 * updated @read_state.
 */
static int gen7_oa_read(struct i915_perf_stream *stream,
			struct i915_perf_read_state *read_state,
			u32 ts, u32 max_records)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u32 oastatus2;
	u32 oastatus1;
	u32 head;
	u32 tail;
	int ret;

	BUG_ON(!dev_priv->perf.oa.oa_buffer.addr);

	oastatus2 = I915_READ(GEN7_OASTATUS2);
	oastatus1 = I915_READ(GEN7_OASTATUS1);

	head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	/* XXX: On Haswell we don't have a safe way to clear oastatus1
	 * bits while the OA unit is enabled (while the tail pointer
	 * may be updated asynchronously) so we ignore status bits
	 * that have already been reported to userspace.
	 */
	oastatus1 &= ~dev_priv->perf.oa.gen7_latched_oastatus1;

	/* We treat OABUFFER_OVERFLOW as a significant error:
	 *
	 * - The status can be interpreted to mean that the buffer is
	 *   currently full (with a higher precedence than OA_TAKEN()
	 *   which will start to report a near-empty buffer after an
	 *   overflow) but it's awkward that we can't clear the status
	 *   on Haswell, so without a reset we won't be able to catch
	 *   the state again.
	 *
	 * - Since it also implies the HW has started overwriting old
	 *   reports it may also affect our sanity checks for invalid
	 *   reports when copying to userspace that assume new reports
	 *   are being written to cleared memory.
	 *
	 * - In the future we may want to introduce a flight recorder
	 *   mode where the driver will automatically maintain a safe
	 *   guard band between head/tail, avoiding this overflow
	 *   condition, but we avoid the added driver complexity for
	 *   now.
	 */
	if (unlikely(oastatus1 & GEN7_OASTATUS1_OABUFFER_OVERFLOW)) {
		ret = append_oa_status(stream, read_state,
				       DRM_I915_PERF_RECORD_OA_BUFFER_LOST);
		if (ret)
			return ret;

		DRM_ERROR("OA buffer overflow: force restart");

		dev_priv->perf.oa.ops.oa_disable(dev_priv);
		dev_priv->perf.oa.ops.oa_enable(dev_priv);

		oastatus2 = I915_READ(GEN7_OASTATUS2);
		oastatus1 = I915_READ(GEN7_OASTATUS1);

		head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
		tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;
	}

	if (unlikely(oastatus1 & GEN7_OASTATUS1_REPORT_LOST)) {
		ret = append_oa_status(stream, read_state,
				       DRM_I915_PERF_RECORD_OA_REPORT_LOST);
		if (ret)
			return ret;
		dev_priv->perf.oa.gen7_latched_oastatus1 |=
			GEN7_OASTATUS1_REPORT_LOST;
	}

	ret = gen7_append_oa_reports(stream, read_state, &head, tail,
				ts, max_records);

	/* All the report sizes are a power of two and the
	 * head should always be incremented by some multiple
	 * of the report size.
	 *
	 * A warning here, but notably if we later read back a
	 * misaligned pointer we will treat that as a bug since
	 * it could lead to a buffer overrun.
	 */
	WARN_ONCE(head & (report_size - 1),
		  "i915: Writing misaligned OA head pointer");

	/* Note: we update the head pointer here even if an error
	 * was returned since the error may represent a short read
	 * where some some reports were successfully copied.
	 */
	I915_WRITE(GEN7_OASTATUS2,
		   ((head & GEN7_OASTATUS2_HEAD_MASK) |
		    OA_MEM_SELECT_GGTT));

	return ret;
}

/**
 * Copy one command stream report into userspace read() buffer.
 * For OA reports, also forward the periodic OA reports with timestamp
 * lower than current CS OA sample.
 *
 * NB: some data may be successfully copied to the userspace buffer
 * even if an error is returned, and this is reflected in the
 * updated @read_state.
 */
static int append_one_cs_sample(struct i915_perf_stream *stream,
				 struct i915_perf_read_state *read_state,
				 struct i915_perf_cs_data_node *node)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	enum intel_engine_id id = stream->engine;
	struct sample_data data = { 0 };
	u32 sample_flags = stream->sample_flags;
	u64 gpu_ts = 0;
	int ret = 0;

	if (sample_flags & SAMPLE_OA_REPORT) {
		const u8 *report = dev_priv->perf.command_stream_buf[id].addr +
				   node->oa_offset;
		u32 sample_ts = *(u32 *)(report + 4);

		data.report = report;

		/* First, append the periodic OA samples having lower
		 * timestamp values
		 */
		ret = dev_priv->perf.oa.ops.read(stream, read_state, sample_ts,
						U32_MAX);
		if (ret)
			return ret;

		if (sample_flags & (SAMPLE_TS|SAMPLE_CLK_MONO_RAW))
			gpu_ts = get_gpu_ts_from_oa_report(dev_priv, report);
	}

	if (sample_flags & SAMPLE_OA_SOURCE_INFO)
		data.source = I915_PERF_OA_EVENT_SOURCE_RCS;

	if (sample_flags & SAMPLE_CTX_ID) {
		data.ctx_id = node->ctx_id;
		dev_priv->perf.last_ctx_id = node->ctx_id;
	}

	if (sample_flags & SAMPLE_PID) {
		data.pid = node->pid;
		dev_priv->perf.last_pid = node->pid;
	}

	if (sample_flags & SAMPLE_TAG) {
		data.tag = node->tag;
		dev_priv->perf.last_tag = node->tag;
	}

	if (sample_flags & (SAMPLE_TS|SAMPLE_CLK_MONO_RAW)) {
		/* If OA sampling is enabled, derive the ts from OA report.
		 * Else, forward the timestamp collected via command stream.
		 */
		if (!(sample_flags & SAMPLE_OA_REPORT))
			gpu_ts = *(u64 *)
				(dev_priv->perf.command_stream_buf[id].addr +
					node->ts_offset);

		if (sample_flags & SAMPLE_TS)
			data.gpu_ts = gpu_ts;
		if (sample_flags & SAMPLE_CLK_MONO_RAW)
			data.clk_monoraw = get_clk_monoraw_from_gpu_ts(
							stream,	gpu_ts);
	}

	if (sample_flags & SAMPLE_MMIO)
		data.mmio = dev_priv->perf.command_stream_buf[id].addr +
				node->mmio_offset;

	return append_sample(stream, read_state, &data);
}

/**
 * Copies all samples into userspace read() buffer. This includes command
 * stream samples as well as periodic OA reports (if enabled).
 *
 * NB: some data may be successfully copied to the userspace buffer
 * even if an error is returned, and this is reflected in the
 * updated @read_state.
 */
static int append_command_stream_samples(struct i915_perf_stream *stream,
				  struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	struct i915_perf_cs_data_node *entry, *next;
	enum intel_engine_id id = stream->engine;
	LIST_HEAD(free_list);
	int ret = 0;
#ifndef CMD_STREAM_BUF_OVERFLOW_ALLOWED
	u32 cs_buf_status = dev_priv->perf.command_stream_buf.status;

	if (unlikely(cs_buf_status &
			I915_PERF_CMD_STREAM_BUF_STATUS_OVERFLOW)) {
		ret = append_oa_status(stream, read_state,
				       DRM_I915_PERF_RECORD_OA_BUFFER_OVERFLOW);
		if (ret)
			return ret;

		dev_priv->perf.command_stream_buf.status &=
				~I915_PERF_CMD_STREAM_BUF_STATUS_OVERFLOW;
	}
#endif
	spin_lock(&dev_priv->perf.node_list_lock[id]);
	if (list_empty(&dev_priv->perf.node_list[id])) {
		spin_unlock(&dev_priv->perf.node_list_lock[id]);
		goto pending_periodic;
	}
	list_for_each_entry_safe(entry, next,
				 &dev_priv->perf.node_list[id], link) {
		if (!i915_gem_request_completed(entry->request, true))
			break;
		list_move_tail(&entry->link, &free_list);
	}
	spin_unlock(&dev_priv->perf.node_list_lock[id]);

	if (list_empty(&free_list))
		goto pending_periodic;

	list_for_each_entry_safe(entry, next, &free_list, link) {
		ret = append_one_cs_sample(stream, read_state, entry);
		if (ret)
			break;

		list_del(&entry->link);
		i915_gem_request_unreference(entry->request);
		kfree(entry);
	}

	/* Don't discard remaining entries, keep them for next read */
	spin_lock(&dev_priv->perf.node_list_lock[id]);
	list_splice(&free_list, &dev_priv->perf.node_list[id]);
	spin_unlock(&dev_priv->perf.node_list_lock[id]);

	return ret;

pending_periodic:
	if (!((stream->sample_flags & SAMPLE_OA_REPORT) &&
			dev_priv->perf.oa.n_pending_periodic_samples))
		return 0;

	ret = dev_priv->perf.oa.ops.read(stream, read_state,
				dev_priv->perf.oa.pending_periodic_ts,
				dev_priv->perf.oa.n_pending_periodic_samples);
	dev_priv->perf.oa.n_pending_periodic_samples = 0;
	dev_priv->perf.oa.pending_periodic_ts = 0;
	return ret;
}

enum cs_buf_data_state {
	CS_BUF_EMPTY,
	CS_BUF_REQ_PENDING,
	CS_BUF_HAVE_DATA,
};

/*
 * Checks whether the command stream buffer associated with the stream has
 * data ready to be forwarded to userspace.
 * Value returned:
 * CS_BUF_HAVE_DATA	- if there is atleast one completed request
 * CS_BUF_REQ_PENDING	- there are requests pending, but no completed requests
 * CS_BUF_EMPTY		- no requests scheduled
 */
static enum cs_buf_data_state command_stream_buf_state(
				struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	enum intel_engine_id id = stream->engine;
	struct i915_perf_cs_data_node *entry = NULL;
	struct drm_i915_gem_request *request = NULL;

	spin_lock(&dev_priv->perf.node_list_lock[id]);
	entry = list_first_entry_or_null(&dev_priv->perf.node_list[id],
			struct i915_perf_cs_data_node, link);
	if (entry)
		request = entry->request;
	spin_unlock(&dev_priv->perf.node_list_lock[id]);

	if (!entry)
		return CS_BUF_EMPTY;
	else if (!i915_gem_request_completed(request, true))
		return CS_BUF_REQ_PENDING;
	else
		return CS_BUF_HAVE_DATA;
}

/*
 * Checks whether the stream has data ready to forward to userspace, by
 * querying for periodic oa buffer and command stream buffer samples.
 */

static bool stream_have_data__unlocked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	enum cs_buf_data_state cs_buf_state = CS_BUF_EMPTY;
	u32 num_samples = 0, last_ts = 0;

	if (stream->sample_flags & SAMPLE_OA_REPORT) {
		/* Note: oa_buffer_num_samples() is ok to run unlocked as it
		 * just performs mmio reads of the OA buffer head + tail
		 * pointers and it's assumed we're handling some operation that
		 * implies the stream can't be destroyed until completion (such
		 * as a read()) that ensures the device + OA buffer can't
		 * disappear
		 */
		dev_priv->perf.oa.n_pending_periodic_samples = 0;
		dev_priv->perf.oa.pending_periodic_ts = 0;
		num_samples = dev_priv->perf.oa.ops.oa_buffer_num_samples(
							dev_priv, &last_ts);
	} else if (stream->cs_mode)
		cs_buf_state = command_stream_buf_state(stream);

	/*
	 * Note: We can safely forward the periodic OA samples in the case we
	 * have no pending CS samples, but we can't do so in the case we have
	 * pending CS samples, since we don't know what the ordering between
	 * pending CS samples and periodic samples will eventually be. If we
	 * have no pending CS sample, it won't be possible for future pending CS
	 * sample to have timestamps earlier than current periodic timestamp.
	 */
	switch (cs_buf_state) {
	case CS_BUF_EMPTY:
		if (stream->sample_flags & SAMPLE_OA_REPORT) {
			dev_priv->perf.oa.n_pending_periodic_samples =
								num_samples;
			dev_priv->perf.oa.pending_periodic_ts = last_ts;
			return (num_samples != 0);
		} else
			return false;

	case CS_BUF_HAVE_DATA:
		return true;

	case CS_BUF_REQ_PENDING:
		default:
		return false;
	}
}

static bool i915_ring_stream_can_read_unlocked(struct i915_perf_stream *stream)
{

	return stream_have_data__unlocked(stream);
}

static int i915_ring_stream_wait_unlocked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	enum intel_engine_id id = stream->engine;
	int ret;

	/* We would wait indefinitly if periodic sampling is not enabled */
	if (!dev_priv->perf.oa.periodic)
		return -EIO;

	if (stream->cs_mode) {
		ret = i915_ring_stream_wait_gpu(dev_priv, id);
		if (ret)
			return ret;
	}

	return wait_event_interruptible(dev_priv->perf.poll_wq[id],
					stream_have_data__unlocked(stream));
}

static void i915_ring_stream_poll_wait(struct i915_perf_stream *stream,
			      struct file *file,
			      poll_table *wait)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	poll_wait(file, &dev_priv->perf.poll_wq[stream->engine], wait);
}

static int i915_ring_stream_read(struct i915_perf_stream *stream,
			struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (stream->cs_mode)
		return append_command_stream_samples(stream, read_state);
	else if (stream->sample_flags & SAMPLE_OA_REPORT)
		return dev_priv->perf.oa.ops.read(stream, read_state,
						U32_MAX, U32_MAX);
	else
		return -EINVAL;
}

static void
free_command_stream_buf(struct drm_i915_private *dev_priv,
				enum intel_engine_id id)
{
	mutex_lock(&dev_priv->dev->struct_mutex);

	i915_gem_object_unpin_map(dev_priv->perf.command_stream_buf[id].obj);
	i915_gem_object_ggtt_unpin(dev_priv->perf.command_stream_buf[id].obj);
	drm_gem_object_unreference(
			&dev_priv->perf.command_stream_buf[id].obj->base);

	dev_priv->perf.command_stream_buf[id].obj = NULL;
	dev_priv->perf.command_stream_buf[id].vma = NULL;
	dev_priv->perf.command_stream_buf[id].addr = NULL;

	mutex_unlock(&dev_priv->dev->struct_mutex);
}

static void
free_oa_buffer(struct drm_i915_private *i915)
{
	mutex_lock(&i915->dev->struct_mutex);

	i915_gem_object_unpin_map(i915->perf.oa.oa_buffer.obj);
	i915_gem_object_ggtt_unpin(i915->perf.oa.oa_buffer.obj);
	drm_gem_object_unreference(&i915->perf.oa.oa_buffer.obj->base);

	i915->perf.oa.oa_buffer.obj = NULL;
	i915->perf.oa.oa_buffer.gtt_offset = 0;
	i915->perf.oa.oa_buffer.addr = NULL;

	mutex_unlock(&i915->dev->struct_mutex);
}

static void i915_ring_stream_destroy(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	BUG_ON(stream != dev_priv->perf.ring_stream[stream->engine]);

	if (stream->using_oa) {
		dev_priv->perf.oa.ops.disable_metric_set(dev_priv);

		free_oa_buffer(dev_priv);

		intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
		intel_runtime_pm_put(dev_priv);
	}

	if (stream->cs_mode)
		free_command_stream_buf(dev_priv, stream->engine);

	dev_priv->perf.ring_stream[stream->engine] = NULL;
}

static void gen7_init_oa_buffer(struct drm_i915_private *dev_priv)
{
	/* Pre-DevBDW: OABUFFER must be set with counters off,
	 * before OASTATUS1, but after OASTATUS2
	 */
	I915_WRITE(GEN7_OASTATUS2, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OA_MEM_SELECT_GGTT); /* head */
	I915_WRITE(GEN7_OABUFFER, dev_priv->perf.oa.oa_buffer.gtt_offset);
	I915_WRITE(GEN7_OASTATUS1, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OABUFFER_SIZE_16M); /* tail */

	/* On Haswell we have to track which OASTATUS1 flags we've
	 * already seen since they can't be cleared while periodic
	 * sampling is enabled.
	 */
	dev_priv->perf.oa.gen7_latched_oastatus1 = 0;

	/* NB: although the OA buffer will initially be allocated
	 * zeroed via shmfs (and so this memset is redundant when
	 * first allocating), we may re-init the OA buffer, either
	 * when re-enabling a stream or in error/reset paths.
	 *
	 * The reason we clear the buffer for each re-init is for the
	 * sanity check in gen7_append_oa_reports() that looks at the
	 * report-id field to make sure it's non-zero which relies on
	 * the assumption that new reports are being written to zeroed
	 * memory...
	 */
	memset(dev_priv->perf.oa.oa_buffer.addr, 0, SZ_16M);
}

static void gen8_init_oa_buffer(struct drm_i915_private *dev_priv)
{
#warning "gen8_init_oa_buffer(): should we clear OASTATUS here?"
	I915_WRITE(GEN8_OAHEADPTR,
		   dev_priv->perf.oa.oa_buffer.gtt_offset);

	I915_WRITE(GEN8_OABUFFER_UDW, 0);

	/* PRM says:
	 *
	 *  "This MMIO must be set before the OATAILPTR
	 *  register and after the OAHEADPTR register. This is
	 *  to enable proper functionality of the overflow
	 *  bit."
	 */
	I915_WRITE(GEN8_OABUFFER, dev_priv->perf.oa.oa_buffer.gtt_offset |
		   OABUFFER_SIZE_16M | OA_MEM_SELECT_GGTT);
	I915_WRITE(GEN8_OATAILPTR,
		   (dev_priv->perf.oa.oa_buffer.gtt_offset &
		    GEN8_OATAILPTR_MASK));
}

static int alloc_obj(struct drm_i915_private *dev_priv,
		     struct drm_i915_gem_object **obj, u8 **addr)
{
	struct drm_i915_gem_object *bo;
	int ret;

	intel_runtime_pm_get(dev_priv);

	ret = i915_mutex_lock_interruptible(dev_priv->dev);
	if (ret)
		goto out;

	bo = i915_gem_object_create(dev_priv->dev, OA_BUFFER_SIZE);
	if (IS_ERR(bo)) {
		DRM_ERROR("Failed to allocate OA buffer\n");
		ret = PTR_ERR(bo);
		goto unlock;
	}

	ret = i915_gem_object_set_cache_level(bo, I915_CACHE_LLC);
	if (ret)
		goto err_unref;

	/* PreHSW required 512K alignment, HSW requires 16M */
	ret = i915_gem_obj_ggtt_pin(bo, SZ_16M, 0);
	if (ret)
		goto err_unref;

	*addr = i915_gem_object_pin_map(bo);
	if (*addr == NULL) {
		ret = -ENOMEM;
		goto err_unpin;
	}

	*obj = bo;
	goto unlock;

err_unpin:
	i915_gem_object_ggtt_unpin(bo);

err_unref:
	drm_gem_object_unreference(&bo->base);

unlock:
	mutex_unlock(&dev_priv->dev->struct_mutex);

out:
	intel_runtime_pm_put(dev_priv);
	return ret;
}

static int alloc_oa_buffer(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *bo;
	u8 *obj_addr;
	int ret;

	BUG_ON(dev_priv->perf.oa.oa_buffer.obj);

	ret = alloc_obj(dev_priv, &bo, &obj_addr);
	if (ret)
		return ret;

	dev_priv->perf.oa.oa_buffer.obj = bo;
	dev_priv->perf.oa.oa_buffer.addr = obj_addr;
	dev_priv->perf.oa.oa_buffer.gtt_offset = i915_gem_obj_ggtt_offset(bo);

	dev_priv->perf.oa.ops.init_oa_buffer(dev_priv);

	DRM_DEBUG_DRIVER("OA Buffer initialized, gtt offset = 0x%x, vaddr = %p",
			 dev_priv->perf.oa.oa_buffer.gtt_offset,
			 dev_priv->perf.oa.oa_buffer.addr);
	return 0;
}

static int alloc_command_stream_buf(struct drm_i915_private *dev_priv,
					enum intel_engine_id id)
{
	struct drm_i915_gem_object *bo;
	u8 *obj_addr;
	int ret;

	BUG_ON(dev_priv->perf.command_stream_buf[id].obj);

	ret = alloc_obj(dev_priv, &bo, &obj_addr);
	if (ret)
		return ret;

	dev_priv->perf.command_stream_buf[id].obj = bo;
	dev_priv->perf.command_stream_buf[id].addr = obj_addr;
	dev_priv->perf.command_stream_buf[id].vma = i915_gem_obj_to_ggtt(bo);
	if (WARN_ON(!list_empty(&dev_priv->perf.node_list[id])))
		INIT_LIST_HEAD(&dev_priv->perf.node_list[id]);

	DRM_DEBUG_DRIVER(
		"command stream buf initialized, gtt offset = 0x%x, vaddr = %p",
		 (unsigned int)
		 dev_priv->perf.command_stream_buf[id].vma->node.start,
		 dev_priv->perf.command_stream_buf[id].addr);

	return 0;
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

	I915_WRITE(GDT_CHICKEN_BITS, (I915_READ(GDT_CHICKEN_BITS) |
				      GT_NOA_ENABLE));

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

	/* It takes a fairly long time for a new MUX configuration to
	 * be be applied after these register writes. This delay
	 * duration was derived empirically based on the render_basic
	 * config but hopefully it covers the maximum configuration
	 * latency...
	 */
	msleep(100);

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

/* Manages updating the per-context aspects of the OA stream
 * configuration across all contexts.
 *
 * The awkward consideration here is that OACTXCONTROL controls the
 * exponent for periodic sampling which is primarily used for system
 * wide profiling where we'd like a consistent sampling period even in
 * the face of context switches.
 *
 * Our approach of updating the register state context (as opposed to
 * say using a workaround batch buffer) ensures that the hardware
 * won't automatically reload an out-of-date timer exponent even
 * transiently before a WA BB could be parsed.
 */
static int configure_all_contexts(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = dev_priv->dev;
	struct intel_context *ctx;
	int ret;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	list_for_each_entry(ctx, &dev_priv->context_list, link) {
		int i;

		for (i = 0; i < I915_NUM_ENGINES; i++) {
			/* The actual update of the register state context
			 * will happen the next time this logical ring
			 * is submitted. (See i915_oa_update_reg_state()
			 * which hooks into execlists_update_context())
			 */
			atomic_set(&ctx->engine[i].oa_state_dirty, true);
		}
	}

	mutex_unlock(&dev->struct_mutex);

	/* Now update the current context.
	 *
	 * Note: Using MMIO to update per-context registers requires
	 * some extra care...
	 */
	ret = intel_uncore_begin_ctx_mmio(dev_priv);
	if (ret) {
		DRM_ERROR("Failed to bring RCS out of idle to update current ctx OA state");
		return ret;
	}

	I915_WRITE(GEN8_OACTXCONTROL, ((dev_priv->perf.oa.period_exponent <<
					GEN8_OA_TIMER_PERIOD_SHIFT) |
				      (dev_priv->perf.oa.periodic ?
				       GEN8_OA_TIMER_ENABLE : 0) |
				      GEN8_OA_COUNTER_RESUME));

	config_oa_regs(dev_priv, dev_priv->perf.oa.flex_regs,
			dev_priv->perf.oa.flex_regs_len);

	intel_uncore_end_ctx_mmio(dev_priv);

	return 0;
}

static int bdw_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_bdw(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);

	/* It takes a fairly long time for a new MUX configuration to
	 * be be applied after these register writes. This delay
	 * duration is take from Haswell (derived empirically based on
	 * the render_basic config) but hopefully it covers the
	 * maximum configuration latency on BDW too...
	 */
	mdelay(100);

	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void bdw_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "BDW: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static int chv_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_chv(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);

	/* It takes a fairly long time for a new MUX configuration to
	 * be be applied after these register writes. This delay
	 * duration is take from Haswell (derived empirically based on
	 * the render_basic config) but hopefully it covers the
	 * maximum configuration latency on CHV too...
	 */
	mdelay(100);

	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void chv_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "CHV: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static int skl_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_skl(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);

	/* It takes a fairly long time for a new MUX configuration to
	 * be be applied after these register writes. This delay
	 * duration is take from Haswell (derived empirically based on
	 * the render_basic config) but hopefully it covers the
	 * maximum configuration latency on CHV too...
	 */
	mdelay(100);

	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void skl_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "SKL: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static int bxt_enable_metric_set(struct drm_i915_private *dev_priv)
{
	int ret = i915_oa_select_metric_set_bxt(dev_priv);

	if (ret)
		return ret;

	I915_WRITE(GDT_CHICKEN_BITS, 0xA0);
	config_oa_regs(dev_priv, dev_priv->perf.oa.mux_regs,
		       dev_priv->perf.oa.mux_regs_len);
	I915_WRITE(GDT_CHICKEN_BITS, 0x80);
	config_oa_regs(dev_priv, dev_priv->perf.oa.b_counter_regs,
		       dev_priv->perf.oa.b_counter_regs_len);

	configure_all_contexts(dev_priv);

	return 0;
}

static void bxt_disable_metric_set(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_UCGCTL1, (I915_READ(GEN6_UCGCTL1) &
				  ~GEN6_CSUNIT_CLOCK_GATE_DISABLE));
	I915_WRITE(GEN7_MISCCPCTL, (I915_READ(GEN7_MISCCPCTL) |
				    GEN7_DOP_CLOCK_GATE_ENABLE));
#warning "BXT: Do we need to write to CHICKEN2 to disable DOP clock gating when idle? (vpg does this)"
}

static void gen7_update_oacontrol_locked(struct drm_i915_private *dev_priv)
{
	assert_spin_locked(&dev_priv->perf.hook_lock);

	if (dev_priv->perf.ring_stream[RCS]->state !=
					I915_PERF_STREAM_DISABLED) {
		unsigned long ctx_id = 0;

		if (dev_priv->perf.ring_stream[RCS]->ctx)
			ctx_id = dev_priv->perf.oa.specific_ctx_id;

		if (dev_priv->perf.ring_stream[RCS]->ctx == NULL || ctx_id) {
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
	unsigned long flags;

	/* Reset buf pointers so we don't forward reports from before now. */
	gen7_init_oa_buffer(dev_priv);

	spin_lock_irqsave(&dev_priv->perf.hook_lock, flags);
	gen7_update_oacontrol_locked(dev_priv);
	spin_unlock_irqrestore(&dev_priv->perf.hook_lock, flags);
}

static void gen8_oa_enable(struct drm_i915_private *dev_priv)
{
	u32 report_format = dev_priv->perf.oa.oa_buffer.format;

	/* Reset buf pointers so we don't forward reports from before now. */
	gen8_init_oa_buffer(dev_priv);

	/* Note: we don't rely on the hardware to perform single context
	 * filtering and instead filter on the cpu based on the context-id
	 * field of reports */
	I915_WRITE(GEN8_OACONTROL, (report_format <<
				    GEN8_OA_REPORT_FORMAT_SHIFT) |
				   GEN8_OA_COUNTER_ENABLE);
}

static void i915_ring_stream_enable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	if (stream->sample_flags & SAMPLE_OA_REPORT) {
		dev_priv->perf.oa.ops.oa_enable(dev_priv);

		if (stream->sample_flags & (SAMPLE_TS|SAMPLE_CLK_MONO_RAW))
			dev_priv->perf.oa.last_gpu_ts =
				I915_READ64_2x32(GT_TIMESTAMP_COUNT,
					GT_TIMESTAMP_COUNT_UDW);
	}

	if (stream->sample_flags & SAMPLE_CLK_MONO_RAW) {
		struct system_device_crosststamp *xtstamp =
			&dev_priv->ts_clk_info.xtstamp;

		get_device_system_crosststamp(i915_get_syncdevicetime, dev_priv,
					NULL, xtstamp);
		dev_priv->ts_clk_info.clk_offset = ktime_sub(xtstamp->sys_monoraw,
						xtstamp->device);

		if (dev_priv->ts_clk_info.resync_period < MAX_CLK_SYNC_PERIOD)
			schedule_delayed_work(
				&dev_priv->ts_clk_info.clk_sync_work,
				msecs_to_jiffies(
					dev_priv->ts_clk_info.resync_period));
	}

	if (stream->cs_mode || dev_priv->perf.oa.periodic)
		hrtimer_start(&dev_priv->perf.poll_check_timer,
			      ns_to_ktime(POLL_PERIOD),
			      HRTIMER_MODE_REL_PINNED);
}

static void gen7_oa_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN7_OACONTROL, 0);
}

static void gen8_oa_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN8_OACONTROL, 0);
}

static void i915_ring_stream_disable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	cancel_delayed_work_sync(&dev_priv->ts_clk_info.clk_sync_work);

	if (stream->cs_mode || dev_priv->perf.oa.periodic)
		hrtimer_cancel(&dev_priv->perf.poll_check_timer);

	if (stream->cs_mode) {
		i915_ring_stream_wait_gpu(dev_priv, stream->engine);
		i915_ring_stream_free_requests(dev_priv, stream->engine);
	}

	if (stream->sample_flags & SAMPLE_OA_REPORT)
		dev_priv->perf.oa.ops.oa_disable(dev_priv);
}

static u64 oa_exponent_to_ns(struct drm_i915_private *dev_priv, int exponent)
{
	return 1000000000ULL * (2ULL << exponent) /
		dev_priv->ts_clk_info.timestamp_frequency;
}

static const struct i915_perf_stream_ops i915_oa_stream_ops = {
	.destroy = i915_ring_stream_destroy,
	.enable = i915_ring_stream_enable,
	.disable = i915_ring_stream_disable,
	.can_read_unlocked = i915_ring_stream_can_read_unlocked,
	.wait_unlocked = i915_ring_stream_wait_unlocked,
	.poll_wait = i915_ring_stream_poll_wait,
	.read = i915_ring_stream_read,
	.command_stream_hook = i915_ring_stream_cs_hook,
};

static int i915_ring_stream_init(struct i915_perf_stream *stream,
			       struct drm_i915_perf_open_param *param,
			       struct perf_open_properties *props)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	bool require_oa_unit = props->sample_flags & (SAMPLE_OA_REPORT |
						      SAMPLE_OA_SOURCE_INFO);
	bool require_cs_mode = props->sample_flags & (SAMPLE_PID |
						      SAMPLE_TAG |
						      SAMPLE_MMIO);
	bool cs_sample_data = props->sample_flags & (SAMPLE_OA_REPORT |
							SAMPLE_TS |
							SAMPLE_CLK_MONO_RAW |
							SAMPLE_MMIO);
	int ret;

	if ((props->sample_flags & SAMPLE_CTX_ID) && !props->cs_mode) {
		if (IS_HASWELL(dev_priv->dev)) {
			DRM_ERROR(
				"On HSW, context ID sampling only supported via command stream");
			return -EINVAL;
		} else if (!i915.enable_execlists) {
			DRM_ERROR(
				"On Gen8+ without execlists, context ID sampling only supported via command stream");
			return -EINVAL;
		}
	}

	stream->sample_size = sizeof(struct drm_i915_perf_record_header);

	if (require_oa_unit) {
		int format_size;

		/* Only allow exclusive access per stream */
		if (dev_priv->perf.ring_stream[RCS]) {
			DRM_ERROR("Stream:0 already in use\n");
			return -EBUSY;
		}

		if (!dev_priv->perf.oa.ops.init_oa_buffer) {
			DRM_ERROR("OA unit not supported\n");
			return -ENODEV;
		}

		if (!props->metrics_set) {
			DRM_ERROR("OA metric set not specified\n");
			return -EINVAL;
		}

		if (!props->oa_format) {
			DRM_ERROR("OA report format not specified\n");
			return -EINVAL;
		}

		if (props->cs_mode  && (props->engine!= RCS)) {
			DRM_ERROR(
				"Command stream OA metrics only available via Render CS\n");
			return -EINVAL;
		}
		stream->engine= RCS;
		stream->using_oa = true;

		format_size =
			dev_priv->perf.oa.oa_formats[props->oa_format].size;

		if (props->sample_flags & SAMPLE_OA_REPORT) {
			stream->sample_flags |= SAMPLE_OA_REPORT;
			stream->sample_size += format_size;
		}

		if (props->sample_flags & SAMPLE_OA_SOURCE_INFO) {
			if (!(props->sample_flags & SAMPLE_OA_REPORT)) {
				DRM_ERROR(
					  "OA source type can't be sampled without OA report");
				return -EINVAL;
			}
			stream->sample_flags |= SAMPLE_OA_SOURCE_INFO;
			stream->sample_size += 4;
		}

		dev_priv->perf.oa.oa_buffer.format_size = format_size;
		BUG_ON(dev_priv->perf.oa.oa_buffer.format_size == 0);

		dev_priv->perf.oa.oa_buffer.format =
			dev_priv->perf.oa.oa_formats[props->oa_format].format;

		dev_priv->perf.oa.metrics_set = props->metrics_set;

		dev_priv->perf.oa.periodic = props->oa_periodic;
		if (dev_priv->perf.oa.periodic) {
			u64 period_ns = oa_exponent_to_ns(dev_priv,
						props->oa_period_exponent);

			dev_priv->perf.oa.period_exponent =
						props->oa_period_exponent;

			/* See comment for OA_TAIL_MARGIN_NSEC for details
			 * about this tail_margin...
			 */
			dev_priv->perf.oa.tail_margin =
				((OA_TAIL_MARGIN_NSEC / period_ns) + 1) *
				format_size;
		}

		if (i915.enable_execlists && stream->ctx)
			dev_priv->perf.oa.specific_ctx_id = stream->ctx->hw_id;

		ret = alloc_oa_buffer(dev_priv);
		if (ret)
			return ret;

		/* PRM - observability performance counters:
		 *
		 *   OACONTROL, performance counter enable, note:
		 *
		 *   "When this bit is set, in order to have coherent counts,
		 *   RC6 power state and trunk clock gating must be disabled.
		 *   This can be achieved by programming MMIO registers as
		 *   0xA094=0 and 0xA090[31]=1"
		 *
		 *   In our case we are expecting that taking pm + FORCEWAKE
		 *   references will effectively disable RC6.
		 */
		intel_runtime_pm_get(dev_priv);
		intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

		ret = dev_priv->perf.oa.ops.enable_metric_set(dev_priv);
		if (ret) {
			intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
			intel_runtime_pm_put(dev_priv);
			free_oa_buffer(dev_priv);
			return ret;
		}

		/* On Haswell we have to track which OASTATUS1 flags we've already
		 * seen since they can't be cleared while periodic sampling is enabled.
		 */
		dev_priv->perf.oa.gen7_latched_oastatus1 = 0;
	}

	if (props->sample_flags & SAMPLE_CTX_ID) {
		stream->sample_flags |= SAMPLE_CTX_ID;
		stream->sample_size += 4;

		/*
		 * NB: it's meaningful to request SAMPLE_CTX_ID with just CS
		 * mode or periodic OA mode sampling but we don't allow
		 * SAMPLE_CTX_ID without either mode
		 */
		if (!require_oa_unit)
			require_cs_mode = true;
	}

	if (props->sample_flags & SAMPLE_TS) {
		stream->sample_flags |= SAMPLE_TS;
		stream->sample_size += I915_PERF_TS_SAMPLE_SIZE;

		/*
		 * NB: it's meaningful to request SAMPLE_TS with just CS
		 * mode or periodic OA mode sampling but we don't allow
		 * SAMPLE_TS without either mode
		 */
		if (!require_oa_unit)
			require_cs_mode = true;
	}

	if (props->sample_flags & SAMPLE_CLK_MONO_RAW) {
		stream->sample_flags |= SAMPLE_CLK_MONO_RAW;
		stream->sample_size += I915_PERF_TS_SAMPLE_SIZE;

		/*
		 * NB: it's meaningful to request SAMPLE_CLK_MONO with just CS
		 * mode or periodic OA mode sampling but we don't allow
		 * SAMPLE_CLK_MONO without either mode
		 */
		if (!require_oa_unit)
			require_cs_mode = true;
	}

	if (require_cs_mode && !props->cs_mode) {
		DRM_ERROR(
			"PID, TAG, TS or MMIO sampling require a ring to be specified");
		ret = -EINVAL;
		goto cs_error;
	}

	if (props->cs_mode) {
		/* Only allow exclusive access per stream */
		if (dev_priv->perf.ring_stream[props->engine]) {
			DRM_ERROR("Stream:%d already in use\n", props->engine);
			ret = -EBUSY;
			goto cs_error;
		}

		if (!cs_sample_data) {
			DRM_ERROR(
				"Ring given without requesting any CS data to sample");
			ret = -EINVAL;
			goto cs_error;
		}

		/*
		 * The only time we should allow enabling CS mode if it's not
		 * strictly required, is if SAMPLE_CTX_ID, SAMPLE_TS or
		 * SAMPLE_CLK_MONO_RAW has been requested, as they're usable
		 * with periodic OA or CS sampling.
		 */
		if (!require_cs_mode &&
		    !(props->sample_flags &
				(SAMPLE_CTX_ID|SAMPLE_TS|SAMPLE_CLK_MONO_RAW))) {
			DRM_ERROR(
				"Ring given without requesting any CS specific property");
			ret = -EINVAL;
			goto cs_error;
		}

		stream->cs_mode = true;
		stream->engine = props->engine;

		if (props->sample_flags & SAMPLE_PID) {
			stream->sample_flags |= SAMPLE_PID;
			stream->sample_size += 4;
		}

		if (props->sample_flags & SAMPLE_TAG) {
			stream->sample_flags |= SAMPLE_TAG;
			stream->sample_size += 4;
		}

		if (props->sample_flags & SAMPLE_MMIO) {
			stream->sample_flags |= SAMPLE_MMIO;
			stream->sample_size += 4 * dev_priv->perf.num_mmio;
		}

		ret = alloc_command_stream_buf(dev_priv, stream->engine);
		if (ret)
			goto cs_error;
	}

	stream->ops = &i915_oa_stream_ops;

	dev_priv->perf.ring_stream[stream->engine] = stream;

	return 0;

cs_error:
	if (require_oa_unit) {
		dev_priv->perf.oa.ops.disable_metric_set(dev_priv);

		intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
		intel_runtime_pm_put(dev_priv);

		free_oa_buffer(dev_priv);
	}
	return ret;
}

static void gen7_update_hw_ctx_id_locked(struct drm_i915_private *dev_priv,
					 u32 ctx_id)
{
	assert_spin_locked(&dev_priv->perf.hook_lock);

	dev_priv->perf.oa.specific_ctx_id = ctx_id;
	gen7_update_oacontrol_locked(dev_priv);
}

static void i915_oa_context_pin_notify_locked(struct drm_i915_private *dev_priv,
					      struct intel_context *context)
{
	assert_spin_locked(&dev_priv->perf.hook_lock);

	if (i915.enable_execlists ||
	    dev_priv->perf.oa.ops.update_hw_ctx_id_locked == NULL)
		return;

	if (dev_priv->perf.ring_stream[RCS] &&
	    dev_priv->perf.ring_stream[RCS]->ctx == context) {
		struct drm_i915_gem_object *obj =
			context->legacy_hw_ctx.rcs_state;
		u32 ctx_id = i915_gem_obj_ggtt_offset(obj);

		dev_priv->perf.oa.ops.update_hw_ctx_id_locked(dev_priv, ctx_id);
	}
}

void i915_oa_context_pin_notify(struct drm_i915_private *dev_priv,
				struct intel_context *context)
{
	unsigned long flags;

	if (!dev_priv->perf.initialized)
		return;

	spin_lock_irqsave(&dev_priv->perf.hook_lock, flags);
	i915_oa_context_pin_notify_locked(dev_priv, context);
	spin_unlock_irqrestore(&dev_priv->perf.hook_lock, flags);
}

static void gen8_legacy_ctx_switch_unlocked(struct drm_i915_gem_request *req)
{
	struct drm_i915_private *dev_priv = req->i915;
	struct intel_engine_cs *engine = req->engine;
	struct intel_context *ctx = req->ctx;
	const struct i915_oa_reg *flex_regs = dev_priv->perf.oa.flex_regs;
	int n_flex_regs = dev_priv->perf.oa.flex_regs_len;
	int ret;
	int i;

	if (!atomic_read(&ctx->engine[engine->id].oa_state_dirty))
		return;

	ret = intel_ring_begin(req, n_flex_regs * 2 + 4);
	if (ret)
		return;

	intel_ring_emit(engine, MI_LOAD_REGISTER_IMM(n_flex_regs + 1));

	intel_ring_emit_reg(engine, GEN8_OACTXCONTROL);
	intel_ring_emit(engine,
			(dev_priv->perf.oa.period_exponent <<
			 GEN8_OA_TIMER_PERIOD_SHIFT) |
			(dev_priv->perf.oa.periodic ?
			 GEN8_OA_TIMER_ENABLE : 0) |
			GEN8_OA_COUNTER_RESUME);

	for (i = 0; i < n_flex_regs; i++) {
		intel_ring_emit_reg(engine, flex_regs[i].addr);
		intel_ring_emit(engine, flex_regs[i].value);
	}
	intel_ring_emit(engine, MI_NOOP);
	intel_ring_advance(engine);

	atomic_set(&ctx->engine[engine->id].oa_state_dirty, false);
}

void i915_oa_legacy_ctx_switch_notify(struct drm_i915_gem_request *req)
{
	struct drm_i915_private *dev_priv = req->i915;

	if (!dev_priv->perf.initialized)
		return;

	if (dev_priv->perf.oa.ops.legacy_ctx_switch_unlocked == NULL)
		return;

	if (dev_priv->perf.ring_stream[RCS] &&
	    dev_priv->perf.ring_stream[RCS]->state !=
				I915_PERF_STREAM_DISABLED) {

		/* XXX: We don't take a lock here and this may run
		 * async with respect to stream methods. Notably we
		 * don't want to block context switches by long i915
		 * perf read() operations.
		 *
		 * It's expect to always be safe to read the
		 * dev_priv->perf state needed here, and expected to
		 * be benign to redundantly update the state if the OA
		 * unit has been disabled since oa_state_dirty was
		 * last set.
		 */
		dev_priv->perf.oa.ops.legacy_ctx_switch_unlocked(req);
	}
}

static void gen8_update_reg_state_unlocked(struct intel_engine_cs *engine,
					   struct intel_context *ctx,
					   uint32_t *reg_state)
{
	struct drm_i915_private *dev_priv = ctx->i915;
	const struct i915_oa_reg *flex_regs = dev_priv->perf.oa.flex_regs;
	int n_flex_regs = dev_priv->perf.oa.flex_regs_len;
	int ctx_oactxctrl = dev_priv->perf.oa.ctx_oactxctrl_off;
	int ctx_flexeu0 = dev_priv->perf.oa.ctx_flexeu0_off;
	int i;

	if (!atomic_read(&ctx->engine[engine->id].oa_state_dirty))
		return;

	reg_state[ctx_oactxctrl] = i915_mmio_reg_offset(GEN8_OACTXCONTROL);
	reg_state[ctx_oactxctrl+1] = (dev_priv->perf.oa.period_exponent <<
				      GEN8_OA_TIMER_PERIOD_SHIFT) |
				     (dev_priv->perf.oa.periodic ?
				      GEN8_OA_TIMER_ENABLE : 0) |
				     GEN8_OA_COUNTER_RESUME;

	for (i = 0; i < n_flex_regs; i++) {
		uint32_t offset = i915_mmio_reg_offset(flex_regs[i].addr);

		/* Map from mmio address to register state context
		 * offset... */

		offset -= i915_mmio_reg_offset(EU_PERF_CNTL0);

		offset >>= 5; /* Flex EU mmio registers are separated by 256
			       * bytes, here they are separated by 8 bytes */

		/* EU_PERF_CNTL0 offset in register state context... */
		offset += ctx_flexeu0;

		reg_state[offset] = i915_mmio_reg_offset(flex_regs[i].addr);
		reg_state[offset+1] = flex_regs[i].value;
	}

	atomic_set(&ctx->engine[engine->id].oa_state_dirty, false);
}

void i915_oa_update_reg_state(struct intel_engine_cs *engine,
			      struct intel_context *ctx,
			      uint32_t *reg_state)
{
	struct drm_i915_private *dev_priv = engine->dev->dev_private;

	if (!dev_priv->perf.initialized)
		return;

	/* XXX: We don't take a lock here and this may run async with
	 * respect to stream methods. Notably we don't want to block
	 * context switches by long i915 perf read() operations.
	 *
	 * It's expect to always be safe to read the dev_priv->perf
	 * state needed here, and expected to be benign to redundantly
	 * update the state if the OA unit has been disabled since
	 * oa_state_dirty was last set.
	 */

	gen8_update_reg_state_unlocked(engine, ctx, reg_state);
}

static ssize_t i915_perf_read_locked(struct i915_perf_stream *stream,
				     struct file *file,
				     char __user *buf,
				     size_t count,
				     loff_t *ppos)
{
	struct i915_perf_read_state state = { count, 0, buf };
	int ret = stream->ops->read(stream, &state);

	/* If we've successfully copied any data then reporting that
	 * takes precedence over any internal error status, so the
	 * data isn't lost
	 */
	return state.read ? state.read : (ret ? ret : -EAGAIN);
}

static ssize_t i915_perf_read(struct file *file,
			      char __user *buf,
			      size_t count,
			      loff_t *ppos)
{
	struct i915_perf_stream *stream = file->private_data;
	struct drm_i915_private *dev_priv = stream->dev_priv;
	ssize_t ret;

	/* To ensure it's handled consistently we simply treat all reads of a
	 * disabled stream as an error. In particular it might otherwise lead
	 * to a deadlock for blocking file descriptors...
	 */
	if (stream->state == I915_PERF_STREAM_DISABLED)
		return -EIO;

	if (!(file->f_flags & O_NONBLOCK)) {
		/* There's the small chance of false positives from
		 * stream->ops->wait_unlocked.
		 *
		 * E.g. with single context filtering since we only wait until
		 * oabuffer has >= 1 report we don't immediately know whether
		 * any reports really belong to the current context
		 */
		do {
			ret = stream->ops->wait_unlocked(stream);
			if (ret)
				return ret;

			mutex_lock(&dev_priv->perf.lock);
			ret = i915_perf_read_locked(stream, file,
						    buf, count, ppos);
			mutex_unlock(&dev_priv->perf.lock);
		} while (ret == -EAGAIN);
	} else {
		mutex_lock(&dev_priv->perf.lock);
		ret = i915_perf_read_locked(stream, file, buf, count, ppos);
		mutex_unlock(&dev_priv->perf.lock);
	}

	return ret;
}

static void wake_up_perf_streams(void *data, async_cookie_t cookie)
{
	struct drm_i915_private *dev_priv = data;
	struct i915_perf_stream *stream;

	mutex_lock(&dev_priv->perf.streams_lock);
	list_for_each_entry(stream, &dev_priv->perf.streams, link) {
		if (stream_have_data__unlocked(stream))
			wake_up(&dev_priv->perf.poll_wq[stream->engine]);
	}
	mutex_unlock(&dev_priv->perf.streams_lock);
}

static enum hrtimer_restart poll_check_timer_cb(struct hrtimer *hrtimer)
{
	struct drm_i915_private *dev_priv =
		container_of(hrtimer, typeof(*dev_priv),
			     perf.poll_check_timer);

	async_schedule(wake_up_perf_streams, dev_priv);

	hrtimer_forward_now(hrtimer, ns_to_ktime(POLL_PERIOD));

	return HRTIMER_RESTART;
}

static unsigned int i915_perf_poll_locked(struct i915_perf_stream *stream,
					  struct file *file,
					  poll_table *wait)
{
	unsigned int streams = 0;

	stream->ops->poll_wait(stream, file, wait);

	if (stream->ops->can_read_unlocked(stream))
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
	if (stream->state != I915_PERF_STREAM_DISABLED)
		return;

	stream->state = I915_PERF_STREAM_ENABLE_IN_PROGRESS;

	if (stream->ops->enable)
		stream->ops->enable(stream);

	stream->state = I915_PERF_STREAM_ENABLED;
}

static void i915_perf_disable_locked(struct i915_perf_stream *stream)
{
	if (stream->state != I915_PERF_STREAM_ENABLED)
		return;

	stream->state = I915_PERF_STREAM_DISABLED;

	if (stream->ops->disable)
		stream->ops->disable(stream);
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

	mutex_lock(&dev_priv->perf.streams_lock);
	list_del(&stream->link);
	mutex_unlock(&dev_priv->perf.streams_lock);

	if (stream->state == I915_PERF_STREAM_ENABLED)
		i915_perf_disable_locked(stream);

	if (stream->ops->destroy)
		stream->ops->destroy(stream);

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
	       struct drm_i915_file_private *file_priv,
	       u32 ctx_user_handle)
{
	struct intel_context *ctx;
	int ret;

	ret = i915_mutex_lock_interruptible(dev_priv->dev);
	if (ret)
		return ERR_PTR(ret);

	ctx = i915_gem_context_get(file_priv, ctx_user_handle);
	if (!IS_ERR(ctx))
		i915_gem_context_reference(ctx);

	mutex_unlock(&dev_priv->dev->struct_mutex);

	return ctx;
}

int i915_perf_open_ioctl_locked(struct drm_device *dev,
				struct drm_i915_perf_open_param *param,
				struct perf_open_properties *props,
				struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_context *specific_ctx = NULL;
	struct i915_perf_stream *stream = NULL;
	unsigned long f_flags = 0;
	int stream_fd;
	int ret = 0;

	if (props->single_context) {
		u32 ctx_handle = props->ctx_handle;
		struct drm_i915_file_private *file_priv = file->driver_priv;

		specific_ctx = lookup_context(dev_priv, file_priv, ctx_handle);
		if (IS_ERR(specific_ctx)) {
			ret = PTR_ERR(specific_ctx);
			if (ret != -EINTR)
				DRM_ERROR("Failed to look up context with ID %u for opening perf stream\n",
					  ctx_handle);
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

	stream->dev_priv = dev_priv;
	stream->ctx = specific_ctx;

	ret = i915_ring_stream_init(stream, param, props);
	if (ret)
		goto err_alloc;

	/* we avoid simply assigning stream->sample_flags = props->sample_flags
	 * to have _stream_init check the combination of sample flags more
	 * thoroughly, but still this is the expected result at this point.
	 */
	BUG_ON(stream->sample_flags != props->sample_flags);

	mutex_lock(&dev_priv->perf.streams_lock);
	list_add(&stream->link, &dev_priv->perf.streams);
	mutex_unlock(&dev_priv->perf.streams_lock);

	if (param->flags & I915_PERF_FLAG_FD_CLOEXEC)
		f_flags |= O_CLOEXEC;
	if (param->flags & I915_PERF_FLAG_FD_NONBLOCK)
		f_flags |= O_NONBLOCK;

	stream_fd = anon_inode_getfd("[i915_perf]", &fops, stream, f_flags);
	if (stream_fd < 0) {
		ret = stream_fd;
		goto err_open;
	}

	if (!(param->flags & I915_PERF_FLAG_DISABLED))
		i915_perf_enable_locked(stream);

	return stream_fd;

err_open:
	mutex_lock(&dev_priv->perf.streams_lock);
	list_del(&stream->link);
	mutex_unlock(&dev_priv->perf.streams_lock);
	if (stream->ops->destroy)
		stream->ops->destroy(stream);
err_alloc:
	kfree(stream);
err_ctx:
	if (specific_ctx) {
		mutex_lock(&dev_priv->dev->struct_mutex);
		i915_gem_context_unreference(specific_ctx);
		mutex_unlock(&dev_priv->dev->struct_mutex);
	}
err:
	return ret;
}

static int check_mmio_whitelist(struct drm_i915_private *dev_priv, u32 num_mmio)
{
#define GEN_RANGE(l, h) GENMASK(h, l)
	static const struct register_whitelist {
		i915_reg_t mmio;
		uint32_t size;
		/* supported gens, 0x10 for 4, 0x30 for 4 and 5, etc. */
		uint32_t gen_bitmask;
	} whitelist[] = {
		{ GEN6_GT_GFX_RC6, 4, GEN_RANGE(7, 9) },
		{ GEN6_GT_GFX_RC6p, 4, GEN_RANGE(7, 9) },
	};
	int i, count;

	for (count = 0; count < num_mmio; count++) {
		/* Coarse check on mmio reg addresses being non zero */
		if (!dev_priv->perf.mmio_list[count])
			return -EINVAL;

		for (i = 0; i < ARRAY_SIZE(whitelist); i++) {
			if (( i915_mmio_reg_offset(whitelist[i].mmio) ==
				dev_priv->perf.mmio_list[count]) &&
			    (1 << INTEL_INFO(dev_priv->dev)->gen &
					whitelist[i].gen_bitmask))
				break;
		}

		if (i == ARRAY_SIZE(whitelist))
			return -EINVAL;
	}
	return 0;
}

static int copy_mmio_list(struct drm_i915_private *dev_priv,
				void __user *mmio)
{
	void __user *mmio_list = ((u8 __user *)mmio + 4);
	u32 num_mmio;
	int ret;

	if (!mmio)
		return -EINVAL;

	ret = get_user(num_mmio, (u32 __user *)mmio);
	if (ret)
		return ret;

	if (num_mmio > I915_PERF_MMIO_NUM_MAX)
		return -EINVAL;

	memset(dev_priv->perf.mmio_list, 0, I915_PERF_MMIO_NUM_MAX);
	if (copy_from_user(dev_priv->perf.mmio_list, mmio_list, 4*num_mmio))
		return -EINVAL;

	ret = check_mmio_whitelist(dev_priv, num_mmio);
	if (ret)
		return ret;

	dev_priv->perf.num_mmio = num_mmio;

	return 0;
}

/* Note we copy the properties from userspace outside of the i915 perf
 * mutex to avoid an awkward lockdep with mmap_sem.
 *
 * Note this function only validates properties in isolation it doesn't
 * validate that the combination of properties makes sense or that all
 * properties necessary for a particular kind of stream have been set.
 */
static int read_properties_unlocked(struct drm_i915_private *dev_priv,
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
		case DRM_I915_PERF_PROP_CTX_HANDLE:
			props->single_context = 1;
			props->ctx_handle = value;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_OA:
			props->sample_flags |= SAMPLE_OA_REPORT;
			break;
		case DRM_I915_PERF_PROP_OA_METRICS_SET:
			if (value == 0 ||
			    value > dev_priv->perf.oa.n_builtin_sets) {
				DRM_ERROR("Unknown OA metric set ID");
				return -EINVAL;
			}
			props->metrics_set = value;
			break;
		case DRM_I915_PERF_PROP_OA_FORMAT:
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
		case DRM_I915_PERF_PROP_OA_EXPONENT:
			if (value > OA_EXPONENT_MAX) {
				DRM_ERROR("OA timer exponent too high (> %u)\n",
					  OA_EXPONENT_MAX);
				return -EINVAL;
			}

			/* Theoretically we can program the OA unit to sample
			 * every 160ns but don't allow that by default unless
			 * root.
			 */
			if (value < i915_oa_min_timer_exponent &&
			    !capable(CAP_SYS_ADMIN)) {
				DRM_ERROR("OA timer exponent too low without root privileges\n");
				return -EACCES;
			}

			props->oa_periodic = true;
			props->oa_period_exponent = value;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_OA_SOURCE:
			props->sample_flags |= SAMPLE_OA_SOURCE_INFO;
			break;
		case DRM_I915_PERF_PROP_ENGINE: {
				unsigned int user_ring_id =
					value & I915_EXEC_RING_MASK;

				if (user_ring_id > I915_USER_RINGS)
					return -EINVAL;

				props->cs_mode = true;
				props->engine = user_ring_map[user_ring_id];
			}
			break;
		case DRM_I915_PERF_PROP_SAMPLE_CTX_ID:
			props->sample_flags |= SAMPLE_CTX_ID;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_PID:
			props->sample_flags |= SAMPLE_PID;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_TAG:
			props->sample_flags |= SAMPLE_TAG;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_TS:
			props->sample_flags |= SAMPLE_TS;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_CLOCK_MONOTONIC_RAW:
			props->sample_flags |= SAMPLE_CLK_MONO_RAW;
			break;
		case DRM_I915_PERF_PROP_SAMPLE_MMIO:
			ret = copy_mmio_list(dev_priv, (u64 __user *)value);
			if (ret)
				return ret;
			props->sample_flags |= SAMPLE_MMIO;
			break;
		case DRM_I915_PERF_PROP_MAX:
			BUG();
		}

		uprop += 2;
	}

	return 0;
}

int i915_perf_open_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_perf_open_param *param = data;
	struct perf_open_properties props;
	u32 known_open_flags = 0;
	int ret;

	if (!dev_priv->perf.initialized) {
		DRM_ERROR("i915 perf interface not available for this system");
		return -ENOTSUPP;
	}

	known_open_flags = I915_PERF_FLAG_FD_CLOEXEC |
			   I915_PERF_FLAG_FD_NONBLOCK |
			   I915_PERF_FLAG_DISABLED;
	if (param->flags & ~known_open_flags) {
		DRM_ERROR("Unknown drm_i915_perf_open_param flag\n");
		return -EINVAL;
	}

	ret = read_properties_unlocked(dev_priv,
				       to_user_ptr(param->properties_ptr),
				       param->num_properties,
				       &props);
	if (ret)
		return ret;

	mutex_lock(&dev_priv->perf.lock);
	ret = i915_perf_open_ioctl_locked(dev, param, &props, file);
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
	{
	 .procname = "oa_min_timer_exponent",
	 .data = &i915_oa_min_timer_exponent,
	 .maxlen = sizeof(i915_oa_min_timer_exponent),
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

void i915_perf_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i915_clock_info *clk_info = &dev_priv->ts_clk_info;
	int i;

	if (!HAS_OA_PERF(dev))
		return;

	dev_priv->perf.metrics_kobj =
		kobject_create_and_add("metrics", &dev->primary->kdev->kobj);
	if (!dev_priv->perf.metrics_kobj)
		return;

	clk_info->timestamp_frequency = GT_CS_TIMESTAMP_FREQUENCY(dev_priv);
	clk_info->resync_period = INIT_CLK_SYNC_PERIOD;
	INIT_DELAYED_WORK(&clk_info->clk_sync_work, i915_perf_clock_sync_work);

	i915_tstamp_clk_init_base_freq(dev_priv, &clk_info->cc);

	hrtimer_init(&dev_priv->perf.poll_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_priv->perf.poll_check_timer.function = poll_check_timer_cb;

	for (i = 0; i < I915_NUM_ENGINES; i++) {
		INIT_LIST_HEAD(&dev_priv->perf.node_list[i]);
		spin_lock_init(&dev_priv->perf.node_list_lock[i]);
		init_waitqueue_head(&dev_priv->perf.poll_wq[i]);
	}

	INIT_LIST_HEAD(&dev_priv->perf.streams);
	mutex_init(&dev_priv->perf.lock);
	mutex_init(&dev_priv->perf.streams_lock);
	spin_lock_init(&dev_priv->perf.hook_lock);

	if (IS_HASWELL(dev)) {
		dev_priv->perf.oa.ops.init_oa_buffer = gen7_init_oa_buffer;
		dev_priv->perf.oa.ops.enable_metric_set = hsw_enable_metric_set;
		dev_priv->perf.oa.ops.disable_metric_set = hsw_disable_metric_set;
		dev_priv->perf.oa.ops.oa_enable = gen7_oa_enable;
		dev_priv->perf.oa.ops.oa_disable = gen7_oa_disable;
		dev_priv->perf.oa.ops.update_hw_ctx_id_locked =
			gen7_update_hw_ctx_id_locked;
		dev_priv->perf.oa.ops.read = gen7_oa_read;
		dev_priv->perf.oa.ops.oa_buffer_num_samples =
			gen7_oa_buffer_num_samples_fop_unlocked;
		dev_priv->perf.oa.ops.oa_buffer_get_ctx_id =
					gen7_oa_buffer_get_ctx_id;

		dev_priv->perf.oa.oa_formats = hsw_oa_formats;

		dev_priv->perf.oa.n_builtin_sets =
			i915_oa_n_builtin_metric_sets_hsw;

		if (i915_perf_init_sysfs_hsw(dev_priv))
			goto sysfs_error;
	} else {
		dev_priv->perf.oa.ops.init_oa_buffer = gen8_init_oa_buffer;
		dev_priv->perf.oa.ops.oa_enable = gen8_oa_enable;
		dev_priv->perf.oa.ops.oa_disable = gen8_oa_disable;
		dev_priv->perf.oa.ops.read = gen8_oa_read;
		dev_priv->perf.oa.ops.oa_buffer_num_samples =
				gen8_oa_buffer_num_samples_fop_unlocked;
		dev_priv->perf.oa.ops.oa_buffer_get_ctx_id =
					gen8_oa_buffer_get_ctx_id;

		dev_priv->perf.oa.oa_formats = gen8_plus_oa_formats;

		if (!i915.enable_execlists) {
			dev_priv->perf.oa.ops.legacy_ctx_switch_unlocked =
				gen8_legacy_ctx_switch_unlocked;
		}

		if (IS_BROADWELL(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				bdw_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				bdw_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x120;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x2ce;
			dev_priv->perf.oa.ctx_valid_off = 25;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_bdw;

			if (i915_perf_init_sysfs_bdw(dev_priv))
				goto sysfs_error;
		} else if (IS_CHERRYVIEW(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				chv_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				chv_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x120;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x2ce;
			dev_priv->perf.oa.ctx_valid_off = 25;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_chv;

			if (i915_perf_init_sysfs_chv(dev_priv))
				goto sysfs_error;
		} else if (IS_SKYLAKE(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				skl_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				skl_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x128;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x3de;
			dev_priv->perf.oa.ctx_valid_off = 16;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_skl;

			if (i915_perf_init_sysfs_skl(dev_priv))
				goto sysfs_error;
		} else if (IS_BROXTON(dev)) {
			dev_priv->perf.oa.ops.enable_metric_set =
				bxt_enable_metric_set;
			dev_priv->perf.oa.ops.disable_metric_set =
				bxt_disable_metric_set;
			dev_priv->perf.oa.ctx_oactxctrl_off = 0x128;
			dev_priv->perf.oa.ctx_flexeu0_off = 0x3de;
			dev_priv->perf.oa.ctx_valid_off = 16;
			dev_priv->perf.oa.n_builtin_sets =
				i915_oa_n_builtin_metric_sets_bxt;

			if (i915_perf_init_sysfs_bxt(dev_priv))
				goto sysfs_error;
		}
	}

	dev_priv->perf.sysctl_header = register_sysctl_table(dev_root);

	dev_priv->perf.initialized = true;

	return;

sysfs_error:
	kobject_put(dev_priv->perf.metrics_kobj);
	dev_priv->perf.metrics_kobj = NULL;

	return;
}

void i915_perf_init_late(struct drm_i915_private *dev_priv)
{
	struct i915_clock_info *clk_info = &dev_priv->ts_clk_info;

	timecounter_init(&clk_info->tc,	&clk_info->cc,
				ktime_to_ns(ktime_get_boottime()));
}

void i915_perf_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!dev_priv->perf.initialized)
		return;

	unregister_sysctl_table(dev_priv->perf.sysctl_header);

        if (IS_HASWELL(dev))
                i915_perf_deinit_sysfs_hsw(dev_priv);
        else if (IS_BROADWELL(dev))
                i915_perf_deinit_sysfs_bdw(dev_priv);
        else if (IS_CHERRYVIEW(dev))
                i915_perf_deinit_sysfs_chv(dev_priv);
        else if (IS_SKYLAKE(dev))
                i915_perf_deinit_sysfs_skl(dev_priv);
	else if (IS_BROXTON(dev))
		i915_perf_deinit_sysfs_bxt(dev_priv);

	kobject_put(dev_priv->perf.metrics_kobj);
	dev_priv->perf.metrics_kobj = NULL;

	dev_priv->perf.oa.ops.init_oa_buffer = NULL;

	dev_priv->perf.initialized = false;
}
