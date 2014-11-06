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
		/* TODO: Init according to specific type */
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

	/* Currently no global event state to initialize */

	dev_priv->perf.initialized = true;
}

void i915_perf_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (!dev_priv->perf.initialized)
		return;

	/* Currently nothing to clean up */

	dev_priv->perf.initialized = false;
}
