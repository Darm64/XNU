/*
 * Copyright (c) 2017-2019 Apple Inc. All rights reserved.
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
 */
/*
 *	File:	arm/cpu_common.c
 *
 *	cpu routines common to all supported arm variants
 */

#include <kern/kalloc.h>
#include <kern/machine.h>
#include <kern/cpu_number.h>
#include <kern/thread.h>
#include <kern/timer_queue.h>
#include <arm/cpu_data.h>
#include <arm/cpuid.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/misc_protos.h>
#include <arm/machine_cpu.h>
#include <arm/rtclock.h>
#include <mach/processor_info.h>
#include <machine/atomic.h>
#include <machine/config.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <pexpert/arm/protos.h>
#include <pexpert/device_tree.h>
#include <sys/kdebug.h>
#include <arm/machine_routines.h>
#include <libkern/OSAtomic.h>

#if KPERF
void kperf_signal_handler(unsigned int cpu_number);
#endif

cpu_data_t BootCpuData;
cpu_data_entry_t CpuDataEntries[MAX_CPUS];

struct processor BootProcessor;

unsigned int    real_ncpus = 1;
boolean_t       idle_enable = FALSE;
uint64_t        wake_abstime = 0x0ULL;

#if defined(HAS_IPI)
extern unsigned int gFastIPI;
#endif /* defined(HAS_IPI) */

cpu_data_t *
cpu_datap(int cpu)
{
	assert(cpu < MAX_CPUS);
	return CpuDataEntries[cpu].cpu_data_vaddr;
}

kern_return_t
cpu_control(int slot_num,
    processor_info_t info,
    unsigned int count)
{
	printf("cpu_control(%d,%p,%d) not implemented\n",
	    slot_num, info, count);
	return KERN_FAILURE;
}

kern_return_t
cpu_info_count(processor_flavor_t flavor,
    unsigned int *count)
{
	switch (flavor) {
	case PROCESSOR_CPU_STAT:
		*count = PROCESSOR_CPU_STAT_COUNT;
		return KERN_SUCCESS;

	case PROCESSOR_CPU_STAT64:
		*count = PROCESSOR_CPU_STAT64_COUNT;
		return KERN_SUCCESS;

	default:
		*count = 0;
		return KERN_FAILURE;
	}
}

