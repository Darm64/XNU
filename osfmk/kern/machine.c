/*
 * Copyright (c) 2000-2019 Apple Inc. All rights reserved.
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
 * @OSF_COPYRIGHT@
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	kern/machine.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1987
 *
 *	Support for machine independent machine abstraction.
 */

#include <string.h>

#include <mach/mach_types.h>
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/machine.h>
#include <mach/host_info.h>
#include <mach/host_reboot.h>
#include <mach/host_priv_server.h>
#include <mach/processor_server.h>

#include <kern/kern_types.h>
#include <kern/counters.h>
#include <kern/cpu_data.h>
#include <kern/cpu_quiesce.h>
#include <kern/ipc_host.h>
#include <kern/host.h>
#include <kern/machine.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/queue.h>
#include <kern/sched.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <machine/commpage.h>

#if HIBERNATION
#include <IOKit/IOHibernatePrivate.h>
#endif
#include <IOKit/IOPlatformExpert.h>

#if CONFIG_DTRACE
extern void (*dtrace_cpu_state_changed_hook)(int, boolean_t);
#endif

#if defined(__x86_64__)
#include <i386/misc_protos.h>
#include <libkern/OSDebug.h>
#endif

/*
 *	Exported variables:
 */

struct machine_info     machine_info;

/* Forwards */
static void
processor_doshutdown(processor_t processor);

static void
processor_offline(void * parameter, __unused wait_result_t result);

static void
processor_offline_intstack(processor_t processor) __dead2;

/*
 *	processor_up:
 *
 *	Flag processor as up and running, and available
 *	for scheduling.
 */
void
processor_up(
	processor_t                     processor)
{
	processor_set_t         pset;
	spl_t                           s;
	boolean_t pset_online = false;

	s = splsched();
	init_ast_check(processor);
	pset = processor->processor_set;
	pset_lock(pset);
	if (pset->online_processor_count == 0) {
		/* About to bring the first processor of a pset online */
		pset_online = true;
	}
	++pset->online_processor_count;
	pset_update_processor_state(pset, processor, PROCESSOR_RUNNING);
	os_atomic_inc(&processor_avail_count, relaxed);
	if (processor->is_recommended) {
		os_atomic_inc(&processor_avail_count_user, relaxed);
	}
	commpage_update_active_cpus();
	if (pset_online) {
		/* New pset is coming up online; callout to the
		 * scheduler in case it wants to adjust runqs.
		 */
		SCHED(pset_made_schedulable)(processor, pset, true);
		/* pset lock dropped */
	} else {
		pset_unlock(pset);
	}
	ml_cpu_up();
	splx(s);

#if CONFIG_DTRACE
	if (dtrace_cpu_state_changed_hook) {
		(*dtrace_cpu_state_changed_hook)(processor->cpu_id, TRUE);
	}
#endif
}
#include <atm/atm_internal.h>

kern_return_t
host_reboot(
	host_priv_t             host_priv,
	int                             options)
{
	if (host_priv == HOST_PRIV_NULL) {
		return KERN_INVALID_HOST;
	}

	assert(host_priv == &realhost);

#if DEVELOPMENT || DEBUG
	if (options & HOST_REBOOT_DEBUGGER) {
		Debugger("Debugger");
		return KERN_SUCCESS;
	}
#endif

	if (options & HOST_REBOOT_UPSDELAY) {
		// UPS power cutoff path
		PEHaltRestart( kPEUPSDelayHaltCPU );
	} else {
		halt_all_cpus(!(options & HOST_REBOOT_HALT));
	}

	return KERN_SUCCESS;
}

kern_return_t
processor_assign(
	__unused processor_t            processor,
	__unused processor_set_t        new_pset,
	__unused boolean_t              wait)
{
	return KERN_FAILURE;
}

kern_return_t
processor_shutdown(
	processor_t                     processor)
{
	processor_set_t         pset;
	spl_t                           s;

	s = splsched();
	pset = processor->processor_set;
	pset_lock(pset);
	if (processor->state == PROCESSOR_OFF_LINE) {
		/*
		 * Success if already shutdown.
		 */
		pset_unlock(pset);
		splx(s);

		return KERN_SUCCESS;
	}

	if (processor->state == PROCESSOR_START) {
		/*
		 * Failure if currently being started.
		 */
		pset_unlock(pset);
		splx(s);

		return KERN_FAILURE;
	}

	/*
	 * If the processor is dispatching, let it finish.
	 */
	while (processor->state == PROCESSOR_DISPATCHING) {
		pset_unlock(pset);
		splx(s);
		delay(1);
		s = splsched();
		pset_lock(pset);
	}

	/*
	 * Success if already being shutdown.
	 */
	if (processor->state == PROCESSOR_SHUTDOWN) {
		pset_unlock(pset);
		splx(s);

		return KERN_SUCCESS;
	}

	pset_update_processor_state(pset, processor, PROCESSOR_SHUTDOWN);
	pset_unlock(pset);

	processor_doshutdown(processor);
	splx(s);

	cpu_exit_wait(processor->cpu_id);

	return KERN_SUCCESS;
}

