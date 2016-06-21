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

#define SAMPLE_OA_REPORT      (1<<0)

struct perf_open_properties {
	u32 sample_flags;

	u64 single_context:1;
	u64 ctx_handle;

	/* OA sampling state */
	int metrics_set;
	int oa_format;
	bool oa_periodic;
	int oa_period_exponent;
};

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
static bool gen8_oa_buffer_is_empty_fop_unlocked(struct drm_i915_private *dev_priv)
{
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u32 head = I915_READ(GEN8_OAHEADPTR);
	u32 tail = I915_READ(GEN8_OATAILPTR);

	return OA_TAKEN(tail, head) < report_size;
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
static bool gen7_oa_buffer_is_empty_fop_unlocked(struct drm_i915_private *dev_priv)
{
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u32 oastatus2 = I915_READ(GEN7_OASTATUS2);
	u32 oastatus1 = I915_READ(GEN7_OASTATUS1);
	u32 head = oastatus2 & GEN7_OASTATUS2_HEAD_MASK;
	u32 tail = oastatus1 & GEN7_OASTATUS1_TAIL_MASK;

	return OA_TAKEN(tail, head) <
		dev_priv->perf.oa.tail_margin + report_size;
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
 * Copies single OA report into userspace read() buffer.
 */
static int append_oa_sample(struct i915_perf_stream *stream,
			    struct i915_perf_read_state *read_state,
			    const u8 *report)
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

	if (sample_flags & SAMPLE_OA_REPORT) {
		if (copy_to_user(buf, report, report_size))
			return -EFAULT;
	}

	read_state->buf += header.size;
	read_state->read += header.size;

	return 0;
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
				  u32 tail)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u32 head;
	u32 taken;
	int ret = 0;

	BUG_ON(!stream->enabled);

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
	     (taken = OA_TAKEN(tail, head));
	     head = (head + report_size) & mask) {
		u8 *report = oa_buf_base + head;
		u32 *report32 = (void *)report;
		u32 ctx_id = report32[2];
		u32 report_id = report32[0];

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
		if (!dev_priv->perf.oa.exclusive_stream->ctx ||
		    dev_priv->perf.oa.specific_ctx_id == ctx_id ||
		    dev_priv->perf.oa.oa_buffer.last_ctx_id == ctx_id) {

			/* Note: we don't check the reason field to
			 * recognise context-switch reports because
			 * it's possible that the first report after a
			 * context switch is in fact periodic. We mark
			 * the switch-away reports with an invalid
			 * context id to be recognisable by userspace.
			 */
			if (dev_priv->perf.oa.exclusive_stream->ctx &&
			    dev_priv->perf.oa.specific_ctx_id != ctx_id)
				report32[2] = 0x1fffff;

			ret = append_oa_sample(stream, read_state, report);
			if (ret)
				break;

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
			struct i915_perf_read_state *read_state)
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

	ret = gen8_append_oa_reports(stream, read_state, &head, tail);

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
				  u32 tail)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int report_size = dev_priv->perf.oa.oa_buffer.format_size;
	u8 *oa_buf_base = dev_priv->perf.oa.oa_buffer.addr;
	int tail_margin = dev_priv->perf.oa.tail_margin;
	u32 mask = (OA_BUFFER_SIZE - 1);
	u32 head;
	u32 taken;
	int ret = 0;

	BUG_ON(!stream->enabled);

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
	     (taken = OA_TAKEN(tail, head));
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

		ret = append_oa_sample(stream, read_state, report);
		if (ret)
			break;

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
			struct i915_perf_read_state *read_state)
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

	ret = gen7_append_oa_reports(stream, read_state, &head, tail);

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

static bool i915_oa_can_read(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	return !dev_priv->perf.oa.ops.oa_buffer_is_empty(dev_priv);
}