kern_return_t
cpu_info(processor_flavor_t flavor, int slot_num, processor_info_t info,
    unsigned int *count)
{
	cpu_data_t *cpu_data_ptr = CpuDataEntries[slot_num].cpu_data_vaddr;

	switch (flavor) {
	case PROCESSOR_CPU_STAT:
	{
		if (*count < PROCESSOR_CPU_STAT_COUNT) {
			return KERN_FAILURE;
		}

		processor_cpu_stat_t cpu_stat = (processor_cpu_stat_t)info;
		cpu_stat->irq_ex_cnt = (uint32_t)cpu_data_ptr->cpu_stat.irq_ex_cnt;
		cpu_stat->ipi_cnt = (uint32_t)cpu_data_ptr->cpu_stat.ipi_cnt;
		cpu_stat->timer_cnt = (uint32_t)cpu_data_ptr->cpu_stat.timer_cnt;
		cpu_stat->undef_ex_cnt = (uint32_t)cpu_data_ptr->cpu_stat.undef_ex_cnt;
		cpu_stat->unaligned_cnt = (uint32_t)cpu_data_ptr->cpu_stat.unaligned_cnt;
		cpu_stat->vfp_cnt = (uint32_t)cpu_data_ptr->cpu_stat.vfp_cnt;
		cpu_stat->vfp_shortv_cnt = 0;
		cpu_stat->data_ex_cnt = (uint32_t)cpu_data_ptr->cpu_stat.data_ex_cnt;
		cpu_stat->instr_ex_cnt = (uint32_t)cpu_data_ptr->cpu_stat.instr_ex_cnt;

		*count = PROCESSOR_CPU_STAT_COUNT;

		return KERN_SUCCESS;
	}

	case PROCESSOR_CPU_STAT64:
	{
		if (*count < PROCESSOR_CPU_STAT64_COUNT) {
			return KERN_FAILURE;
		}

		processor_cpu_stat64_t cpu_stat = (processor_cpu_stat64_t)info;
		cpu_stat->irq_ex_cnt = cpu_data_ptr->cpu_stat.irq_ex_cnt;
		cpu_stat->ipi_cnt = cpu_data_ptr->cpu_stat.ipi_cnt;
		cpu_stat->timer_cnt = cpu_data_ptr->cpu_stat.timer_cnt;
		cpu_stat->undef_ex_cnt = cpu_data_ptr->cpu_stat.undef_ex_cnt;
		cpu_stat->unaligned_cnt = cpu_data_ptr->cpu_stat.unaligned_cnt;
		cpu_stat->vfp_cnt = cpu_data_ptr->cpu_stat.vfp_cnt;
		cpu_stat->vfp_shortv_cnt = 0;
		cpu_stat->data_ex_cnt = cpu_data_ptr->cpu_stat.data_ex_cnt;
		cpu_stat->instr_ex_cnt = cpu_data_ptr->cpu_stat.instr_ex_cnt;
#if MONOTONIC
		cpu_stat->pmi_cnt = cpu_data_ptr->cpu_monotonic.mtc_npmis;
#endif /* MONOTONIC */

		*count = PROCESSOR_CPU_STAT64_COUNT;

		return KERN_SUCCESS;
	}

	default:
		return KERN_FAILURE;
	}
}

/*
 *	Routine:	cpu_doshutdown
 *	Function:
 */
void
cpu_doshutdown(void (*doshutdown)(processor_t),
    processor_t processor)
{
	doshutdown(processor);
}

/*
 *	Routine:	cpu_idle_tickle
 *
 */
void
cpu_idle_tickle(void)
{
	boolean_t       intr;
	cpu_data_t      *cpu_data_ptr;
	uint64_t        new_idle_timeout_ticks = 0x0ULL;

	intr = ml_set_interrupts_enabled(FALSE);
	cpu_data_ptr = getCpuDatap();

	if (cpu_data_ptr->idle_timer_notify != (void *)NULL) {
		((idle_timer_t)cpu_data_ptr->idle_timer_notify)(cpu_data_ptr->idle_timer_refcon, &new_idle_timeout_ticks);
		if (new_idle_timeout_ticks != 0x0ULL) {
			/* if a new idle timeout was requested set the new idle timer deadline */
			clock_absolutetime_interval_to_deadline(new_idle_timeout_ticks, &cpu_data_ptr->idle_timer_deadline);
		} else {
			/* turn off the idle timer */
			cpu_data_ptr->idle_timer_deadline = 0x0ULL;
		}
		timer_resync_deadlines();
	}
	(void) ml_set_interrupts_enabled(intr);
}

static void
cpu_handle_xcall(cpu_data_t *cpu_data_ptr)
{
	broadcastFunc   xfunc;
	void            *xparam;

	os_atomic_thread_fence(acquire);
	/* Come back around if cpu_signal_internal is running on another CPU and has just
	* added SIGPxcall to the pending mask, but hasn't yet assigned the call params.*/
	if (cpu_data_ptr->cpu_xcall_p0 != NULL && cpu_data_ptr->cpu_xcall_p1 != NULL) {
		xfunc = cpu_data_ptr->cpu_xcall_p0;
		xparam = cpu_data_ptr->cpu_xcall_p1;
		cpu_data_ptr->cpu_xcall_p0 = NULL;
		cpu_data_ptr->cpu_xcall_p1 = NULL;
		os_atomic_thread_fence(acq_rel);
		os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPxcall, relaxed);
		xfunc(xparam);
	}
	if (cpu_data_ptr->cpu_imm_xcall_p0 != NULL && cpu_data_ptr->cpu_imm_xcall_p1 != NULL) {
		xfunc = cpu_data_ptr->cpu_imm_xcall_p0;
		xparam = cpu_data_ptr->cpu_imm_xcall_p1;
		cpu_data_ptr->cpu_imm_xcall_p0 = NULL;
		cpu_data_ptr->cpu_imm_xcall_p1 = NULL;
		os_atomic_thread_fence(acq_rel);
		os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPxcallImm, relaxed);
		xfunc(xparam);
	}
}

