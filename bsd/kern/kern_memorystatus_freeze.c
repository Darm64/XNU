/*
 * Copyright (c) 2006-2018 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 */

#include <kern/sched_prim.h>
#include <kern/kalloc.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/locks.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/host.h>
#include <kern/policy_internal.h>
#include <kern/thread_group.h>

#include <IOKit/IOBSD.h>

#include <libkern/libkern.h>
#include <mach/coalition.h>
#include <mach/mach_time.h>
#include <mach/task.h>
#include <mach/host_priv.h>
#include <mach/mach_host.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <sys/coalition.h>
#include <sys/kern_event.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/reason.h>
#include <sys/signal.h>
#include <sys/signalvar.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/wait.h>
#include <sys/tree.h>
#include <sys/priv.h>
#include <vm/vm_pageout.h>
#include <vm/vm_protos.h>
#include <mach/machine/sdt.h>
#include <libkern/section_keywords.h>
#include <stdatomic.h>

#if CONFIG_FREEZE
#include <vm/vm_map.h>
#endif /* CONFIG_FREEZE */

#include <sys/kern_memorystatus.h>
#include <sys/kern_memorystatus_freeze.h>
#include <sys/kern_memorystatus_notify.h>

#if CONFIG_JETSAM

extern unsigned int memorystatus_available_pages;
extern unsigned int memorystatus_available_pages_pressure;
extern unsigned int memorystatus_available_pages_critical;
extern unsigned int memorystatus_available_pages_critical_base;
extern unsigned int memorystatus_available_pages_critical_idle_offset;

#else /* CONFIG_JETSAM */

extern uint64_t memorystatus_available_pages;
extern uint64_t memorystatus_available_pages_pressure;
extern uint64_t memorystatus_available_pages_critical;

#endif /* CONFIG_JETSAM */

unsigned int memorystatus_frozen_count = 0;
unsigned int memorystatus_suspended_count = 0;
unsigned long freeze_threshold_percentage = 50;

#if CONFIG_FREEZE

lck_grp_attr_t *freezer_lck_grp_attr;
lck_grp_t *freezer_lck_grp;
static lck_mtx_t freezer_mutex;

/* Thresholds */
unsigned int memorystatus_freeze_threshold = 0;
unsigned int memorystatus_freeze_pages_min = 0;
unsigned int memorystatus_freeze_pages_max = 0;
unsigned int memorystatus_freeze_suspended_threshold = FREEZE_SUSPENDED_THRESHOLD_DEFAULT;
unsigned int memorystatus_freeze_daily_mb_max = FREEZE_DAILY_MB_MAX_DEFAULT;
uint64_t     memorystatus_freeze_budget_pages_remaining = 0; //remaining # of pages that can be frozen to disk
boolean_t memorystatus_freeze_degradation = FALSE; //protected by the freezer mutex. Signals we are in a degraded freeze mode.

unsigned int memorystatus_max_frozen_demotions_daily = 0;
unsigned int memorystatus_thaw_count_demotion_threshold = 0;

boolean_t memorystatus_freeze_enabled = FALSE;
int memorystatus_freeze_wakeup = 0;
int memorystatus_freeze_jetsam_band = 0; /* the jetsam band which will contain P_MEMSTAT_FROZEN processes */

#define MAX_XPC_SERVICE_PIDS 10 /* Max. # of XPC services per coalition we'll consider freezing. */

#ifdef XNU_KERNEL_PRIVATE

unsigned int memorystatus_frozen_processes_max = 0;
unsigned int memorystatus_frozen_shared_mb = 0;
unsigned int memorystatus_frozen_shared_mb_max = 0;
unsigned int memorystatus_freeze_shared_mb_per_process_max = 0; /* Max. MB allowed per process to be freezer-eligible. */
unsigned int memorystatus_freeze_private_shared_pages_ratio = 2; /* Ratio of private:shared pages for a process to be freezer-eligible. */
unsigned int memorystatus_thaw_count = 0;
unsigned int memorystatus_refreeze_eligible_count = 0; /* # of processes currently thawed i.e. have state on disk & in-memory */

#endif /* XNU_KERNEL_PRIVATE */

static inline boolean_t memorystatus_can_freeze_processes(void);
static boolean_t memorystatus_can_freeze(boolean_t *memorystatus_freeze_swap_low);
static boolean_t memorystatus_is_process_eligible_for_freeze(proc_t p);
static void memorystatus_freeze_thread(void *param __unused, wait_result_t wr __unused);

void memorystatus_disable_freeze(void);

/* Stats */
static uint64_t memorystatus_freeze_pageouts = 0;

/* Throttling */
#define DEGRADED_WINDOW_MINS    (30)
#define NORMAL_WINDOW_MINS      (24 * 60)

static throttle_interval_t throttle_intervals[] = {
	{ DEGRADED_WINDOW_MINS, 1, 0, 0, { 0, 0 }},
	{ NORMAL_WINDOW_MINS, 1, 0, 0, { 0, 0 }},
};
throttle_interval_t *degraded_throttle_window = &throttle_intervals[0];
throttle_interval_t *normal_throttle_window = &throttle_intervals[1];

extern uint64_t vm_swap_get_free_space(void);
extern boolean_t vm_swap_max_budget(uint64_t *);
extern int i_coal_jetsam_get_taskrole(coalition_t coal, task_t task);

static void memorystatus_freeze_update_throttle(uint64_t *budget_pages_allowed);
static void memorystatus_demote_frozen_processes(boolean_t force_one);

static uint64_t memorystatus_freezer_thread_next_run_ts = 0;

/* Sysctls needed for aggd stats */

SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_count, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_frozen_count, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_thaw_count, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_thaw_count, 0, "");
SYSCTL_QUAD(_kern, OID_AUTO, memorystatus_freeze_pageouts, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_freeze_pageouts, "");
SYSCTL_QUAD(_kern, OID_AUTO, memorystatus_freeze_budget_pages_remaining, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_freeze_budget_pages_remaining, "");


#if DEVELOPMENT || DEBUG

SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_jetsam_band, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_jetsam_band, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_daily_mb_max, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_daily_mb_max, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_degraded_mode, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_freeze_degradation, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_threshold, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_threshold, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_pages_min, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_pages_min, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_pages_max, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_pages_max, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_refreeze_eligible_count, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_refreeze_eligible_count, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_processes_max, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_frozen_processes_max, 0, "");

/*
 * Max. shared-anonymous memory in MB that can be held by frozen processes in the high jetsam band.
 * "0" means no limit.
 * Default is 10% of system-wide task limit.
 */

SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_shared_mb_max, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_frozen_shared_mb_max, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_shared_mb, CTLFLAG_RD | CTLFLAG_LOCKED, &memorystatus_frozen_shared_mb, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_shared_mb_per_process_max, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_shared_mb_per_process_max, 0, "");
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_private_shared_pages_ratio, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_private_shared_pages_ratio, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_min_processes, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_suspended_threshold, 0, "");

/*
 * max. # of frozen process demotions we will allow in our daily cycle.
 */
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_max_freeze_demotions_daily, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_max_frozen_demotions_daily, 0, "");
/*
 * min # of thaws needed by a process to protect it from getting demoted into the IDLE band.
 */
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_thaw_count_demotion_threshold, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_thaw_count_demotion_threshold, 0, "");

boolean_t memorystatus_freeze_throttle_enabled = TRUE;
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_throttle_enabled, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_throttle_enabled, 0, "");

/*
 * When set to true, this keeps frozen processes in the compressor pool in memory, instead of swapping them out to disk.
 * Exposed via the sysctl kern.memorystatus_freeze_to_memory.
 */
boolean_t memorystatus_freeze_to_memory = FALSE;
SYSCTL_UINT(_kern, OID_AUTO, memorystatus_freeze_to_memory, CTLFLAG_RW | CTLFLAG_LOCKED, &memorystatus_freeze_to_memory, 0, "");

#define VM_PAGES_FOR_ALL_PROCS    (2)
/*
 * Manual trigger of freeze and thaw for dev / debug kernels only.
 */
static int
sysctl_memorystatus_freeze SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, pid = 0;
	proc_t p;
	int freezer_error_code = 0;
	pid_t pid_list[MAX_XPC_SERVICE_PIDS];
	int ntasks = 0;
	coalition_t coal = COALITION_NULL;

	if (memorystatus_freeze_enabled == FALSE) {
		printf("sysctl_freeze: Freeze is DISABLED\n");
		return ENOTSUP;
	}

	error = sysctl_handle_int(oidp, &pid, 0, req);
	if (error || !req->newptr) {
		return error;
	}

	if (pid == VM_PAGES_FOR_ALL_PROCS) {
		vm_pageout_anonymous_pages();

		return 0;
	}

	lck_mtx_lock(&freezer_mutex);