static int i915_oa_wait_unlocked(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	/* We would wait indefinitly if periodic sampling is not enabled */
	if (!dev_priv->perf.oa.periodic)
		return -EIO;

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

static int i915_oa_read(struct i915_perf_stream *stream,
			struct i915_perf_read_state *read_state)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	return dev_priv->perf.oa.ops.read(stream, read_state);
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

static int alloc_oa_buffer(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *bo;
	int ret;

	BUG_ON(dev_priv->perf.oa.oa_buffer.obj);

	ret = i915_mutex_lock_interruptible(dev_priv->dev);
	if (ret)
		return ret;

	bo = i915_gem_object_create(dev_priv->dev, OA_BUFFER_SIZE);
	if (IS_ERR(bo)) {
		DRM_ERROR("Failed to allocate OA buffer\n");
		ret = PTR_ERR(bo);
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
	dev_priv->perf.oa.oa_buffer.addr = i915_gem_object_pin_map(bo);
	if (dev_priv->perf.oa.oa_buffer.addr == NULL)
		goto err_unpin;

	dev_priv->perf.oa.ops.init_oa_buffer(dev_priv);

	DRM_DEBUG_DRIVER("OA Buffer initialized, gtt offset = 0x%x, vaddr = %p",
			 dev_priv->perf.oa.oa_buffer.gtt_offset,
			 dev_priv->perf.oa.oa_buffer.addr);

	goto unlock;

err_unpin:
	i915_gem_object_ggtt_unpin(bo);

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

	if (dev_priv->perf.oa.exclusive_stream->enabled) {
		unsigned long ctx_id = 0;

		if (dev_priv->perf.oa.exclusive_stream->ctx)
			ctx_id = dev_priv->perf.oa.specific_ctx_id;

		if (dev_priv->perf.oa.exclusive_stream->ctx == NULL || ctx_id) {
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

static void gen8_oa_disable(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN8_OACONTROL, 0);
}

static void i915_oa_stream_disable(struct i915_perf_stream *stream)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;

	dev_priv->perf.oa.ops.oa_disable(dev_priv);

	if (dev_priv->perf.oa.periodic)
		hrtimer_cancel(&dev_priv->perf.oa.poll_check_timer);
}

static u64 oa_exponent_to_ns(struct drm_i915_private *dev_priv, int exponent)
{
	return 1000000000ULL * (2ULL << exponent) /
		dev_priv->perf.oa.timestamp_frequency;
}

static const struct i915_perf_stream_ops i915_oa_stream_ops = {
	.destroy = i915_oa_stream_destroy,
	.enable = i915_oa_stream_enable,
	.disable = i915_oa_stream_disable,
	.can_read = i915_oa_can_read,
	.wait_unlocked = i915_oa_wait_unlocked,
	.poll_wait = i915_oa_poll_wait,
	.read = i915_oa_read,
};

static int i915_oa_stream_init(struct i915_perf_stream *stream,
			       struct drm_i915_perf_open_param *param,
			       struct perf_open_properties *props)
{
	struct drm_i915_private *dev_priv = stream->dev_priv;
	int format_size;
	int ret;

	if (!(props->sample_flags & SAMPLE_OA_REPORT)) {
		DRM_ERROR("Only OA report sampling supported\n");
		return -EINVAL;
	}

	if (!dev_priv->perf.oa.ops.init_oa_buffer) {
		DRM_ERROR("OA unit not supported\n");
		return -ENODEV;
	}

	/* To avoid the complexity of having to accurately filter
	 * counter reports and marshal to the appropriate client
	 * we currently only allow exclusive access
	 */
	if (dev_priv->perf.oa.exclusive_stream) {
		DRM_ERROR("OA unit already in use\n");
		return -EBUSY;
	}

	if (!props->metrics_set) {
		DRM_ERROR("OA metric set not specified\n");
		return -EINVAL;
	}

	if (!props->oa_format) {
		DRM_ERROR("OA report format not specified\n");
		return -EINVAL;
	}

	stream->sample_size = sizeof(struct drm_i915_perf_record_header);

	format_size = dev_priv->perf.oa.oa_formats[props->oa_format].size;

	stream->sample_flags |= SAMPLE_OA_REPORT;
	stream->sample_size += format_size;

	dev_priv->perf.oa.oa_buffer.format_size = format_size;
	BUG_ON(dev_priv->perf.oa.oa_buffer.format_size == 0);

	dev_priv->perf.oa.oa_buffer.format =
		dev_priv->perf.oa.oa_formats[props->oa_format].format;

	dev_priv->perf.oa.metrics_set = props->metrics_set;

	dev_priv->perf.oa.periodic = props->oa_periodic;
	if (dev_priv->perf.oa.periodic) {
		u64 period_ns = oa_exponent_to_ns(dev_priv,
						  props->oa_period_exponent);

		dev_priv->perf.oa.period_exponent = props->oa_period_exponent;

		/* See comment for OA_TAIL_MARGIN_NSEC for details
		 * about this tail_margin...
		 */
		dev_priv->perf.oa.tail_margin =
			((OA_TAIL_MARGIN_NSEC / period_ns) + 1) * format_size;
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

	stream->ops = &i915_oa_stream_ops;

	/* On Haswell we have to track which OASTATUS1 flags we've already
	 * seen since they can't be cleared while periodic sampling is enabled.
	 */
	dev_priv->perf.oa.gen7_latched_oastatus1 = 0;

	dev_priv->perf.oa.exclusive_stream = stream;

	return 0;
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

	if (dev_priv->perf.oa.exclusive_stream &&
	    dev_priv->perf.oa.exclusive_stream->ctx == context) {
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

	if (dev_priv->perf.oa.exclusive_stream &&
	    dev_priv->perf.oa.exclusive_stream->enabled) {

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
	if (!stream->enabled)
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

static enum hrtimer_restart oa_poll_check_timer_cb(struct hrtimer *hrtimer)
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

	stream->ops->poll_wait(stream, file, wait);

	if (stream->ops->can_read(stream))
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

	/* Allow stream->ops->enable() to refer to this */
	stream->enabled = true;

	if (stream->ops->enable)
		stream->ops->enable(stream);
}

static void i915_perf_disable_locked(struct i915_perf_stream *stream)
{
	if (!stream->enabled)
		return;

	/* Allow stream->ops->disable() to refer to this */
	stream->enabled = false;

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

	if (stream->enabled)
		i915_perf_disable_locked(stream);

	if (stream->ops->destroy)
		stream->ops->destroy(stream);

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

	ret = i915_oa_stream_init(stream, param, props);
	if (ret)
		goto err_alloc;

	/* we avoid simply assigning stream->sample_flags = props->sample_flags
	 * to have _stream_init check the combination of sample flags more
	 * thoroughly, but still this is the expected result at this point.
	 */
	BUG_ON(stream->sample_flags != props->sample_flags);

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

	if (!(param->flags & I915_PERF_FLAG_DISABLED))
		i915_perf_enable_locked(stream);

	return stream_fd;

err_open:
	list_del(&stream->link);
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

	if (!HAS_OA_PERF(dev))
		return;

	dev_priv->perf.metrics_kobj =
		kobject_create_and_add("metrics", &dev->primary->kdev->kobj);
	if (!dev_priv->perf.metrics_kobj)
		return;

	hrtimer_init(&dev_priv->perf.oa.poll_check_timer,
		     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev_priv->perf.oa.poll_check_timer.function = oa_poll_check_timer_cb;
	init_waitqueue_head(&dev_priv->perf.oa.poll_wq);

	INIT_LIST_HEAD(&dev_priv->perf.streams);
	mutex_init(&dev_priv->perf.lock);
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
		dev_priv->perf.oa.ops.oa_buffer_is_empty =
			gen7_oa_buffer_is_empty_fop_unlocked;

		dev_priv->perf.oa.timestamp_frequency = 12500000;

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
		dev_priv->perf.oa.ops.oa_buffer_is_empty =
			gen8_oa_buffer_is_empty_fop_unlocked;

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

			dev_priv->perf.oa.timestamp_frequency = 12500000;

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

			dev_priv->perf.oa.timestamp_frequency = 12500000;

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

			dev_priv->perf.oa.timestamp_frequency = 12000000;

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