static unsigned int
cpu_broadcast_xcall_internal(unsigned int signal,
    uint32_t *synch,
    boolean_t self_xcall,
    broadcastFunc func,
    void *parm)
{
	boolean_t       intr;
	cpu_data_t      *cpu_data_ptr;
	cpu_data_t      *target_cpu_datap;
	unsigned int    failsig;
	int             cpu;
	int             max_cpu = ml_get_max_cpu_number() + 1;

	intr = ml_set_interrupts_enabled(FALSE);
	cpu_data_ptr = getCpuDatap();

	failsig = 0;

	if (synch != NULL) {
		*synch = max_cpu;
		assert_wait((event_t)synch, THREAD_UNINT);
	}

	for (cpu = 0; cpu < max_cpu; cpu++) {
		target_cpu_datap = (cpu_data_t *)CpuDataEntries[cpu].cpu_data_vaddr;

		if (target_cpu_datap == cpu_data_ptr) {
			continue;
		}

		if ((target_cpu_datap == NULL) ||
		    KERN_SUCCESS != cpu_signal(target_cpu_datap, signal, (void *)func, parm)) {
			failsig++;
		}
	}


	if (self_xcall) {
		func(parm);
	}

	(void) ml_set_interrupts_enabled(intr);

	if (synch != NULL) {
		if (os_atomic_sub(synch, (!self_xcall) ? failsig + 1 : failsig, relaxed) == 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
	}

	if (!self_xcall) {
		return max_cpu - failsig - 1;
	} else {
		return max_cpu - failsig;
	}
}

unsigned int
cpu_broadcast_xcall(uint32_t *synch,
    boolean_t self_xcall,
    broadcastFunc func,
    void *parm)
{
	return cpu_broadcast_xcall_internal(SIGPxcall, synch, self_xcall, func, parm);
}

unsigned int
cpu_broadcast_immediate_xcall(uint32_t *synch,
    boolean_t self_xcall,
    broadcastFunc func,
    void *parm)
{
	return cpu_broadcast_xcall_internal(SIGPxcallImm, synch, self_xcall, func, parm);
}

static kern_return_t
cpu_xcall_internal(unsigned int signal, int cpu_number, broadcastFunc func, void *param)
{
	cpu_data_t      *target_cpu_datap;

	if ((cpu_number < 0) || (cpu_number > ml_get_max_cpu_number())) {
		return KERN_INVALID_ARGUMENT;
	}

	if (func == NULL || param == NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	target_cpu_datap = (cpu_data_t*)CpuDataEntries[cpu_number].cpu_data_vaddr;
	if (target_cpu_datap == NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	return cpu_signal(target_cpu_datap, signal, (void*)func, param);
}

kern_return_t
cpu_xcall(int cpu_number, broadcastFunc func, void *param)
{
	return cpu_xcall_internal(SIGPxcall, cpu_number, func, param);
}

kern_return_t
cpu_immediate_xcall(int cpu_number, broadcastFunc func, void *param)
{
	return cpu_xcall_internal(SIGPxcallImm, cpu_number, func, param);
}

static kern_return_t
cpu_signal_internal(cpu_data_t *target_proc,
    unsigned int signal,
    void *p0,
    void *p1,
    boolean_t defer)
{
	unsigned int    Check_SIGPdisabled;
	int             current_signals;
	Boolean         swap_success;
	boolean_t       interruptible = ml_set_interrupts_enabled(FALSE);
	cpu_data_t      *current_proc = getCpuDatap();

	/* We'll mandate that only IPIs meant to kick a core out of idle may ever be deferred. */
	if (defer) {
		assert(signal == SIGPnop);
	}

	if (current_proc != target_proc) {
		Check_SIGPdisabled = SIGPdisabled;
	} else {
		Check_SIGPdisabled = 0;
	}

	if ((signal == SIGPxcall) || (signal == SIGPxcallImm)) {
		do {
			current_signals = target_proc->cpu_signal;
			if ((current_signals & SIGPdisabled) == SIGPdisabled) {
				ml_set_interrupts_enabled(interruptible);
				return KERN_FAILURE;
			}
			swap_success = OSCompareAndSwap(current_signals & (~signal), current_signals | signal,
			    &target_proc->cpu_signal);

			if (!swap_success && (signal == SIGPxcallImm) && (target_proc->cpu_signal & SIGPxcallImm)) {
				ml_set_interrupts_enabled(interruptible);
				return KERN_ALREADY_WAITING;
			}

			/* Drain pending xcalls on this cpu; the CPU we're trying to xcall may in turn
			 * be trying to xcall us.  Since we have interrupts disabled that can deadlock,
			 * so break the deadlock by draining pending xcalls. */
			if (!swap_success && (current_proc->cpu_signal & signal)) {
				cpu_handle_xcall(current_proc);
			}
		} while (!swap_success);

		if (signal == SIGPxcallImm) {
			target_proc->cpu_imm_xcall_p0 = p0;
			target_proc->cpu_imm_xcall_p1 = p1;
		} else {
			target_proc->cpu_xcall_p0 = p0;
			target_proc->cpu_xcall_p1 = p1;
		}
	} else {
		do {
			current_signals = target_proc->cpu_signal;
			if ((Check_SIGPdisabled != 0) && (current_signals & Check_SIGPdisabled) == SIGPdisabled) {
				ml_set_interrupts_enabled(interruptible);
				return KERN_FAILURE;
			}

			swap_success = OSCompareAndSwap(current_signals, current_signals | signal,
			    &target_proc->cpu_signal);
		} while (!swap_success);
	}

	/*
	 * Issue DSB here to guarantee: 1) prior stores to pending signal mask and xcall params
	 * will be visible to other cores when the IPI is dispatched, and 2) subsequent
	 * instructions to signal the other cores will not execute until after the barrier.
	 * DMB would be sufficient to guarantee 1) but not 2).
	 */
	__builtin_arm_dsb(DSB_ISH);

	if (!(target_proc->cpu_signal & SIGPdisabled)) {
		if (defer) {
#if defined(HAS_IPI)
			if (gFastIPI) {
				ml_cpu_signal_deferred(target_proc->cpu_phys_id);
			} else {
				PE_cpu_signal_deferred(getCpuDatap()->cpu_id, target_proc->cpu_id);
			}
#else
			PE_cpu_signal_deferred(getCpuDatap()->cpu_id, target_proc->cpu_id);
#endif /* defined(HAS_IPI) */
		} else {
#if defined(HAS_IPI)
			if (gFastIPI) {
				ml_cpu_signal(target_proc->cpu_phys_id);
			} else {
				PE_cpu_signal(getCpuDatap()->cpu_id, target_proc->cpu_id);
			}
#else
			PE_cpu_signal(getCpuDatap()->cpu_id, target_proc->cpu_id);
#endif /* defined(HAS_IPI) */
		}
	}

	ml_set_interrupts_enabled(interruptible);
	return KERN_SUCCESS;
}

kern_return_t
cpu_signal(cpu_data_t *target_proc,
    unsigned int signal,
    void *p0,
    void *p1)
{
	return cpu_signal_internal(target_proc, signal, p0, p1, FALSE);
}

kern_return_t
cpu_signal_deferred(cpu_data_t *target_proc)
{
	return cpu_signal_internal(target_proc, SIGPnop, NULL, NULL, TRUE);
}

void
cpu_signal_cancel(cpu_data_t *target_proc)
{
	/* TODO: Should we care about the state of a core as far as squashing deferred IPIs goes? */
	if (!(target_proc->cpu_signal & SIGPdisabled)) {
#if defined(HAS_IPI)
		if (gFastIPI) {
			ml_cpu_signal_retract(target_proc->cpu_phys_id);
		} else {
			PE_cpu_signal_cancel(getCpuDatap()->cpu_id, target_proc->cpu_id);
		}
#else
		PE_cpu_signal_cancel(getCpuDatap()->cpu_id, target_proc->cpu_id);
#endif /* defined(HAS_IPI) */
	}
}

void
cpu_signal_handler(void)
{
	cpu_signal_handler_internal(FALSE);
}

void
cpu_signal_handler_internal(boolean_t disable_signal)
{
	cpu_data_t     *cpu_data_ptr = getCpuDatap();
	unsigned int    cpu_signal;


	cpu_data_ptr->cpu_stat.ipi_cnt++;
	cpu_data_ptr->cpu_stat.ipi_cnt_wake++;

	SCHED_STATS_IPI(current_processor());

	cpu_signal = os_atomic_or(&cpu_data_ptr->cpu_signal, 0, relaxed);

	if ((!(cpu_signal & SIGPdisabled)) && (disable_signal == TRUE)) {
		os_atomic_or(&cpu_data_ptr->cpu_signal, SIGPdisabled, relaxed);
	} else if ((cpu_signal & SIGPdisabled) && (disable_signal == FALSE)) {
		os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPdisabled, relaxed);
	}

	while (cpu_signal & ~SIGPdisabled) {
		if (cpu_signal & SIGPdec) {
			os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPdec, relaxed);
			rtclock_intr(FALSE);
		}
#if KPERF
		if (cpu_signal & SIGPkptimer) {
			os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPkptimer, relaxed);
			kperf_signal_handler((unsigned int)cpu_data_ptr->cpu_number);
		}
#endif
		if (cpu_signal & (SIGPxcall | SIGPxcallImm)) {
			cpu_handle_xcall(cpu_data_ptr);
		}
		if (cpu_signal & SIGPast) {
			os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPast, relaxed);
			ast_check(cpu_data_ptr->cpu_processor);
		}
		if (cpu_signal & SIGPdebug) {
			os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPdebug, relaxed);
			DebuggerXCall(cpu_data_ptr->cpu_int_state);
		}