again:
	p = proc_find(pid);
	if (p != NULL) {
		uint32_t purgeable, wired, clean, dirty, shared;
		uint32_t max_pages = 0, state = 0;

		if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
			/*
			 * Freezer backed by the compressor and swap file(s)
			 * will hold compressed data.
			 *
			 * Set the sysctl kern.memorystatus_freeze_to_memory to true to keep compressed data from
			 * being swapped out to disk. Note that this disables freezer swap support globally,
			 * not just for the process being frozen.
			 *
			 *
			 * We don't care about the global freezer budget or the process's (min/max) budget here.
			 * The freeze sysctl is meant to force-freeze a process.
			 *
			 * We also don't update any global or process stats on this path, so that the jetsam/ freeze
			 * logic remains unaffected. The tasks we're performing here are: freeze the process, set the
			 * P_MEMSTAT_FROZEN bit, and elevate the process to a higher band (if the freezer is active).
			 */
			max_pages = memorystatus_freeze_pages_max;
		} else {
			/*
			 * We only have the compressor without any swap.
			 */
			max_pages = UINT32_MAX - 1;
		}

		proc_list_lock();
		state = p->p_memstat_state;
		proc_list_unlock();

		/*
		 * The jetsam path also verifies that the process is a suspended App. We don't care about that here.
		 * We simply ensure that jetsam is not already working on the process and that the process has not
		 * explicitly disabled freezing.
		 */
		if (state & (P_MEMSTAT_TERMINATED | P_MEMSTAT_LOCKED | P_MEMSTAT_FREEZE_DISABLED)) {
			printf("sysctl_freeze: p_memstat_state check failed, process is%s%s%s\n",
			    (state & P_MEMSTAT_TERMINATED) ? " terminated" : "",
			    (state & P_MEMSTAT_LOCKED) ? " locked" : "",
			    (state & P_MEMSTAT_FREEZE_DISABLED) ? " unfreezable" : "");

			proc_rele(p);
			lck_mtx_unlock(&freezer_mutex);
			return EPERM;
		}

		error = task_freeze(p->task, &purgeable, &wired, &clean, &dirty, max_pages, &shared, &freezer_error_code, FALSE /* eval only */);

		if (error) {
			char reason[128];
			if (freezer_error_code == FREEZER_ERROR_EXCESS_SHARED_MEMORY) {
				strlcpy(reason, "too much shared memory", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_LOW_PRIVATE_SHARED_RATIO) {
				strlcpy(reason, "low private-shared pages ratio", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_NO_COMPRESSOR_SPACE) {
				strlcpy(reason, "no compressor space", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_NO_SWAP_SPACE) {
				strlcpy(reason, "no swap space", 128);
			}

			printf("sysctl_freeze: task_freeze failed: %s\n", reason);

			if (error == KERN_NO_SPACE) {
				/* Make it easy to distinguish between failures due to low compressor/ swap space and other failures. */
				error = ENOSPC;
			} else {
				error = EIO;
			}
		} else {
			proc_list_lock();
			if ((p->p_memstat_state & P_MEMSTAT_FROZEN) == 0) {
				p->p_memstat_state |= P_MEMSTAT_FROZEN;
				memorystatus_frozen_count++;
			}
			p->p_memstat_frozen_count++;


			proc_list_unlock();

			if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
				/*
				 * We elevate only if we are going to swap out the data.
				 */
				error = memorystatus_update_inactive_jetsam_priority_band(pid, MEMORYSTATUS_CMD_ELEVATED_INACTIVEJETSAMPRIORITY_ENABLE,
				    memorystatus_freeze_jetsam_band, TRUE);

				if (error) {
					printf("sysctl_freeze: Elevating frozen process to higher jetsam band failed with %d\n", error);
				}
			}
		}

		if ((error == 0) && (coal == NULL)) {
			/*
			 * We froze a process and so we check to see if it was
			 * a coalition leader and if it has XPC services that
			 * might need freezing.
			 * Only one leader can be frozen at a time and so we shouldn't
			 * enter this block more than once per call. Hence the
			 * check that 'coal' has to be NULL. We should make this an
			 * assert() or panic() once we have a much more concrete way
			 * to detect an app vs a daemon.
			 */

			task_t          curr_task = NULL;

			curr_task = proc_task(p);
			coal = task_get_coalition(curr_task, COALITION_TYPE_JETSAM);
			if (coalition_is_leader(curr_task, coal)) {
				ntasks = coalition_get_pid_list(coal, COALITION_ROLEMASK_XPC,
				    COALITION_SORT_DEFAULT, pid_list, MAX_XPC_SERVICE_PIDS);

				if (ntasks > MAX_XPC_SERVICE_PIDS) {
					ntasks = MAX_XPC_SERVICE_PIDS;
				}
			}
		}

		proc_rele(p);

		while (ntasks) {
			pid = pid_list[--ntasks];
			goto again;
		}

		lck_mtx_unlock(&freezer_mutex);
		return error;
	} else {
		printf("sysctl_freeze: Invalid process\n");
	}


	lck_mtx_unlock(&freezer_mutex);
	return EINVAL;
}

SYSCTL_PROC(_kern, OID_AUTO, memorystatus_freeze, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, &sysctl_memorystatus_freeze, "I", "");

/*
 * Manual trigger of agressive frozen demotion for dev / debug kernels only.
 */
static int
sysctl_memorystatus_demote_frozen_process SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp, req)
	memorystatus_demote_frozen_processes(false);
	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, memorystatus_demote_frozen_processes, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_MASKED, 0, 0, &sysctl_memorystatus_demote_frozen_process, "I", "");

static int
sysctl_memorystatus_available_pages_thaw SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)

	int error, pid = 0;
	proc_t p;

	if (memorystatus_freeze_enabled == FALSE) {
		return ENOTSUP;
	}

	error = sysctl_handle_int(oidp, &pid, 0, req);
	if (error || !req->newptr) {
		return error;
	}

	if (pid == VM_PAGES_FOR_ALL_PROCS) {
		do_fastwake_warmup_all();
		return 0;
	} else {
		p = proc_find(pid);
		if (p != NULL) {
			error = task_thaw(p->task);

			if (error) {
				error = EIO;
			} else {
				/*
				 * task_thaw() succeeded.
				 *
				 * We increment memorystatus_frozen_count on the sysctl freeze path.
				 * And so we need the P_MEMSTAT_FROZEN to decrement the frozen count
				 * when this process exits.
				 *
				 * proc_list_lock();
				 * p->p_memstat_state &= ~P_MEMSTAT_FROZEN;
				 * proc_list_unlock();
				 */
			}
			proc_rele(p);
			return error;
		}
	}

	return EINVAL;
}

SYSCTL_PROC(_kern, OID_AUTO, memorystatus_thaw, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, &sysctl_memorystatus_available_pages_thaw, "I", "");


typedef struct _global_freezable_status {
	boolean_t       freeze_pages_threshold_crossed;
	boolean_t       freeze_eligible_procs_available;
	boolean_t       freeze_scheduled_in_future;
}global_freezable_status_t;

typedef struct _proc_freezable_status {
	boolean_t    freeze_has_memstat_state;
	boolean_t    freeze_has_pages_min;
	int        freeze_has_probability;
	int        freeze_leader_eligible;
	boolean_t    freeze_attempted;
	uint32_t    p_memstat_state;
	uint32_t    p_pages;
	int        p_freeze_error_code;
	int        p_pid;
	int        p_leader_pid;
	char        p_name[MAXCOMLEN + 1];
}proc_freezable_status_t;

#define MAX_FREEZABLE_PROCESSES 200 /* Total # of processes in band 0 that we evaluate for freezability */

/*
 * For coalition based freezing evaluations, we proceed as follows:
 *  - detect that the process is a coalition member and a XPC service
 *  - mark its 'freeze_leader_eligible' field with FREEZE_PROC_LEADER_FREEZABLE_UNKNOWN
 *  - continue its freezability evaluation assuming its leader will be freezable too
 *
 * Once we are done evaluating all processes, we do a quick run thru all
 * processes and for a coalition member XPC service we look up the 'freezable'
 * status of its leader and iff:
 *  - the xpc service is freezable i.e. its individual freeze evaluation worked
 *  - and, its leader is also marked freezable
 * we update its 'freeze_leader_eligible' to FREEZE_PROC_LEADER_FREEZABLE_SUCCESS.
 */

#define FREEZE_PROC_LEADER_FREEZABLE_UNKNOWN   (-1)
#define FREEZE_PROC_LEADER_FREEZABLE_SUCCESS    (1)
#define FREEZE_PROC_LEADER_FREEZABLE_FAILURE    (2)