/*
 * Called with interrupts disabled.
 */
static void
processor_doshutdown(
	processor_t processor)
{
	thread_t self = current_thread();

	/*
	 *	Get onto the processor to shutdown
	 */
	processor_t prev = thread_bind(processor);
	thread_block(THREAD_CONTINUE_NULL);

	/* interrupts still disabled */
	assert(ml_get_interrupts_enabled() == FALSE);

	assert(processor == current_processor());
	assert(processor->state == PROCESSOR_SHUTDOWN);

#if CONFIG_DTRACE
	if (dtrace_cpu_state_changed_hook) {
		(*dtrace_cpu_state_changed_hook)(processor->cpu_id, FALSE);
	}
#endif

	ml_cpu_down();

#if HIBERNATION
	if (processor_avail_count < 2) {
		hibernate_vm_lock();
		hibernate_vm_unlock();
	}
#endif

	processor_set_t pset = processor->processor_set;

	pset_lock(pset);
	pset_update_processor_state(pset, processor, PROCESSOR_OFF_LINE);
	--pset->online_processor_count;
	os_atomic_dec(&processor_avail_count, relaxed);
	if (processor->is_recommended) {
		os_atomic_dec(&processor_avail_count_user, relaxed);
	}
	commpage_update_active_cpus();
	SCHED(processor_queue_shutdown)(processor);
	/* pset lock dropped */
	SCHED(rt_queue_shutdown)(processor);

	thread_bind(prev);

	/* interrupts still disabled */

	/*
	 * Continue processor shutdown on the processor's idle thread.
	 * The handoff won't fail because the idle thread has a reserved stack.
	 * Switching to the idle thread leaves interrupts disabled,
	 * so we can't accidentally take an interrupt after the context switch.
	 */
	thread_t shutdown_thread = processor->idle_thread;
	shutdown_thread->continuation = processor_offline;
	shutdown_thread->parameter = processor;

	thread_run(self, NULL, NULL, shutdown_thread);
}

/*
 * Called in the context of the idle thread to shut down the processor
 *
 * A shut-down processor looks like it's 'running' the idle thread parked
 * in this routine, but it's actually been powered off and has no hardware state.
 */
static void
processor_offline(
	void * parameter,
	__unused wait_result_t result)
{
	processor_t processor = (processor_t) parameter;
	thread_t self = current_thread();
	__assert_only thread_t old_thread = THREAD_NULL;

	assert(processor == current_processor());
	assert(self->state & TH_IDLE);
	assert(processor->idle_thread == self);
	assert(ml_get_interrupts_enabled() == FALSE);
	assert(self->continuation == NULL);
	assert(processor->processor_offlined == false);

	bool enforce_quiesce_safety = gEnforceQuiesceSafety;

	/*
	 * Scheduling is now disabled for this processor.
	 * Ensure that primitives that need scheduling (like mutexes) know this.
	 */
	if (enforce_quiesce_safety) {
		disable_preemption();
	}

	/* convince slave_main to come back here */
	processor->processor_offlined = true;

	/*
	 * Switch to the interrupt stack and shut down the processor.
	 *
	 * When the processor comes back, it will eventually call load_context which
	 * restores the context saved by machine_processor_shutdown, returning here.
	 */
	old_thread = machine_processor_shutdown(self, processor_offline_intstack, processor);

	/* old_thread should be NULL because we got here through Load_context */
	assert(old_thread == THREAD_NULL);

	assert(processor == current_processor());
	assert(processor->idle_thread == current_thread());

	assert(ml_get_interrupts_enabled() == FALSE);
	assert(self->continuation == NULL);

	/* Extract the machine_param value stashed by slave_main */
	void * machine_param = self->parameter;
	self->parameter = NULL;

	/* Re-initialize the processor */
	slave_machine_init(machine_param);

	assert(processor->processor_offlined == true);
	processor->processor_offlined = false;

	if (enforce_quiesce_safety) {
		enable_preemption();
	}

	/*
	 * Now that the processor is back, invoke the idle thread to find out what to do next.
	 * idle_thread will enable interrupts.
	 */
	thread_block(idle_thread);
	/*NOTREACHED*/
}

/*
 * Complete the shutdown and place the processor offline.
 *
 * Called at splsched in the shutdown context
 * (i.e. on the idle thread, on the interrupt stack)
 *
 * The onlining half of this is done in load_context().
 */