#if     __ARM_SMP__ && defined(ARMA7)
		if (cpu_signal & SIGPLWFlush) {
			os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPLWFlush, relaxed);
			cache_xcall_handler(LWFlush);
		}
		if (cpu_signal & SIGPLWClean) {
			os_atomic_andnot(&cpu_data_ptr->cpu_signal, SIGPLWClean, relaxed);
			cache_xcall_handler(LWClean);
		}
#endif

		cpu_signal = os_atomic_or(&cpu_data_ptr->cpu_signal, 0, relaxed);
	}
}

void
cpu_exit_wait(int cpu)
{
	if (cpu != master_cpu) {
		cpu_data_t      *cpu_data_ptr;

		cpu_data_ptr = CpuDataEntries[cpu].cpu_data_vaddr;
		while (!((*(volatile unsigned int*)&cpu_data_ptr->cpu_sleep_token) == ARM_CPU_ON_SLEEP_PATH)) {
		}
		;
	}
}

boolean_t
cpu_can_exit(__unused int cpu)
{
	return TRUE;
}

void
cpu_machine_init(void)
{
	static boolean_t started = FALSE;
	cpu_data_t      *cpu_data_ptr;

	cpu_data_ptr = getCpuDatap();
	started = ((cpu_data_ptr->cpu_flags & StartedState) == StartedState);
	if (cpu_data_ptr->cpu_cache_dispatch != (cache_dispatch_t) NULL) {
		platform_cache_init();
	}

	/* Note: this calls IOCPURunPlatformActiveActions when resuming on boot cpu */
	PE_cpu_machine_init(cpu_data_ptr->cpu_id, !started);

	cpu_data_ptr->cpu_flags |= StartedState;
	ml_init_interrupt();
}