static int
memorystatus_freezer_get_status(user_addr_t buffer, size_t buffer_size, int32_t *retval)
{
	uint32_t            proc_count = 0, freeze_eligible_proc_considered = 0, band = 0, xpc_index = 0, leader_index = 0;
	global_freezable_status_t    *list_head;
	proc_freezable_status_t     *list_entry, *list_entry_start;
	size_t                list_size = 0;
	proc_t                p, leader_proc;
	memstat_bucket_t        *bucket;
	uint32_t            state = 0, pages = 0, entry_count = 0;
	boolean_t            try_freeze = TRUE, xpc_skip_size_probability_check = FALSE;
	int                error = 0, probability_of_use = 0;
	pid_t              leader_pid = 0;


	if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE == FALSE) {
		return ENOTSUP;
	}

	list_size = sizeof(global_freezable_status_t) + (sizeof(proc_freezable_status_t) * MAX_FREEZABLE_PROCESSES);

	if (buffer_size < list_size) {
		return EINVAL;
	}

	list_head = (global_freezable_status_t*)kalloc(list_size);
	if (list_head == NULL) {
		return ENOMEM;
	}

	memset(list_head, 0, list_size);

	list_size = sizeof(global_freezable_status_t);

	proc_list_lock();

	uint64_t curr_time = mach_absolute_time();

	list_head->freeze_pages_threshold_crossed = (memorystatus_available_pages < memorystatus_freeze_threshold);
	list_head->freeze_eligible_procs_available = ((memorystatus_suspended_count - memorystatus_frozen_count) > memorystatus_freeze_suspended_threshold);
	list_head->freeze_scheduled_in_future = (curr_time < memorystatus_freezer_thread_next_run_ts);

	list_entry_start = (proc_freezable_status_t*) ((uintptr_t)list_head + sizeof(global_freezable_status_t));
	list_entry = list_entry_start;

	bucket = &memstat_bucket[JETSAM_PRIORITY_IDLE];

	entry_count = (memorystatus_global_probabilities_size / sizeof(memorystatus_internal_probabilities_t));

	p = memorystatus_get_first_proc_locked(&band, FALSE);
	proc_count++;

	while ((proc_count <= MAX_FREEZABLE_PROCESSES) &&
	    (p) &&
	    (list_size < buffer_size)) {
		if (isSysProc(p)) {
			/*
			 * Daemon:- We will consider freezing it iff:
			 * - it belongs to a coalition and the leader is freeze-eligible (delayed evaluation)
			 * - its role in the coalition is XPC service.
			 *
			 * We skip memory size requirements in this case.
			 */

			coalition_t     coal = COALITION_NULL;
			task_t          leader_task = NULL, curr_task = NULL;
			int             task_role_in_coalition = 0;

			curr_task = proc_task(p);
			coal = task_get_coalition(curr_task, COALITION_TYPE_JETSAM);

			if (coal == COALITION_NULL || coalition_is_leader(curr_task, coal)) {
				/*
				 * By default, XPC services without an app
				 * will be the leader of their own single-member
				 * coalition.
				 */
				goto skip_ineligible_xpc;
			}

			leader_task = coalition_get_leader(coal);
			if (leader_task == TASK_NULL) {
				/*
				 * This jetsam coalition is currently leader-less.
				 * This could happen if the app died, but XPC services
				 * have not yet exited.
				 */
				goto skip_ineligible_xpc;
			}

			leader_proc = (proc_t)get_bsdtask_info(leader_task);
			task_deallocate(leader_task);

			if (leader_proc == PROC_NULL) {
				/* leader task is exiting */
				goto skip_ineligible_xpc;
			}

			task_role_in_coalition = i_coal_jetsam_get_taskrole(coal, curr_task);

			if (task_role_in_coalition == COALITION_TASKROLE_XPC) {
				xpc_skip_size_probability_check = TRUE;
				leader_pid = leader_proc->p_pid;
				goto continue_eval;
			}

skip_ineligible_xpc:
			p = memorystatus_get_next_proc_locked(&band, p, FALSE);
			proc_count++;
			continue;
		}

continue_eval:
		strlcpy(list_entry->p_name, p->p_name, MAXCOMLEN + 1);

		list_entry->p_pid = p->p_pid;

		state = p->p_memstat_state;

		if ((state & (P_MEMSTAT_TERMINATED | P_MEMSTAT_LOCKED | P_MEMSTAT_FREEZE_DISABLED | P_MEMSTAT_FREEZE_IGNORE)) ||
		    !(state & P_MEMSTAT_SUSPENDED)) {
			try_freeze = list_entry->freeze_has_memstat_state = FALSE;
		} else {
			try_freeze = list_entry->freeze_has_memstat_state = TRUE;
		}

		list_entry->p_memstat_state = state;

		if (xpc_skip_size_probability_check == TRUE) {
			/*
			 * Assuming the coalition leader is freezable
			 * we don't care re. minimum pages and probability
			 * as long as the process isn't marked P_MEMSTAT_FREEZE_DISABLED.
			 * XPC services have to be explicity opted-out of the disabled
			 * state. And we checked that state above.
			 */
			list_entry->freeze_has_pages_min = TRUE;
			list_entry->p_pages = -1;
			list_entry->freeze_has_probability = -1;

			list_entry->freeze_leader_eligible = FREEZE_PROC_LEADER_FREEZABLE_UNKNOWN;
			list_entry->p_leader_pid = leader_pid;

			xpc_skip_size_probability_check = FALSE;
		} else {
			list_entry->freeze_leader_eligible = FREEZE_PROC_LEADER_FREEZABLE_SUCCESS; /* Apps are freeze eligible and their own leaders. */
			list_entry->p_leader_pid = 0; /* Setting this to 0 signifies this isn't a coalition driven freeze. */

			memorystatus_get_task_page_counts(p->task, &pages, NULL, NULL);
			if (pages < memorystatus_freeze_pages_min) {
				try_freeze = list_entry->freeze_has_pages_min = FALSE;
			} else {
				list_entry->freeze_has_pages_min = TRUE;
			}

			list_entry->p_pages = pages;

			if (entry_count) {
				uint32_t j = 0;
				for (j = 0; j < entry_count; j++) {
					if (strncmp(memorystatus_global_probabilities_table[j].proc_name,
					    p->p_name,
					    MAXCOMLEN + 1) == 0) {
						probability_of_use = memorystatus_global_probabilities_table[j].use_probability;
						break;
					}
				}

				list_entry->freeze_has_probability = probability_of_use;

				try_freeze = ((probability_of_use > 0) && try_freeze);
			} else {
				list_entry->freeze_has_probability = -1;
			}
		}

		if (try_freeze) {
			uint32_t purgeable, wired, clean, dirty, shared;
			uint32_t max_pages = 0;
			int freezer_error_code = 0;

			error = task_freeze(p->task, &purgeable, &wired, &clean, &dirty, max_pages, &shared, &freezer_error_code, TRUE /* eval only */);

			if (error) {
				list_entry->p_freeze_error_code = freezer_error_code;
			}

			list_entry->freeze_attempted = TRUE;
		}

		list_entry++;
		freeze_eligible_proc_considered++;

		list_size += sizeof(proc_freezable_status_t);

		p = memorystatus_get_next_proc_locked(&band, p, FALSE);
		proc_count++;
	}

	proc_list_unlock();

	list_entry = list_entry_start;

	for (xpc_index = 0; xpc_index < freeze_eligible_proc_considered; xpc_index++) {
		if (list_entry[xpc_index].freeze_leader_eligible == FREEZE_PROC_LEADER_FREEZABLE_UNKNOWN) {
			leader_pid = list_entry[xpc_index].p_leader_pid;

			leader_proc = proc_find(leader_pid);

			if (leader_proc) {
				if (leader_proc->p_memstat_state & P_MEMSTAT_FROZEN) {
					/*
					 * Leader has already been frozen.
					 */
					list_entry[xpc_index].freeze_leader_eligible = FREEZE_PROC_LEADER_FREEZABLE_SUCCESS;
					proc_rele(leader_proc);
					continue;
				}
				proc_rele(leader_proc);
			}

			for (leader_index = 0; leader_index < freeze_eligible_proc_considered; leader_index++) {
				if (list_entry[leader_index].p_pid == leader_pid) {
					if (list_entry[leader_index].freeze_attempted && list_entry[leader_index].p_freeze_error_code == 0) {
						list_entry[xpc_index].freeze_leader_eligible = FREEZE_PROC_LEADER_FREEZABLE_SUCCESS;
					} else {
						list_entry[xpc_index].freeze_leader_eligible = FREEZE_PROC_LEADER_FREEZABLE_FAILURE;
						list_entry[xpc_index].p_freeze_error_code = FREEZER_ERROR_GENERIC;
					}
					break;
				}
			}

			/*
			 * Didn't find the leader entry. This might be likely because
			 * the leader never made it down to band 0.
			 */
			if (leader_index == freeze_eligible_proc_considered) {
				list_entry[xpc_index].freeze_leader_eligible = FREEZE_PROC_LEADER_FREEZABLE_FAILURE;
				list_entry[xpc_index].p_freeze_error_code = FREEZER_ERROR_GENERIC;
			}
		}
	}

	buffer_size = list_size;

	error = copyout(list_head, buffer, buffer_size);
	if (error == 0) {
		*retval = buffer_size;
	} else {
		*retval = 0;
	}

	list_size = sizeof(global_freezable_status_t) + (sizeof(proc_freezable_status_t) * MAX_FREEZABLE_PROCESSES);
	kfree(list_head, list_size);

	MEMORYSTATUS_DEBUG(1, "memorystatus_freezer_get_status: returning %d (%lu - size)\n", error, (unsigned long)*list_size);

	return error;
}

int
memorystatus_freezer_control(int32_t flags, user_addr_t buffer, size_t buffer_size, int32_t *retval)
{
	int err = ENOTSUP;

	if (flags == FREEZER_CONTROL_GET_STATUS) {
		err = memorystatus_freezer_get_status(buffer, buffer_size, retval);
	}

	return err;
}

#endif /* DEVELOPMENT || DEBUG */

extern void        vm_swap_consider_defragmenting(int);
extern boolean_t memorystatus_kill_elevated_process(uint32_t, os_reason_t, unsigned int, int, uint32_t *, uint64_t *);

/*
 * This routine will _jetsam_ all frozen processes
 * and reclaim the swap space immediately.
 *
 * So freeze has to be DISABLED when we call this routine.
 */

void
memorystatus_disable_freeze(void)
{
	memstat_bucket_t *bucket;
	int bucket_count = 0, retries = 0;
	boolean_t retval = FALSE, killed = FALSE;
	uint32_t errors = 0, errors_over_prev_iteration = 0;
	os_reason_t jetsam_reason = 0;
	unsigned int band = 0;
	proc_t p = PROC_NULL, next_p = PROC_NULL;
	uint64_t memory_reclaimed = 0, footprint = 0;

	KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE_DISABLE) | DBG_FUNC_START,
	    memorystatus_available_pages, 0, 0, 0, 0);

	assert(memorystatus_freeze_enabled == FALSE);

	jetsam_reason = os_reason_create(OS_REASON_JETSAM, JETSAM_REASON_MEMORY_DISK_SPACE_SHORTAGE);
	if (jetsam_reason == OS_REASON_NULL) {
		printf("memorystatus_disable_freeze: failed to allocate jetsam reason\n");
	}

	/*
	 * Let's relocate all frozen processes into band 8. Demoted frozen processes
	 * are sitting in band 0 currently and it's possible to have a frozen process
	 * in the FG band being actively used. We don't reset its frozen state when
	 * it is resumed because it has state on disk.
	 *
	 * We choose to do this relocation rather than implement a new 'kill frozen'
	 * process function for these reasons:
	 * - duplication of code: too many kill functions exist and we need to rework them better.
	 * - disk-space-shortage kills are rare
	 * - not having the 'real' jetsam band at time of the this frozen kill won't preclude us
	 *   from answering any imp. questions re. jetsam policy/effectiveness.
	 *
	 * This is essentially what memorystatus_update_inactive_jetsam_priority_band() does while
	 * avoiding the application of memory limits.
	 */

