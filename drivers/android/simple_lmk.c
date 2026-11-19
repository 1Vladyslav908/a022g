// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/ratelimit.h>
#include <linux/sched/mm.h>
#include <linux/sort.h>
#include <uapi/linux/sched/types.h>

/* The minimum number of pages to free per reclaim */
#define MIN_FREE_PAGES (CONFIG_ANDROID_SIMPLE_LMK_MINFREE * SZ_1M / PAGE_SIZE)

/* Kill up to this many victims per reclaim */
#define MAX_VICTIMS 1024

/* Timeout in jiffies for each reclaim */
#define RECLAIM_EXPIRES msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC)

/* Protect foreground, visible, and other important processes */
#define LMK_PROTECT_ADJ 200

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;
};

static struct victim_info victims[MAX_VICTIMS] __cacheline_aligned_in_smp;
static struct task_struct *task_bucket[SHRT_MAX + 1] __cacheline_aligned;
static DECLARE_WAIT_QUEUE_HEAD(oom_waitq);
static DECLARE_WAIT_QUEUE_HEAD(reaper_waitq);
static DECLARE_COMPLETION(reclaim_done);
static __cacheline_aligned_in_smp DEFINE_RWLOCK(mm_free_lock);
static int nr_victims;
static bool reclaim_active;
static atomic_t needs_reclaim = ATOMIC_INIT(0);
static atomic_t needs_reap = ATOMIC_INIT(0);
static atomic_t nr_killed = ATOMIC_INIT(0);

static int victim_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = lhs_ptr;
	const struct victim_info *rhs = rhs_ptr;

	if (lhs->size < rhs->size)
		return 1;
	if (lhs->size > rhs->size)
		return -1;
	return 0;
}

static void victim_swap(void *lhs_ptr, void *rhs_ptr, int size)
{
	struct victim_info *lhs = lhs_ptr;
	struct victim_info *rhs = rhs_ptr;

	swap(*lhs, *rhs);
}

static unsigned long get_total_mm_pages(struct mm_struct *mm)
{
	unsigned long pages = 0;
	int i;

	for (i = 0; i < NR_MM_COUNTERS; i++)
		pages += get_mm_counter(mm, i);

	return pages;
}

static unsigned long find_victims(int *vindex)
{
	short i, min_adj = SHRT_MAX, max_adj = 0;
	unsigned long pages_found = 0;
	struct task_struct *tsk;

	rcu_read_lock();
	for_each_process(tsk) {
		struct signal_struct *sig;
		short adj;

		sig = tsk->signal;
		adj = READ_ONCE(sig->oom_score_adj);
		if (adj < LMK_PROTECT_ADJ ||
		    sig->flags & (SIGNAL_GROUP_EXIT | SIGNAL_GROUP_COREDUMP) ||
		    (thread_group_empty(tsk) && tsk->flags & PF_EXITING))
			continue;

		tsk->simple_lmk_next = task_bucket[adj];
		task_bucket[adj] = tsk;

		if (adj > max_adj)
			max_adj = adj;
		if (adj < min_adj)
			min_adj = adj;
	}

	for (i = max_adj; i >= min_adj; i--) {
		int old_vindex;

		tsk = task_bucket[i];
		if (!tsk)
			continue;

		task_bucket[i] = NULL;

		old_vindex = *vindex;
		do {
			struct task_struct *vtsk;

			vtsk = find_lock_task_mm(tsk);
			if (!vtsk)
				continue;

			victims[*vindex].tsk = vtsk;
			victims[*vindex].mm = vtsk->mm;
			victims[*vindex].size = get_total_mm_pages(vtsk->mm);

			pages_found += victims[*vindex].size;

			if (++*vindex == MAX_VICTIMS)
				break;
		} while ((tsk = tsk->simple_lmk_next));

		if (*vindex == old_vindex)
			continue;

		sort(&victims[old_vindex], *vindex - old_vindex,
		     sizeof(*victims), victim_cmp, victim_swap);

		if (*vindex == MAX_VICTIMS || pages_found >= MIN_FREE_PAGES) {
			if (i > min_adj)
				memset(&task_bucket[min_adj], 0,
				       (i - min_adj) * sizeof(*task_bucket));
			break;
		}
	}
	rcu_read_unlock();

	return pages_found;
}

static int process_victims(int vlen)
{
	unsigned long pages_found = 0;
	int i, nr_to_kill = 0;

	for (i = 0; i < vlen; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		if (pages_found >= MIN_FREE_PAGES) {
			task_unlock(vtsk);
		} else {
			pages_found += victim->size;
			nr_to_kill++;
		}
	}

	return nr_to_kill;
}

static void set_task_rt_prio(struct task_struct *tsk, int priority)
{
	const struct sched_param rt_prio = {
		.sched_priority = priority
	};

	sched_setscheduler_nocheck(tsk, SCHED_RR, &rt_prio);
}