static void
processor_offline_intstack(
	processor_t processor)
{
	assert(processor == current_processor());
	assert(processor->active_thread == current_thread());

	timer_stop(PROCESSOR_DATA(processor, current_state), processor->last_dispatch);

	cpu_quiescent_counter_leave(processor->last_dispatch);

	PMAP_DEACTIVATE_KERNEL(processor->cpu_id);

	cpu_sleep();
	panic("zombie processor");
	/*NOTREACHED*/
}

kern_return_t
host_get_boot_info(
	host_priv_t         host_priv,
	kernel_boot_info_t  boot_info)
{
	const char *src = "";
	if (host_priv == HOST_PRIV_NULL) {
		return KERN_INVALID_HOST;
	}

	assert(host_priv == &realhost);

	/*
	 * Copy first operator string terminated by '\0' followed by
	 *	standardized strings generated from boot string.
	 */
	src = machine_boot_info(boot_info, KERNEL_BOOT_INFO_MAX);
	if (src != boot_info) {
		(void) strncpy(boot_info, src, KERNEL_BOOT_INFO_MAX);
	}

	return KERN_SUCCESS;
}

#if CONFIG_DTRACE
#include <mach/sdt.h>
#endif

unsigned long long
ml_io_read(uintptr_t vaddr, int size)
{
	unsigned long long result = 0;
	unsigned char s1;
	unsigned short s2;

#if defined(__x86_64__)
	uint64_t sabs, eabs;
	boolean_t istate, timeread = FALSE;
#if DEVELOPMENT || DEBUG
	extern uint64_t simulate_stretched_io;
	uintptr_t paddr = pmap_verify_noncacheable(vaddr);
#endif /* x86_64 DEVELOPMENT || DEBUG */
	if (__improbable(reportphyreaddelayabs != 0)) {
		istate = ml_set_interrupts_enabled(FALSE);
		sabs = mach_absolute_time();
		timeread = TRUE;
	}

#if DEVELOPMENT || DEBUG
	if (__improbable(timeread && simulate_stretched_io)) {
		sabs -= simulate_stretched_io;
	}
#endif /* x86_64 DEVELOPMENT || DEBUG */

#endif /* x86_64 */

	switch (size) {
	case 1:
		s1 = *(volatile unsigned char *)vaddr;
		result = s1;
		break;
	case 2:
		s2 = *(volatile unsigned short *)vaddr;
		result = s2;
		break;
	case 4:
		result = *(volatile unsigned int *)vaddr;
		break;
	case 8:
		result = *(volatile unsigned long long *)vaddr;
		break;
	default:
		panic("Invalid size %d for ml_io_read(%p)", size, (void *)vaddr);
		break;
	}

#if defined(__x86_64__)
	if (__improbable(timeread == TRUE)) {
		eabs = mach_absolute_time();

#if DEVELOPMENT || DEBUG
		iotrace(IOTRACE_IO_READ, vaddr, paddr, size, result, sabs, eabs - sabs);
#endif

		if (__improbable((eabs - sabs) > reportphyreaddelayabs)) {
#if !(DEVELOPMENT || DEBUG)
			uintptr_t paddr = kvtophys(vaddr);
#endif

			(void)ml_set_interrupts_enabled(istate);

			if (phyreadpanic && (machine_timeout_suspended() == FALSE)) {
				panic_io_port_read();
				panic("Read from IO vaddr 0x%lx paddr 0x%lx took %llu ns, "
				    "result: 0x%llx (start: %llu, end: %llu), ceiling: %llu",
				    vaddr, paddr, (eabs - sabs), result, sabs, eabs,
				    reportphyreaddelayabs);
			}

			if (reportphyreadosbt) {
				OSReportWithBacktrace("ml_io_read(v=%p, p=%p) size %d result 0x%llx "
				    "took %lluus",
				    (void *)vaddr, (void *)paddr, size, result,
				    (eabs - sabs) / NSEC_PER_USEC);
			}
#if CONFIG_DTRACE
			DTRACE_PHYSLAT5(physioread, uint64_t, (eabs - sabs),
			    uint64_t, vaddr, uint32_t, size, uint64_t, paddr, uint64_t, result);
#endif /* CONFIG_DTRACE */
		} else if (__improbable(tracephyreaddelayabs > 0 && (eabs - sabs) > tracephyreaddelayabs)) {
#if !(DEVELOPMENT || DEBUG)
			uintptr_t paddr = kvtophys(vaddr);
#endif

			KDBG(MACHDBG_CODE(DBG_MACH_IO, DBC_MACH_IO_MMIO_READ),
			    (eabs - sabs), VM_KERNEL_UNSLIDE_OR_PERM(vaddr), paddr, result);

			(void)ml_set_interrupts_enabled(istate);
		} else {
			(void)ml_set_interrupts_enabled(istate);
		}
	}
#endif /* x86_64 */
	return result;
}