again:
	proc_list_lock();

	band = JETSAM_PRIORITY_IDLE;
	p = PROC_NULL;
	next_p = PROC_NULL;

	next_p = memorystatus_get_first_proc_locked(&band, TRUE);
	while (next_p) {
		p = next_p;
		next_p = memorystatus_get_next_proc_locked(&band, p, TRUE);

		if (p->p_memstat_effectivepriority > JETSAM_PRIORITY_FOREGROUND) {
			break;
		}

		if ((p->p_memstat_state & P_MEMSTAT_FROZEN) == FALSE) {
			continue;
		}

		if (p->p_memstat_state & P_MEMSTAT_ERROR) {
			p->p_memstat_state &= ~P_MEMSTAT_ERROR;
		}

		if (p->p_memstat_effectivepriority == memorystatus_freeze_jetsam_band) {
			continue;
		}

		/*
		 * We explicitly add this flag here so the process looks like a normal
		 * frozen process i.e. P_MEMSTAT_FROZEN and P_MEMSTAT_USE_ELEVATED_INACTIVE_BAND.
		 * We don't bother with assigning the 'active' memory
		 * limits at this point because we are going to be killing it soon below.
		 */
		p->p_memstat_state |= P_MEMSTAT_USE_ELEVATED_INACTIVE_BAND;
		memorystatus_invalidate_idle_demotion_locked(p, TRUE);

		memorystatus_update_priority_locked(p, memorystatus_freeze_jetsam_band, FALSE, TRUE);
	}

	bucket = &memstat_bucket[memorystatus_freeze_jetsam_band];
	bucket_count = bucket->count;
	proc_list_unlock();

	/*
	 * Bucket count is already stale at this point. But, we don't expect
	 * freezing to continue since we have already disabled the freeze functionality.
	 * However, an existing freeze might be in progress. So we might miss that process
	 * in the first go-around. We hope to catch it in the next.
	 */

	errors_over_prev_iteration = 0;
	while (bucket_count) {
		bucket_count--;

		/*
		 * memorystatus_kill_elevated_process() drops a reference,
		 * so take another one so we can continue to use this exit reason
		 * even after it returns.
		 */

		os_reason_ref(jetsam_reason);
		retval = memorystatus_kill_elevated_process(
			kMemorystatusKilledDiskSpaceShortage,
			jetsam_reason,
			memorystatus_freeze_jetsam_band,
			0,                             /* the iteration of aggressive jetsam..ignored here */
			&errors,
			&footprint);

		if (errors > 0) {
			printf("memorystatus_disable_freeze: memorystatus_kill_elevated_process returned %d error(s)\n", errors);
			errors_over_prev_iteration += errors;
			errors = 0;
		}

		if (retval == 0) {
			/*
			 * No frozen processes left to kill.
			 */
			break;
		}

		killed = TRUE;
		memory_reclaimed += footprint;
	}

	proc_list_lock();

	if (memorystatus_frozen_count) {
		/*
		 * A frozen process snuck in and so
		 * go back around to kill it. That
		 * process may have been resumed and
		 * put into the FG band too. So we
		 * have to do the relocation again.
		 */
		assert(memorystatus_freeze_enabled == FALSE);

		retries++;
		if (retries < 3) {
			proc_list_unlock();
			goto again;
		}
#if DEVELOPMENT || DEBUG
		panic("memorystatus_disable_freeze: Failed to kill all frozen processes, memorystatus_frozen_count = %d, errors = %d",
		    memorystatus_frozen_count, errors_over_prev_iteration);
#endif /* DEVELOPMENT || DEBUG */
	}
	proc_list_unlock();

	os_reason_free(jetsam_reason);

	if (killed) {
		vm_swap_consider_defragmenting(VM_SWAP_FLAGS_FORCE_DEFRAG | VM_SWAP_FLAGS_FORCE_RECLAIM);

		proc_list_lock();
		size_t snapshot_size = sizeof(memorystatus_jetsam_snapshot_t) +
		    sizeof(memorystatus_jetsam_snapshot_entry_t) * (memorystatus_jetsam_snapshot_count);
		uint64_t timestamp_now = mach_absolute_time();
		memorystatus_jetsam_snapshot->notification_time = timestamp_now;
		memorystatus_jetsam_snapshot->js_gencount++;
		if (memorystatus_jetsam_snapshot_count > 0 && (memorystatus_jetsam_snapshot_last_timestamp == 0 ||
		    timestamp_now > memorystatus_jetsam_snapshot_last_timestamp + memorystatus_jetsam_snapshot_timeout)) {
			proc_list_unlock();
			int ret = memorystatus_send_note(kMemorystatusSnapshotNote, &snapshot_size, sizeof(snapshot_size));
			if (!ret) {
				proc_list_lock();
				memorystatus_jetsam_snapshot_last_timestamp = timestamp_now;
				proc_list_unlock();
			}
		} else {
			proc_list_unlock();
		}
	}

	KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE_DISABLE) | DBG_FUNC_END,
	    memorystatus_available_pages, memory_reclaimed, 0, 0, 0);

	return;
}

__private_extern__ void
memorystatus_freeze_init(void)
{
	kern_return_t result;
	thread_t thread;

	freezer_lck_grp_attr = lck_grp_attr_alloc_init();
	freezer_lck_grp = lck_grp_alloc_init("freezer", freezer_lck_grp_attr);

	lck_mtx_init(&freezer_mutex, freezer_lck_grp, NULL);

	/*
	 * This is just the default value if the underlying
	 * storage device doesn't have any specific budget.
	 * We check with the storage layer in memorystatus_freeze_update_throttle()
	 * before we start our freezing the first time.
	 */
	memorystatus_freeze_budget_pages_remaining = (memorystatus_freeze_daily_mb_max * 1024 * 1024) / PAGE_SIZE;

	result = kernel_thread_start(memorystatus_freeze_thread, NULL, &thread);
	if (result == KERN_SUCCESS) {
		proc_set_thread_policy(thread, TASK_POLICY_INTERNAL, TASK_POLICY_IO, THROTTLE_LEVEL_COMPRESSOR_TIER2);
		proc_set_thread_policy(thread, TASK_POLICY_INTERNAL, TASK_POLICY_PASSIVE_IO, TASK_POLICY_ENABLE);
		thread_set_thread_name(thread, "VM_freezer");

		thread_deallocate(thread);
	} else {
		panic("Could not create memorystatus_freeze_thread");
	}
}

static boolean_t
memorystatus_is_process_eligible_for_freeze(proc_t p)
{
	/*
	 * Called with proc_list_lock held.
	 */

	LCK_MTX_ASSERT(proc_list_mlock, LCK_MTX_ASSERT_OWNED);

	boolean_t should_freeze = FALSE;
	uint32_t state = 0, entry_count = 0, pages = 0, i = 0;
	int probability_of_use = 0;

	state = p->p_memstat_state;

	if (state & (P_MEMSTAT_TERMINATED | P_MEMSTAT_LOCKED | P_MEMSTAT_FREEZE_DISABLED | P_MEMSTAT_FREEZE_IGNORE)) {
		goto out;
	}

	if (isSysProc(p)) {
		/*
		 * Daemon:- We consider freezing it if:
		 * - it belongs to a coalition and the leader is frozen, and,
		 * - its role in the coalition is XPC service.
		 *
		 * We skip memory size requirements in this case.
		 */

		coalition_t     coal = COALITION_NULL;
		task_t          leader_task = NULL, curr_task = NULL;
		proc_t          leader_proc = NULL;
		int             task_role_in_coalition = 0;

		curr_task = proc_task(p);
		coal = task_get_coalition(curr_task, COALITION_TYPE_JETSAM);

		if (coal == NULL || coalition_is_leader(curr_task, coal)) {
			/*
			 * By default, XPC services without an app
			 * will be the leader of their own single-member
			 * coalition.
			 */
			goto out;
		}

		leader_task = coalition_get_leader(coal);
		if (leader_task == TASK_NULL) {
			/*
			 * This jetsam coalition is currently leader-less.
			 * This could happen if the app died, but XPC services
			 * have not yet exited.
			 */
			goto out;
		}

		leader_proc = (proc_t)get_bsdtask_info(leader_task);
		task_deallocate(leader_task);

		if (leader_proc == PROC_NULL) {
			/* leader task is exiting */
			goto out;
		}

		if (!(leader_proc->p_memstat_state & P_MEMSTAT_FROZEN)) {
			goto out;
		}

		task_role_in_coalition = i_coal_jetsam_get_taskrole(coal, curr_task);

		if (task_role_in_coalition == COALITION_TASKROLE_XPC) {
			should_freeze = TRUE;
		}

		goto out;
	} else {
		/*
		 * Application. In addition to the above states we need to make
		 * sure we only consider suspended applications for freezing.
		 */
		if (!(state & P_MEMSTAT_SUSPENDED)) {
			goto out;
		}
	}


	/* Only freeze applications meeting our minimum resident page criteria */
	memorystatus_get_task_page_counts(p->task, &pages, NULL, NULL);
	if (pages < memorystatus_freeze_pages_min) {
		goto out;
	}

	/* Don't freeze processes that are already exiting on core. It may have started exiting
	 * after we chose it for freeze, but before we obtained the proc_list_lock.
	 * NB: This is only possible if we're coming in from memorystatus_freeze_process_sync.
	 * memorystatus_freeze_top_process holds the proc_list_lock while it traverses the bands.
	 */
	if ((p->p_listflag & P_LIST_EXITED) != 0) {
		goto out;
	}

	entry_count = (memorystatus_global_probabilities_size / sizeof(memorystatus_internal_probabilities_t));

	if (entry_count) {
		for (i = 0; i < entry_count; i++) {
			if (strncmp(memorystatus_global_probabilities_table[i].proc_name,
			    p->p_name,
			    MAXCOMLEN + 1) == 0) {
				probability_of_use = memorystatus_global_probabilities_table[i].use_probability;
				break;
			}
		}

		if (probability_of_use == 0) {
			goto out;
		}
	}

	should_freeze = TRUE;
out:
	return should_freeze;
}

