/*
 * Copyright (C) 2017 Zhenyu Wang <zhenyuw@linux.intel.com>
 *
 * This file is subject to the terms and conditions of version 2 of the GNU
 * General Public License. See the file COPYING in the main directory of the
 * Linux distribution for more details.
 */

#ifndef CGROUP_GPU_H
#define CGROUP_GPU_H

extern s64 gpucg_get_priority(struct task_struct *task);
extern u64 gpucg_get_max_mem(struct task_struct *task);
extern u64 gpucg_get_cur_mem(struct task_struct *task);
extern void gpucg_adjust_mem(struct task_struct *task, u64 size, bool);

#endif