static void scan_and_kill(void)
{
	int i, nr_to_kill, nr_found = 0;
	unsigned long pages_found;

	write_lock(&mm_free_lock);
	nr_victims = 0;
	write_unlock(&mm_free_lock);

	pages_found = find_victims(&nr_found);
	if (unlikely(!nr_found)) {
		pr_err_ratelimited("No processes available to kill!\n");
		return;
	}

	if (pages_found > MIN_FREE_PAGES) {
		nr_to_kill = process_victims(nr_found);

		sort(victims, nr_to_kill, sizeof(*victims), victim_cmp,
		     victim_swap);

		nr_to_kill = process_victims(nr_to_kill);
	} else {
		nr_to_kill = nr_found;
	}

	write_lock(&mm_free_lock);
	nr_victims = nr_to_kill;
	reclaim_active = true;
	write_unlock(&mm_free_lock);

	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *t, *vtsk = victim->tsk;
		struct mm_struct *mm = victim->mm;

		pr_info("Killing %s with adj %d to free %lu KiB\n", vtsk->comm,
			vtsk->signal->oom_score_adj,
			victim->size << (PAGE_SHIFT - 10));

		set_bit(MMF_OOM_VICTIM, &mm->flags);

		do_send_sig_info(SIGKILL, SEND_SIG_FORCED, vtsk, true);

		rcu_read_lock();
		for_each_thread(vtsk, t)
			set_tsk_thread_flag(t, TIF_MEMDIE);
		for_each_thread(vtsk, t)
			set_task_rt_prio(t, 1);
		rcu_read_unlock();

		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		__thaw_task(vtsk);

		victim->size = get_mm_counter(mm, MM_ANONPAGES);

		task_unlock(vtsk);
	}

	write_lock(&mm_free_lock);
	sort(victims, nr_to_kill, sizeof(*victims), victim_cmp, victim_swap);
	atomic_set(&needs_reap, 1);
	write_unlock(&mm_free_lock);
	if (waitqueue_active(&reaper_waitq))
		wake_up(&reaper_waitq);

	if (!wait_for_completion_timeout(&reclaim_done, RECLAIM_EXPIRES))
		pr_info("Timeout hit waiting for victims to die, proceeding\n");

	write_lock(&mm_free_lock);
	reinit_completion(&reclaim_done);
	reclaim_active = false;
	nr_killed = (atomic_t)ATOMIC_INIT(0);
	write_unlock(&mm_free_lock);
}

static int simple_lmk_reclaim_thread(void *data)
{
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		wait_event_freezable(oom_waitq, atomic_read(&needs_reclaim));
		atomic_set(&needs_reclaim, 0);
		scan_and_kill();
	}

	return 0;
}

static struct mm_struct *next_reap_victim(void)
{
	struct mm_struct *mm = NULL;
	bool should_retry = false;
	int i;

	write_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++, mm = NULL) {
		mm = victims[i].mm;
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		if (!down_read_trylock(&mm->mmap_sem)) {
			should_retry = true;
			continue;
		}

		if (mm_has_notifiers(mm)) {
			up_read(&mm->mmap_sem);
			should_retry = true;
			continue;
		}

		if (!test_bit(MMF_OOM_SKIP, &mm->flags))
			break;
		up_read(&mm->mmap_sem);
	}

	if (!mm) {
		if (should_retry)
			mm = ERR_PTR(-EAGAIN);
		else if (!reclaim_active)
			nr_victims = 0;
	}
	write_unlock(&mm_free_lock);

	return mm;
}

static void reap_victims(void)
{
	struct mm_struct *mm;

	while ((mm = next_reap_victim())) {
		if (IS_ERR(mm)) {
			schedule_timeout_uninterruptible(1);
			continue;
		}

		__oom_reap_task_mm(mm);
		clear_bit(MMF_OOM_VICTIM, &mm->flags);
		set_bit(MMF_OOM_SKIP, &mm->flags);
		up_read(&mm->mmap_sem);
	}
}

static int simple_lmk_reaper_thread(void *data)
{
	set_task_rt_prio(current, MAX_RT_PRIO - 2);
	set_freezable();

	while (1) {
		wait_event_freezable(reaper_waitq,
				     atomic_cmpxchg_relaxed(&needs_reap, 1, 0));
		reap_victims();
	}

	return 0;
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	int i;

	if (!test_bit(MMF_OOM_SKIP, &mm->flags))
		return;

	read_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++) {
		if (victims[i].mm == mm) {
			victims[i].mm = NULL;
			if (reclaim_active &&
			    atomic_inc_return_relaxed(&nr_killed) == nr_victims)
				complete(&reclaim_done);
			break;
		}
	}
	read_unlock(&mm_free_lock);
}

void simple_lmk_start_reclaim(void)
{
	if (atomic_cmpxchg(&needs_reclaim, 0, 1))
		return;

	smp_mb__after_atomic();
	if (waitqueue_active(&oom_waitq))
		wake_up(&oom_waitq);
}

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);
	struct task_struct *thread;

	if (!atomic_cmpxchg(&init_done, 0, 1)) {
		thread = kthread_run(simple_lmk_reaper_thread, NULL,
				     "simple_lmkd_reaper");
		BUG_ON(IS_ERR(thread));
		thread = kthread_run(simple_lmk_reclaim_thread, NULL,
				     "simple_lmkd");
		BUG_ON(IS_ERR(thread));
	}

	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);

static int __init simple_lmk_late_init(void)
{
	simple_lmk_init_set(NULL, NULL);
	return 0;
}
late_initcall(simple_lmk_late_init);