/*
 * Synchronously freeze the passed proc. Called with a reference to the proc held.
 *
 * Doesn't deal with:
 * - re-freezing because this is called on a specific process and
 *   not by the freezer thread. If that changes, we'll have to teach it about
 *   refreezing a frozen process.
 *
 * - grouped/coalition freezing because we are hoping to deprecate this
 *   interface as it was used by user-space to freeze particular processes. But
 *   we have moved away from that approach to having the kernel choose the optimal
 *   candidates to be frozen.
 *
 * Returns EINVAL or the value returned by task_freeze().
 */
int
memorystatus_freeze_process_sync(proc_t p)
{
	int ret = EINVAL;
	pid_t aPid = 0;
	boolean_t memorystatus_freeze_swap_low = FALSE;
	int    freezer_error_code = 0;

	lck_mtx_lock(&freezer_mutex);

	if (p == NULL) {
		printf("memorystatus_freeze_process_sync: Invalid process\n");
		goto exit;
	}

	if (memorystatus_freeze_enabled == FALSE) {
		printf("memorystatus_freeze_process_sync: Freezing is DISABLED\n");
		goto exit;
	}

	if (!memorystatus_can_freeze(&memorystatus_freeze_swap_low)) {
		printf("memorystatus_freeze_process_sync: Low compressor and/or low swap space...skipping freeze\n");
		goto exit;
	}

	memorystatus_freeze_update_throttle(&memorystatus_freeze_budget_pages_remaining);
	if (!memorystatus_freeze_budget_pages_remaining) {
		printf("memorystatus_freeze_process_sync: exit with NO available budget\n");
		goto exit;
	}

	proc_list_lock();

	if (p != NULL) {
		uint32_t purgeable, wired, clean, dirty, shared;
		uint32_t max_pages, i;

		aPid = p->p_pid;

		/* Ensure the process is eligible for freezing */
		if (memorystatus_is_process_eligible_for_freeze(p) == FALSE) {
			proc_list_unlock();
			goto exit;
		}

		if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
			max_pages = MIN(memorystatus_freeze_pages_max, memorystatus_freeze_budget_pages_remaining);
		} else {
			/*
			 * We only have the compressor without any swap.
			 */
			max_pages = UINT32_MAX - 1;
		}

		/* Mark as locked temporarily to avoid kill */
		p->p_memstat_state |= P_MEMSTAT_LOCKED;
		proc_list_unlock();

		KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE) | DBG_FUNC_START,
		    memorystatus_available_pages, 0, 0, 0, 0);

		ret = task_freeze(p->task, &purgeable, &wired, &clean, &dirty, max_pages, &shared, &freezer_error_code, FALSE /* eval only */);

		KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE) | DBG_FUNC_END,
		    memorystatus_available_pages, aPid, 0, 0, 0);

		DTRACE_MEMORYSTATUS6(memorystatus_freeze, proc_t, p, unsigned int, memorystatus_available_pages, boolean_t, purgeable, unsigned int, wired, uint32_t, clean, uint32_t, dirty);

		MEMORYSTATUS_DEBUG(1, "memorystatus_freeze_process_sync: task_freeze %s for pid %d [%s] - "
		    "memorystatus_pages: %d, purgeable: %d, wired: %d, clean: %d, dirty: %d, max_pages %d, shared %d\n",
		    (ret == KERN_SUCCESS) ? "SUCCEEDED" : "FAILED", aPid, (*p->p_name ? p->p_name : "(unknown)"),
		    memorystatus_available_pages, purgeable, wired, clean, dirty, max_pages, shared);

		proc_list_lock();

		if (ret == KERN_SUCCESS) {
			memorystatus_freeze_entry_t data = { aPid, TRUE, dirty };

			p->p_memstat_freeze_sharedanon_pages += shared;

			memorystatus_frozen_shared_mb += shared;

			if ((p->p_memstat_state & P_MEMSTAT_FROZEN) == 0) {
				p->p_memstat_state |= P_MEMSTAT_FROZEN;
				memorystatus_frozen_count++;
			}

			p->p_memstat_frozen_count++;

			/*
			 * Still keeping the P_MEMSTAT_LOCKED bit till we are actually done elevating this frozen process
			 * to its higher jetsam band.
			 */
			proc_list_unlock();

			memorystatus_send_note(kMemorystatusFreezeNote, &data, sizeof(data));

			if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
				ret = memorystatus_update_inactive_jetsam_priority_band(p->p_pid, MEMORYSTATUS_CMD_ELEVATED_INACTIVEJETSAMPRIORITY_ENABLE,
				    memorystatus_freeze_jetsam_band, TRUE);

				if (ret) {
					printf("Elevating the frozen process failed with %d\n", ret);
					/* not fatal */
					ret = 0;
				}

				proc_list_lock();

				/* Update stats */
				for (i = 0; i < sizeof(throttle_intervals) / sizeof(struct throttle_interval_t); i++) {
					throttle_intervals[i].pageouts += dirty;
				}
			} else {
				proc_list_lock();
			}

			memorystatus_freeze_pageouts += dirty;

			if (memorystatus_frozen_count == (memorystatus_frozen_processes_max - 1)) {
				/*
				 * Add some eviction logic here? At some point should we
				 * jetsam a process to get back its swap space so that we
				 * can freeze a more eligible process at this moment in time?
				 */
			}

			memorystatus_freeze_update_throttle(&memorystatus_freeze_budget_pages_remaining);
			os_log_with_startup_serial(OS_LOG_DEFAULT, "memorystatus: freezing (specific) pid %d [%s] done memorystatus_freeze_budget_pages_remaining %llu froze %u pages",
			    aPid, ((p && *p->p_name) ? p->p_name : "unknown"), memorystatus_freeze_budget_pages_remaining, dirty);
		} else {
			char reason[128];
			if (freezer_error_code == FREEZER_ERROR_EXCESS_SHARED_MEMORY) {
				strlcpy(reason, "too much shared memory", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_LOW_PRIVATE_SHARED_RATIO) {
				strlcpy(reason, "low private-shared pages ratio", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_NO_COMPRESSOR_SPACE) {
				strlcpy(reason, "no compressor space", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_NO_SWAP_SPACE) {
				strlcpy(reason, "no swap space", 128);
			}

			os_log_with_startup_serial(OS_LOG_DEFAULT, "memorystatus: freezing (specific) pid %d [%s]...skipped (%s)",
			    aPid, ((p && *p->p_name) ? p->p_name : "unknown"), reason);
			p->p_memstat_state |= P_MEMSTAT_FREEZE_IGNORE;
		}

		p->p_memstat_state &= ~P_MEMSTAT_LOCKED;
		wakeup(&p->p_memstat_state);
		proc_list_unlock();
	}

exit:
	lck_mtx_unlock(&freezer_mutex);

	return ret;
}

static int
memorystatus_freeze_top_process(void)
{
	pid_t aPid = 0, coal_xpc_pid = 0;
	int ret = -1;
	proc_t p = PROC_NULL, next_p = PROC_NULL;
	unsigned int i = 0;
	unsigned int band = JETSAM_PRIORITY_IDLE;
	boolean_t refreeze_processes = FALSE;
	task_t curr_task = NULL;
	coalition_t coal = COALITION_NULL;
	pid_t pid_list[MAX_XPC_SERVICE_PIDS];
	unsigned int    ntasks = 0;

	KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE_SCAN) | DBG_FUNC_START, memorystatus_available_pages, 0, 0, 0, 0);

	proc_list_lock();

	if (memorystatus_frozen_count >= memorystatus_frozen_processes_max) {
		/*
		 * Freezer is already full but we are here and so let's
		 * try to refreeze any processes we might have thawed
		 * in the past and push out their compressed state out.
		 */
		refreeze_processes = TRUE;
		band = (unsigned int) memorystatus_freeze_jetsam_band;
	}

