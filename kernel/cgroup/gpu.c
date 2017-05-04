/*
 * GPU cgroup
 *
 * GPU resource control based on cgroup hierarchy
 *
 * Copyright (C) 2017 Zhenyu Wang <zhenyuw@linux.intel.com>
 *
 * This file is subject to the terms and conditions of version 2 of the GNU
 * General Public License. See the file COPYING in the main directory of the
 * Linux distribution for more details.
 */

#include <linux/slab.h>
#include <linux/cgroup.h>
#include <linux/cgroup_gpu.h>

struct gpu_cgroup {
	struct cgroup_subsys_state css;
	s64 prio;
	u64 max_mem_in_bytes;
};

static DEFINE_MUTEX(gpucg_mutex);

#define GPU_PRIO_MAX 1024 /* align with i915 now */

static struct gpu_cgroup *css_gpucg(struct cgroup_subsys_state *css)
{
	return container_of(css, struct gpu_cgroup, css);
}

static inline struct gpu_cgroup *get_current_gpucg(void)
{
	return css_gpucg(task_get_css(current, gpu_cgrp_id));
}

static inline struct gpu_cgroup *get_task_gpucg(struct task_struct *task)
{
	return css_gpucg(task_get_css(task, gpu_cgrp_id));
}

static s64 gpu_css_get_prio(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	return cg->prio;
}

static int gpu_css_set_prio(struct cgroup_subsys_state *css, struct cftype *cft,
			    s64 val)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	if (val > GPU_PRIO_MAX || val < -GPU_PRIO_MAX)
		return -EINVAL;
	
	mutex_lock(&gpucg_mutex);
	cg->prio = val;
	mutex_unlock(&gpucg_mutex);
	return 0;
}

static u64 gpu_css_get_max_mem(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	return cg->max_mem_in_bytes;
}

static int gpu_css_set_max_mem(struct cgroup_subsys_state *css, struct cftype *cft,
			       u64 val)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	mutex_lock(&gpucg_mutex);
	cg->max_mem_in_bytes = val;
	mutex_unlock(&gpucg_mutex);
	return 0;
}

static struct cftype gpu_css_files[] = {
	{
		.name = "priority",
		.write_s64 = gpu_css_set_prio,
		.read_s64 = gpu_css_get_prio,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "max_mem_in_bytes",
		.write_u64 = gpu_css_set_max_mem,
		.read_u64 = gpu_css_get_max_mem,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
};

s64 gpucg_get_priority(struct task_struct *task)
{
	struct gpu_cgroup *cg = get_task_gpucg(task);

	BUG_ON(!cg);
	return cg->prio;
}
EXPORT_SYMBOL(gpucg_get_priority);

u64 gpucg_get_max_mem(struct task_struct *task)
{
	struct gpu_cgroup *cg = get_task_gpucg(task);
	BUG_ON(!cg);
	return cg->max_mem_in_bytes;
}
EXPORT_SYMBOL(gpucg_get_max_mem);

static struct cgroup_subsys_state *
gpu_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct gpu_cgroup *cg;
	
	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	cg->prio = 0;
	cg->max_mem_in_bytes = ULONG_MAX;
	return &cg->css;
}

static void gpu_css_free(struct cgroup_subsys_state *css)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	kfree(cg);
}


struct cgroup_subsys gpu_cgrp_subsys = {
	.css_alloc = gpu_css_alloc,
	.css_free = gpu_css_free,
	.legacy_cftypes = gpu_css_files,
	.dfl_cftypes = gpu_css_files,
};
