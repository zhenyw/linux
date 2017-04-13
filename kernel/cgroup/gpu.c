
#include <linux/slab.h>
#include <linux/cgroup.h>
#include <linux/cgroup_gpu.h>

struct gpu_cgroup {
	struct cgroup_subsys_state css;
	u64 prio;
};

static DEFINE_MUTEX(gpucg_mutex);

#define GPU_PRIO_MAX 1024 /* XXX hack for i915 */

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

static u64 gpu_css_get_prio(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	return cg->prio;
}

static int gpu_css_set_prio(struct cgroup_subsys_state *css, struct cftype *cft,
			    u64 val)
{
	struct gpu_cgroup *cg = css_gpucg(css);

	if (val > GPU_PRIO_MAX)
		return -EINVAL;
	
	mutex_lock(&gpucg_mutex);
	cg->prio = val;
	mutex_unlock(&gpucg_mutex);
	return 0;
}

static struct cftype gpu_css_files[] = {
	{
		.name = "priority",
		.write_u64 = gpu_css_set_prio,
		.read_u64 = gpu_css_get_prio,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
};

u64 gpucg_get_priority(struct task_struct *task)
{
	struct gpu_cgroup *cg = get_task_gpucg(task);

	BUG_ON(!cg);
	return cg->prio;
}
EXPORT_SYMBOL(gpucg_get_priority);

static struct cgroup_subsys_state *
gpu_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct gpu_cgroup *cg;
	
	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	printk("zhen: new gpu cgroup %p\n", cg);
	
	return &cg->css;
}

static void gpu_css_free(struct cgroup_subsys_state *css)
{
	struct gpu_cgroup *cg = css_gpucg(css);
	printk("zhen: free gpu cgroup %p\n", cg);

	kfree(cg);

}


struct cgroup_subsys gpu_cgrp_subsys = {
	.css_alloc = gpu_css_alloc,
	.css_free = gpu_css_free,
	.legacy_cftypes = gpu_css_files,
	.dfl_cftypes = gpu_css_files,
};