freeze_process:

	next_p = memorystatus_get_first_proc_locked(&band, FALSE);
	while (next_p) {
		kern_return_t kr;
		uint32_t purgeable, wired, clean, dirty, shared;
		uint32_t max_pages = 0;
		int    freezer_error_code = 0;

		p = next_p;

		if (coal == NULL) {
			next_p = memorystatus_get_next_proc_locked(&band, p, FALSE);
		} else {
			/*
			 * We have frozen a coalition leader and now are
			 * dealing with its XPC services. We get our
			 * next_p for each XPC service from the pid_list
			 * acquired after a successful task_freeze call
			 * on the coalition leader.
			 */

			if (ntasks > 0) {
				coal_xpc_pid = pid_list[--ntasks];
				next_p = proc_findinternal(coal_xpc_pid, 1 /* proc_list_lock held */);
				/*
				 * We grab a reference when we are about to freeze the process. So, drop
				 * the reference that proc_findinternal() grabbed for us.
				 * We also have the proc_list_lock and so this process is stable.
				 */
				if (next_p) {
					proc_rele_locked(next_p);
				}
			} else {
				next_p = NULL;
			}
		}

		aPid = p->p_pid;

		if (p->p_memstat_effectivepriority != (int32_t) band) {
			/*
			 * We shouldn't be freezing processes outside the
			 * prescribed band.
			 */
			break;
		}

		/* Ensure the process is eligible for (re-)freezing */
		if (refreeze_processes) {
			/*
			 * Has to have been frozen once before.
			 */
			if ((p->p_memstat_state & P_MEMSTAT_FROZEN) == FALSE) {
				continue;
			}

			/*
			 * Has to have been resumed once before.
			 */
			if ((p->p_memstat_state & P_MEMSTAT_REFREEZE_ELIGIBLE) == FALSE) {
				continue;
			}

			/*
			 * Not currently being looked at for something.
			 */
			if (p->p_memstat_state & P_MEMSTAT_LOCKED) {
				continue;
			}

			/*
			 * We are going to try and refreeze and so re-evaluate
			 * the process. We don't want to double count the shared
			 * memory. So deduct the old snapshot here.
			 */
			memorystatus_frozen_shared_mb -= p->p_memstat_freeze_sharedanon_pages;
			p->p_memstat_freeze_sharedanon_pages = 0;

			p->p_memstat_state &= ~P_MEMSTAT_REFREEZE_ELIGIBLE;
			memorystatus_refreeze_eligible_count--;
		} else {
			if (memorystatus_is_process_eligible_for_freeze(p) == FALSE) {
				continue; // with lock held
			}
		}

		if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
			/*
			 * Freezer backed by the compressor and swap file(s)
			 * will hold compressed data.
			 */

			max_pages = MIN(memorystatus_freeze_pages_max, memorystatus_freeze_budget_pages_remaining);
		} else {
			/*
			 * We only have the compressor pool.
			 */
			max_pages = UINT32_MAX - 1;
		}

		/* Mark as locked temporarily to avoid kill */
		p->p_memstat_state |= P_MEMSTAT_LOCKED;

		p = proc_ref_locked(p);
		if (!p) {
			break;
		}

		proc_list_unlock();

		KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE) | DBG_FUNC_START,
		    memorystatus_available_pages, 0, 0, 0, 0);

		kr = task_freeze(p->task, &purgeable, &wired, &clean, &dirty, max_pages, &shared, &freezer_error_code, FALSE /* eval only */);

		KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE) | DBG_FUNC_END,
		    memorystatus_available_pages, aPid, 0, 0, 0);

		MEMORYSTATUS_DEBUG(1, "memorystatus_freeze_top_process: task_freeze %s for pid %d [%s] - "
		    "memorystatus_pages: %d, purgeable: %d, wired: %d, clean: %d, dirty: %d, max_pages %d, shared %d\n",
		    (kr == KERN_SUCCESS) ? "SUCCEEDED" : "FAILED", aPid, (*p->p_name ? p->p_name : "(unknown)"),
		    memorystatus_available_pages, purgeable, wired, clean, dirty, max_pages, shared);

		proc_list_lock();

		/* Success? */
		if (KERN_SUCCESS == kr) {
			memorystatus_freeze_entry_t data = { aPid, TRUE, dirty };

			p->p_memstat_freeze_sharedanon_pages += shared;

			memorystatus_frozen_shared_mb += shared;

			if ((p->p_memstat_state & P_MEMSTAT_FROZEN) == 0) {
				p->p_memstat_state |= P_MEMSTAT_FROZEN;
				memorystatus_frozen_count++;
			}

			p->p_memstat_frozen_count++;

			/*
			 * Still keeping the P_MEMSTAT_LOCKED bit till we are actually done elevating this frozen process
			 * to its higher jetsam band.
			 */
			proc_list_unlock();

			memorystatus_send_note(kMemorystatusFreezeNote, &data, sizeof(data));

			if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
				ret = memorystatus_update_inactive_jetsam_priority_band(p->p_pid, MEMORYSTATUS_CMD_ELEVATED_INACTIVEJETSAMPRIORITY_ENABLE, memorystatus_freeze_jetsam_band, TRUE);

				if (ret) {
					printf("Elevating the frozen process failed with %d\n", ret);
					/* not fatal */
					ret = 0;
				}

				proc_list_lock();

				/* Update stats */
				for (i = 0; i < sizeof(throttle_intervals) / sizeof(struct throttle_interval_t); i++) {
					throttle_intervals[i].pageouts += dirty;
				}
			} else {
				proc_list_lock();
			}

			memorystatus_freeze_pageouts += dirty;

			if (memorystatus_frozen_count == (memorystatus_frozen_processes_max - 1)) {
				/*
				 * Add some eviction logic here? At some point should we
				 * jetsam a process to get back its swap space so that we
				 * can freeze a more eligible process at this moment in time?
				 */
			}

			memorystatus_freeze_update_throttle(&memorystatus_freeze_budget_pages_remaining);
			os_log_with_startup_serial(OS_LOG_DEFAULT, "memorystatus: %sfreezing (%s) pid %d [%s] done, memorystatus_freeze_budget_pages_remaining %llu %sfroze %u pages\n",
			    refreeze_processes? "re" : "", (coal == NULL ? "general" : "coalition-driven"), aPid, ((p && *p->p_name) ? p->p_name : "unknown"), memorystatus_freeze_budget_pages_remaining, refreeze_processes? "Re" : "", dirty);

			/* Return KERN_SUCCESS */
			ret = kr;

			/*
			 * We froze a process successfully. We can stop now
			 * and see if that helped if this process isn't part
			 * of a coalition.
			 *
			 * Else:
			 * - if it is a leader, get the list of XPC services
			 *   that need to be frozen.
			 * - if it is a XPC service whose leader was frozen
			 *   here, continue on to the next XPC service in the list.
			 */

			if (coal == NULL) {
				curr_task = proc_task(p);
				coal = task_get_coalition(curr_task, COALITION_TYPE_JETSAM);
				if (coalition_is_leader(curr_task, coal)) {
					ntasks = coalition_get_pid_list(coal, COALITION_ROLEMASK_XPC,
					    COALITION_SORT_DEFAULT, pid_list, MAX_XPC_SERVICE_PIDS);

					if (ntasks > MAX_XPC_SERVICE_PIDS) {
						ntasks = MAX_XPC_SERVICE_PIDS;
					}
				}

				next_p = NULL;

				if (ntasks > 0) {
					/*
					 * Start off with our first next_p in this list.
					 */
					coal_xpc_pid = pid_list[--ntasks];
					next_p = proc_findinternal(coal_xpc_pid, 1 /* proc_list_lock held */);

					/*
					 * We grab a reference when we are about to freeze the process. So drop
					 * the reference that proc_findinternal() grabbed for us.
					 * We also have the proc_list_lock and so this process is stable.
					 */
					if (next_p) {
						proc_rele_locked(next_p);
					}
				}
			}

			p->p_memstat_state &= ~P_MEMSTAT_LOCKED;
			wakeup(&p->p_memstat_state);
			proc_rele_locked(p);

			if (coal && next_p) {
				continue;
			}

			/*
			 * No coalition leader was frozen. So we don't
			 * need to evaluate any XPC services.
			 *
			 * OR
			 *
			 * We have frozen all eligible XPC services for
			 * the current coalition leader.
			 *
			 * Either way, we can break here and see if freezing
			 * helped.
			 */

			break;
		} else {
			p->p_memstat_state &= ~P_MEMSTAT_LOCKED;
			wakeup(&p->p_memstat_state);

			if (refreeze_processes == TRUE) {
				if ((freezer_error_code == FREEZER_ERROR_EXCESS_SHARED_MEMORY) ||
				    (freezer_error_code == FREEZER_ERROR_LOW_PRIVATE_SHARED_RATIO)) {
					/*
					 * Keeping this prior-frozen process in this high band when
					 * we failed to re-freeze it due to bad shared memory usage
					 * could cause excessive pressure on the lower bands.
					 * We need to demote it for now. It'll get re-evaluated next
					 * time because we don't set the P_MEMSTAT_FREEZE_IGNORE
					 * bit.
					 */

					p->p_memstat_state &= ~P_MEMSTAT_USE_ELEVATED_INACTIVE_BAND;
					memorystatus_invalidate_idle_demotion_locked(p, TRUE);
					memorystatus_update_priority_locked(p, JETSAM_PRIORITY_IDLE, TRUE, TRUE);
				}
			} else {
				p->p_memstat_state |= P_MEMSTAT_FREEZE_IGNORE;
			}

			char reason[128];
			if (freezer_error_code == FREEZER_ERROR_EXCESS_SHARED_MEMORY) {
				strlcpy(reason, "too much shared memory", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_LOW_PRIVATE_SHARED_RATIO) {
				strlcpy(reason, "low private-shared pages ratio", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_NO_COMPRESSOR_SPACE) {
				strlcpy(reason, "no compressor space", 128);
			}

			if (freezer_error_code == FREEZER_ERROR_NO_SWAP_SPACE) {
				strlcpy(reason, "no swap space", 128);
			}

			os_log_with_startup_serial(OS_LOG_DEFAULT, "memorystatus: freezing (%s) pid %d [%s]...skipped (%s)\n",
			    (coal == NULL ? "general" : "coalition-driven"), aPid, ((p && *p->p_name) ? p->p_name : "unknown"), reason);

			proc_rele_locked(p);

			if (vm_compressor_low_on_space() || vm_swap_low_on_space()) {
				break;
			}
		}
	}

	if ((ret == -1) &&
	    (memorystatus_refreeze_eligible_count >= MIN_THAW_REFREEZE_THRESHOLD) &&
	    (refreeze_processes == FALSE)) {
		/*
		 * We failed to freeze a process from the IDLE
		 * band AND we have some thawed  processes
		 * AND haven't tried refreezing as yet.
		 * Let's try and re-freeze processes in the
		 * frozen band that have been resumed in the past
		 * and so have brought in state from disk.
		 */

		band = (unsigned int) memorystatus_freeze_jetsam_band;

		refreeze_processes = TRUE;

		goto freeze_process;
	}

	proc_list_unlock();

	KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_MEMSTAT, BSD_MEMSTAT_FREEZE_SCAN) | DBG_FUNC_END, memorystatus_available_pages, aPid, 0, 0, 0);

	return ret;
}