processor_t
cpu_processor_alloc(boolean_t is_boot_cpu)
{
	processor_t proc;

	if (is_boot_cpu) {
		return &BootProcessor;
	}

	proc = kalloc(sizeof(*proc));
	if (!proc) {
		return NULL;
	}

	bzero((void *) proc, sizeof(*proc));
	return proc;
}

void
cpu_processor_free(processor_t proc)
{
	if (proc != NULL && proc != &BootProcessor) {
		kfree(proc, sizeof(*proc));
	}
}

processor_t
current_processor(void)
{
	return getCpuDatap()->cpu_processor;
}

processor_t
cpu_to_processor(int cpu)
{
	cpu_data_t *cpu_data = cpu_datap(cpu);
	if (cpu_data != NULL) {
		return cpu_data->cpu_processor;
	} else {
		return NULL;
	}
}

cpu_data_t *
processor_to_cpu_datap(processor_t processor)
{
	cpu_data_t *target_cpu_datap;

	assert(processor->cpu_id < MAX_CPUS);
	assert(CpuDataEntries[processor->cpu_id].cpu_data_vaddr != NULL);

	target_cpu_datap = (cpu_data_t*)CpuDataEntries[processor->cpu_id].cpu_data_vaddr;
	assert(target_cpu_datap->cpu_processor == processor);

	return target_cpu_datap;
}