unsigned int
ml_io_read8(uintptr_t vaddr)
{
	return (unsigned) ml_io_read(vaddr, 1);
}

unsigned int
ml_io_read16(uintptr_t vaddr)
{
	return (unsigned) ml_io_read(vaddr, 2);
}

unsigned int
ml_io_read32(uintptr_t vaddr)
{
	return (unsigned) ml_io_read(vaddr, 4);
}

unsigned long long
ml_io_read64(uintptr_t vaddr)
{
	return ml_io_read(vaddr, 8);
}

/* ml_io_write* */

void
ml_io_write(uintptr_t vaddr, uint64_t val, int size)
{
#if defined(__x86_64__)
	uint64_t sabs, eabs;
	boolean_t istate, timewrite = FALSE;
#if DEVELOPMENT || DEBUG
	extern uint64_t simulate_stretched_io;
	uintptr_t paddr = pmap_verify_noncacheable(vaddr);
#endif /* x86_64 DEVELOPMENT || DEBUG */
	if (__improbable(reportphywritedelayabs != 0)) {
		istate = ml_set_interrupts_enabled(FALSE);
		sabs = mach_absolute_time();
		timewrite = TRUE;
	}

#if DEVELOPMENT || DEBUG
	if (__improbable(timewrite && simulate_stretched_io)) {
		sabs -= simulate_stretched_io;
	}
#endif /* x86_64 DEVELOPMENT || DEBUG */
#endif /* x86_64 */

	switch (size) {
	case 1:
		*(volatile uint8_t *)vaddr = (uint8_t)val;
		break;
	case 2:
		*(volatile uint16_t *)vaddr = (uint16_t)val;
		break;
	case 4:
		*(volatile uint32_t *)vaddr = (uint32_t)val;
		break;
	case 8:
		*(volatile uint64_t *)vaddr = (uint64_t)val;
		break;
	default:
		panic("Invalid size %d for ml_io_write(%p, 0x%llx)", size, (void *)vaddr, val);
		break;
	}

#if defined(__x86_64__)
	if (__improbable(timewrite == TRUE)) {
		eabs = mach_absolute_time();

#if DEVELOPMENT || DEBUG
		iotrace(IOTRACE_IO_WRITE, vaddr, paddr, size, val, sabs, eabs - sabs);
#endif

		if (__improbable((eabs - sabs) > reportphywritedelayabs)) {
#if !(DEVELOPMENT || DEBUG)
			uintptr_t paddr = kvtophys(vaddr);
#endif

			(void)ml_set_interrupts_enabled(istate);

			if (phywritepanic && (machine_timeout_suspended() == FALSE)) {
				panic_io_port_read();
				panic("Write to IO vaddr %p paddr %p val 0x%llx took %llu ns,"
				    " (start: %llu, end: %llu), ceiling: %llu",
				    (void *)vaddr, (void *)paddr, val, (eabs - sabs), sabs, eabs,
				    reportphywritedelayabs);
			}

			if (reportphywriteosbt) {
				OSReportWithBacktrace("ml_io_write size %d (v=%p, p=%p, 0x%llx) "
				    "took %lluus",
				    size, (void *)vaddr, (void *)paddr, val, (eabs - sabs) / NSEC_PER_USEC);
			}
#if CONFIG_DTRACE
			DTRACE_PHYSLAT5(physiowrite, uint64_t, (eabs - sabs),
			    uint64_t, vaddr, uint32_t, size, uint64_t, paddr, uint64_t, val);
#endif /* CONFIG_DTRACE */
		} else if (__improbable(tracephywritedelayabs > 0 && (eabs - sabs) > tracephywritedelayabs)) {
#if !(DEVELOPMENT || DEBUG)
			uintptr_t paddr = kvtophys(vaddr);
#endif

			KDBG(MACHDBG_CODE(DBG_MACH_IO, DBC_MACH_IO_MMIO_WRITE),
			    (eabs - sabs), VM_KERNEL_UNSLIDE_OR_PERM(vaddr), paddr, val);

			(void)ml_set_interrupts_enabled(istate);
		} else {
			(void)ml_set_interrupts_enabled(istate);
		}
	}
#endif /* x86_64 */
}

void
ml_io_write8(uintptr_t vaddr, uint8_t val)
{
	ml_io_write(vaddr, val, 1);
}

void
ml_io_write16(uintptr_t vaddr, uint16_t val)
{
	ml_io_write(vaddr, val, 2);
}

void
ml_io_write32(uintptr_t vaddr, uint32_t val)
{
	ml_io_write(vaddr, val, 4);
}

void
ml_io_write64(uintptr_t vaddr, uint64_t val)
{
	ml_io_write(vaddr, val, 8);
}