static inline boolean_t
memorystatus_can_freeze_processes(void)
{
	boolean_t ret;

	proc_list_lock();

	if (memorystatus_suspended_count) {
		memorystatus_freeze_suspended_threshold = MIN(memorystatus_freeze_suspended_threshold, FREEZE_SUSPENDED_THRESHOLD_DEFAULT);

		if ((memorystatus_suspended_count - memorystatus_frozen_count) > memorystatus_freeze_suspended_threshold) {
			ret = TRUE;
		} else {
			ret = FALSE;
		}
	} else {
		ret = FALSE;
	}

	proc_list_unlock();

	return ret;
}

static boolean_t
memorystatus_can_freeze(boolean_t *memorystatus_freeze_swap_low)
{
	boolean_t can_freeze = TRUE;

	/* Only freeze if we're sufficiently low on memory; this holds off freeze right
	*  after boot,  and is generally is a no-op once we've reached steady state. */
	if (memorystatus_available_pages > memorystatus_freeze_threshold) {
		return FALSE;
	}

	/* Check minimum suspended process threshold. */
	if (!memorystatus_can_freeze_processes()) {
		return FALSE;
	}
	assert(VM_CONFIG_COMPRESSOR_IS_PRESENT);

	if (!VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
		/*
		 * In-core compressor used for freezing WITHOUT on-disk swap support.
		 */
		if (vm_compressor_low_on_space()) {
			if (*memorystatus_freeze_swap_low) {
				*memorystatus_freeze_swap_low = TRUE;
			}

			can_freeze = FALSE;
		} else {
			if (*memorystatus_freeze_swap_low) {
				*memorystatus_freeze_swap_low = FALSE;
			}

			can_freeze = TRUE;
		}
	} else {
		/*
		 * Freezing WITH on-disk swap support.
		 *
		 * In-core compressor fronts the swap.
		 */
		if (vm_swap_low_on_space()) {
			if (*memorystatus_freeze_swap_low) {
				*memorystatus_freeze_swap_low = TRUE;
			}

			can_freeze = FALSE;
		}
	}

	return can_freeze;
}

/*
 * This function evaluates if the currently frozen processes deserve
 * to stay in the higher jetsam band. There are 2 modes:
 * - 'force one == TRUE': (urgent mode)
 *	We are out of budget and can't refreeze a process. The process's
 * state, if it was resumed, will stay in compressed memory. If we let it
 * remain up in the higher frozen jetsam band, it'll put a lot of pressure on
 * the lower bands. So we force-demote the least-recently-used-and-thawed
 * process.
 *
 * - 'force_one == FALSE': (normal mode)
 *      If the # of thaws of a process is below our threshold, then we
 * will demote that process into the IDLE band.
 * We don't immediately kill the process here because it  already has
 * state on disk and so it might be worth giving it another shot at
 * getting thawed/resumed and used.
 */
static void
memorystatus_demote_frozen_processes(boolean_t force_one)
{
	unsigned int band = (unsigned int) memorystatus_freeze_jetsam_band;
	unsigned int demoted_proc_count = 0;
	proc_t p = PROC_NULL, next_p = PROC_NULL;
	/* We demote to IDLE unless someone has asserted a higher priority on this process. */
	int maxpriority = JETSAM_PRIORITY_IDLE;

	proc_list_lock();

	if (memorystatus_freeze_enabled == FALSE) {
		/*
		 * Freeze has been disabled likely to
		 * reclaim swap space. So don't change
		 * any state on the frozen processes.
		 */
		proc_list_unlock();
		return;
	}

	next_p = memorystatus_get_first_proc_locked(&band, FALSE);
	while (next_p) {
		p = next_p;
		next_p = memorystatus_get_next_proc_locked(&band, p, FALSE);

		if ((p->p_memstat_state & P_MEMSTAT_FROZEN) == FALSE) {
			continue;
		}

		if (p->p_memstat_state & P_MEMSTAT_LOCKED) {
			continue;
		}

		if (force_one == TRUE) {
			if ((p->p_memstat_state & P_MEMSTAT_REFREEZE_ELIGIBLE) == 0) {
				/*
				 * This process hasn't been thawed recently and so most of
				 * its state sits on NAND and so we skip it -- jetsamming it
				 * won't help with memory pressure.
				 */
				continue;
			}
		} else {
			if (p->p_memstat_thaw_count >= memorystatus_thaw_count_demotion_threshold) {
				/*
				 * This process has met / exceeded our thaw count demotion threshold
				 * and so we let it live in the higher bands.
				 */
				continue;
			}
		}

		p->p_memstat_state &= ~P_MEMSTAT_USE_ELEVATED_INACTIVE_BAND;
		memorystatus_invalidate_idle_demotion_locked(p, TRUE);

		maxpriority = MAX(p->p_memstat_assertionpriority, maxpriority);
		memorystatus_update_priority_locked(p, maxpriority, FALSE, FALSE);
#if DEVELOPMENT || DEBUG
		os_log_with_startup_serial(OS_LOG_DEFAULT, "memorystatus_demote_frozen_process(%s) pid %d [%s]",
		    (force_one ? "urgent" : "normal"), (p ? p->p_pid : -1), ((p && *p->p_name) ? p->p_name : "unknown"));
#endif /* DEVELOPMENT || DEBUG */

		/*
		 * The freezer thread will consider this a normal app to be frozen
		 * because it is in the IDLE band. So we don't need the
		 * P_MEMSTAT_REFREEZE_ELIGIBLE state here. Also, if it gets resumed
		 * we'll correctly count it as eligible for re-freeze again.
		 *
		 * We don't drop the frozen count because this process still has
		 * state on disk. So there's a chance it gets resumed and then it
		 * should land in the higher jetsam band. For that it needs to
		 * remain marked frozen.
		 */
		if (p->p_memstat_state & P_MEMSTAT_REFREEZE_ELIGIBLE) {
			p->p_memstat_state &= ~P_MEMSTAT_REFREEZE_ELIGIBLE;
			memorystatus_refreeze_eligible_count--;
		}

		demoted_proc_count++;

		if ((force_one == TRUE) || (demoted_proc_count == memorystatus_max_frozen_demotions_daily)) {
			break;
		}
	}

	if (force_one == FALSE) {
		/*
		 * We use this counter to track daily thaws.
		 * So we only reset it to 0 under the normal
		 * mode.
		 */
		memorystatus_thaw_count = 0;
	}

	proc_list_unlock();
}


/*
 * This function will do 4 things:
 *
 * 1) check to see if we are currently in a degraded freezer mode, and if so:
 *    - check to see if our window has expired and we should exit this mode, OR,
 *    - return a budget based on the degraded throttle window's max. pageouts vs current pageouts.
 *
 * 2) check to see if we are in a NEW normal window and update the normal throttle window's params.
 *
 * 3) check what the current normal window allows for a budget.
 *
 * 4) calculate the current rate of pageouts for DEGRADED_WINDOW_MINS duration. If that rate is below
 *    what we would normally expect, then we are running low on our daily budget and need to enter
 *    degraded perf. mode.
 */