cpu_data_t *
cpu_data_alloc(boolean_t is_boot_cpu)
{
	cpu_data_t              *cpu_data_ptr = NULL;

	if (is_boot_cpu) {
		cpu_data_ptr = &BootCpuData;
	} else {
		if ((kmem_alloc(kernel_map, (vm_offset_t *)&cpu_data_ptr, sizeof(cpu_data_t), VM_KERN_MEMORY_CPU)) != KERN_SUCCESS) {
			goto cpu_data_alloc_error;
		}

		bzero((void *)cpu_data_ptr, sizeof(cpu_data_t));

		cpu_stack_alloc(cpu_data_ptr);
	}

	cpu_data_ptr->cpu_processor = cpu_processor_alloc(is_boot_cpu);
	if (cpu_data_ptr->cpu_processor == (struct processor *)NULL) {
		goto cpu_data_alloc_error;
	}

	return cpu_data_ptr;

cpu_data_alloc_error:
	panic("cpu_data_alloc() failed\n");
	return (cpu_data_t *)NULL;
}

ast_t *
ast_pending(void)
{
	return &getCpuDatap()->cpu_pending_ast;
}

cpu_type_t
slot_type(int slot_num)
{
	return cpu_datap(slot_num)->cpu_type;
}

cpu_subtype_t
slot_subtype(int slot_num)
{
	return cpu_datap(slot_num)->cpu_subtype;
}

cpu_threadtype_t
slot_threadtype(int slot_num)
{
	return cpu_datap(slot_num)->cpu_threadtype;
}

cpu_type_t
cpu_type(void)
{
	return getCpuDatap()->cpu_type;
}

cpu_subtype_t
cpu_subtype(void)
{
	return getCpuDatap()->cpu_subtype;
}

cpu_threadtype_t
cpu_threadtype(void)
{
	return getCpuDatap()->cpu_threadtype;
}

int
cpu_number(void)
{
	return getCpuDatap()->cpu_number;
}

uint64_t
ml_get_wake_timebase(void)
{
	return wake_abstime;
}