static void
memorystatus_freeze_update_throttle(uint64_t *budget_pages_allowed)
{
	clock_sec_t sec;
	clock_nsec_t nsec;
	mach_timespec_t ts;

	unsigned int freeze_daily_pageouts_max = 0;

#if DEVELOPMENT || DEBUG
	if (!memorystatus_freeze_throttle_enabled) {
		/*
		 * No throttling...we can use the full budget everytime.
		 */
		*budget_pages_allowed = UINT64_MAX;
		return;
	}
#endif

	clock_get_system_nanotime(&sec, &nsec);
	ts.tv_sec = sec;
	ts.tv_nsec = nsec;

	struct throttle_interval_t *interval = NULL;

	if (memorystatus_freeze_degradation == TRUE) {
		interval = degraded_throttle_window;

		if (CMP_MACH_TIMESPEC(&ts, &interval->ts) >= 0) {
			memorystatus_freeze_degradation = FALSE;
			interval->pageouts = 0;
			interval->max_pageouts = 0;
		} else {
			*budget_pages_allowed = interval->max_pageouts - interval->pageouts;
		}
	}

	interval = normal_throttle_window;

	if (CMP_MACH_TIMESPEC(&ts, &interval->ts) >= 0) {
		/*
		 * New throttle window.
		 * Rollover any unused budget.
		 * Also ask the storage layer what the new budget needs to be.
		 */
		uint64_t freeze_daily_budget = 0;
		unsigned int daily_budget_pageouts = 0;

		if (vm_swap_max_budget(&freeze_daily_budget)) {
			memorystatus_freeze_daily_mb_max = (freeze_daily_budget / (1024 * 1024));
			os_log_with_startup_serial(OS_LOG_DEFAULT, "memorystatus: memorystatus_freeze_daily_mb_max set to %dMB\n", memorystatus_freeze_daily_mb_max);
		}

		freeze_daily_pageouts_max = memorystatus_freeze_daily_mb_max * (1024 * 1024 / PAGE_SIZE);

		daily_budget_pageouts =  (interval->burst_multiple * (((uint64_t)interval->mins * freeze_daily_pageouts_max) / NORMAL_WINDOW_MINS));
		interval->max_pageouts = (interval->max_pageouts - interval->pageouts) + daily_budget_pageouts;

		interval->ts.tv_sec = interval->mins * 60;
		interval->ts.tv_nsec = 0;
		ADD_MACH_TIMESPEC(&interval->ts, &ts);
		/* Since we update the throttle stats pre-freeze, adjust for overshoot here */
		if (interval->pageouts > interval->max_pageouts) {
			interval->pageouts -= interval->max_pageouts;
		} else {
			interval->pageouts = 0;
		}
		*budget_pages_allowed = interval->max_pageouts;

		memorystatus_demote_frozen_processes(FALSE); /* normal mode...don't force a demotion */
	} else {
		/*
		 * Current throttle window.
		 * Deny freezing if we have no budget left.
		 * Try graceful degradation if we are within 25% of:
		 * - the daily budget, and
		 * - the current budget left is below our normal budget expectations.
		 */

#if DEVELOPMENT || DEBUG
		/*
		 * This can only happen in the INTERNAL configs because we allow modifying the daily budget for testing.
		 */

		if (freeze_daily_pageouts_max > interval->max_pageouts) {
			/*
			 * We just bumped the daily budget. Re-evaluate our normal window params.
			 */
			interval->max_pageouts = (interval->burst_multiple * (((uint64_t)interval->mins * freeze_daily_pageouts_max) / NORMAL_WINDOW_MINS));
			memorystatus_freeze_degradation = FALSE; //we'll re-evaluate this below...
		}
#endif /* DEVELOPMENT || DEBUG */

		if (memorystatus_freeze_degradation == FALSE) {
			if (interval->pageouts >= interval->max_pageouts) {
				*budget_pages_allowed = 0;
			} else {
				int budget_left = interval->max_pageouts - interval->pageouts;
				int budget_threshold = (freeze_daily_pageouts_max * FREEZE_DEGRADATION_BUDGET_THRESHOLD) / 100;

				mach_timespec_t time_left = {0, 0};

				time_left.tv_sec = interval->ts.tv_sec;
				time_left.tv_nsec = 0;

				SUB_MACH_TIMESPEC(&time_left, &ts);

				if (budget_left <= budget_threshold) {
					/*
					 * For the current normal window, calculate how much we would pageout in a DEGRADED_WINDOW_MINS duration.
					 * And also calculate what we would pageout for the same DEGRADED_WINDOW_MINS duration if we had the full
					 * daily pageout budget.
					 */

					unsigned int current_budget_rate_allowed = ((budget_left / time_left.tv_sec) / 60) * DEGRADED_WINDOW_MINS;
					unsigned int normal_budget_rate_allowed = (freeze_daily_pageouts_max / NORMAL_WINDOW_MINS) * DEGRADED_WINDOW_MINS;

					/*
					 * The current rate of pageouts is below what we would expect for
					 * the normal rate i.e. we have below normal budget left and so...
					 */

					if (current_budget_rate_allowed < normal_budget_rate_allowed) {
						memorystatus_freeze_degradation = TRUE;
						degraded_throttle_window->max_pageouts = current_budget_rate_allowed;
						degraded_throttle_window->pageouts = 0;

						/*
						 * Switch over to the degraded throttle window so the budget
						 * doled out is based on that window.
						 */
						interval = degraded_throttle_window;
					}
				}

				*budget_pages_allowed = interval->max_pageouts - interval->pageouts;
			}
		}
	}

	MEMORYSTATUS_DEBUG(1, "memorystatus_freeze_update_throttle_interval: throttle updated - %d frozen (%d max) within %dm; %dm remaining; throttle %s\n",
	    interval->pageouts, interval->max_pageouts, interval->mins, (interval->ts.tv_sec - ts->tv_sec) / 60,
	    interval->throttle ? "on" : "off");
}

static void
memorystatus_freeze_thread(void *param __unused, wait_result_t wr __unused)
{
	static boolean_t memorystatus_freeze_swap_low = FALSE;

	lck_mtx_lock(&freezer_mutex);

	if (memorystatus_freeze_enabled) {
		if ((memorystatus_frozen_count < memorystatus_frozen_processes_max) ||
		    (memorystatus_refreeze_eligible_count >= MIN_THAW_REFREEZE_THRESHOLD)) {
			if (memorystatus_can_freeze(&memorystatus_freeze_swap_low)) {
				/* Only freeze if we've not exceeded our pageout budgets.*/
				memorystatus_freeze_update_throttle(&memorystatus_freeze_budget_pages_remaining);

				if (memorystatus_freeze_budget_pages_remaining) {
					memorystatus_freeze_top_process();
				} else {
					memorystatus_demote_frozen_processes(TRUE); /* urgent mode..force one demotion */
				}
			}
		}
	}

	/*
	 * We use memorystatus_apps_idle_delay_time because if/when we adopt aging for applications,
	 * it'll tie neatly into running the freezer once we age an application.
	 *
	 * Till then, it serves as a good interval that can be tuned via a sysctl too.
	 */
	memorystatus_freezer_thread_next_run_ts = mach_absolute_time() + memorystatus_apps_idle_delay_time;

	assert_wait((event_t) &memorystatus_freeze_wakeup, THREAD_UNINT);
	lck_mtx_unlock(&freezer_mutex);

	thread_block((thread_continue_t) memorystatus_freeze_thread);
}

boolean_t
memorystatus_freeze_thread_should_run(void)
{
	/*
	 * No freezer_mutex held here...see why near call-site
	 * within memorystatus_pages_update().
	 */

	boolean_t should_run = FALSE;

	if (memorystatus_freeze_enabled == FALSE) {
		goto out;
	}

	if (memorystatus_available_pages > memorystatus_freeze_threshold) {
		goto out;
	}

	if ((memorystatus_frozen_count >= memorystatus_frozen_processes_max) &&
	    (memorystatus_refreeze_eligible_count < MIN_THAW_REFREEZE_THRESHOLD)) {
		goto out;
	}

	if (memorystatus_frozen_shared_mb_max && (memorystatus_frozen_shared_mb >= memorystatus_frozen_shared_mb_max)) {
		goto out;
	}

	uint64_t curr_time = mach_absolute_time();

	if (curr_time < memorystatus_freezer_thread_next_run_ts) {
		goto out;
	}

	should_run = TRUE;

out:
	return should_run;
}

int
memorystatus_get_process_is_freezable(pid_t pid, int *is_freezable)
{
	proc_t p = PROC_NULL;

	if (pid == 0) {
		return EINVAL;
	}

	p = proc_find(pid);
	if (!p) {
		return ESRCH;
	}

	/*
	 * Only allow this on the current proc for now.
	 * We can check for privileges and allow targeting another process in the future.
	 */
	if (p != current_proc()) {
		proc_rele(p);
		return EPERM;
	}

	proc_list_lock();
	*is_freezable = ((p->p_memstat_state & P_MEMSTAT_FREEZE_DISABLED) ? 0 : 1);
	proc_rele_locked(p);
	proc_list_unlock();

	return 0;
}

int
memorystatus_set_process_is_freezable(pid_t pid, boolean_t is_freezable)
{
	proc_t p = PROC_NULL;

	if (pid == 0) {
		return EINVAL;
	}

	/*
	 * To enable freezable status, you need to be root or an entitlement.
	 */
	if (is_freezable &&
	    !kauth_cred_issuser(kauth_cred_get()) &&
	    !IOTaskHasEntitlement(current_task(), MEMORYSTATUS_ENTITLEMENT)) {
		return EPERM;
	}

	p = proc_find(pid);
	if (!p) {
		return ESRCH;
	}

	/*
	 * A process can change its own status. A coalition leader can
	 * change the status of coalition members.
	 */
	if (p != current_proc()) {
		coalition_t coal = task_get_coalition(proc_task(p), COALITION_TYPE_JETSAM);
		if (!coalition_is_leader(proc_task(current_proc()), coal)) {
			proc_rele(p);
			return EPERM;
		}
	}

	proc_list_lock();
	if (is_freezable == FALSE) {
		/* Freeze preference set to FALSE. Set the P_MEMSTAT_FREEZE_DISABLED bit. */
		p->p_memstat_state |= P_MEMSTAT_FREEZE_DISABLED;
		printf("memorystatus_set_process_is_freezable: disabling freeze for pid %d [%s]\n",
		    p->p_pid, (*p->p_name ? p->p_name : "unknown"));
	} else {
		p->p_memstat_state &= ~P_MEMSTAT_FREEZE_DISABLED;
		printf("memorystatus_set_process_is_freezable: enabling freeze for pid %d [%s]\n",
		    p->p_pid, (*p->p_name ? p->p_name : "unknown"));
	}
	proc_rele_locked(p);
	proc_list_unlock();

	return 0;
}

static int
sysctl_memorystatus_do_fastwake_warmup_all  SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)

	if (!req->newptr) {
		return EINVAL;
	}

	/* Need to be root or have entitlement */
	if (!kauth_cred_issuser(kauth_cred_get()) && !IOTaskHasEntitlement(current_task(), MEMORYSTATUS_ENTITLEMENT)) {
		return EPERM;
	}

	if (memorystatus_freeze_enabled == FALSE) {
		return ENOTSUP;
	}

	do_fastwake_warmup_all();

	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, memorystatus_do_fastwake_warmup_all, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, &sysctl_memorystatus_do_fastwake_warmup_all, "I", "");

#endif /* CONFIG_FREEZE */
