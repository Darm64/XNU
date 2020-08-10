/*
 * Copyright (c) 2005-2012 Apple Inc. All rights reserved.
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

// NOTE:  This file is only c++ so I can get static initialisers going
#include <libkern/OSDebug.h>
#include <IOKit/IOLib.h>

#include <sys/cdefs.h>

#include <stdarg.h>
#include <mach/mach_types.h>
#include <mach/kmod.h>
#include <kern/locks.h>

#include <libkern/libkern.h>    // From bsd's libkern directory
#include <mach/vm_param.h>

#include <sys/kdebug.h>
#include <kern/thread.h>

#if defined(HAS_APPLE_PAC)
#include <ptrauth.h>
#endif

extern int etext;
__BEGIN_DECLS
// From osmfk/kern/thread.h but considered to be private
extern vm_offset_t min_valid_stack_address(void);
extern vm_offset_t max_valid_stack_address(void);

// From osfmk/kern/printf.c
extern boolean_t doprnt_hide_pointers;

// From osfmk/kmod.c
extern void kmod_dump_log(vm_offset_t *addr, unsigned int cnt, boolean_t doUnslide);

extern addr64_t kvtophys(vm_offset_t va);
#if __arm__
extern int copyinframe(vm_address_t fp, char *frame);
#elif defined(__arm64__)
extern int copyinframe(vm_address_t fp, char *frame, boolean_t is64bit);
#endif

__END_DECLS

extern lck_grp_t *IOLockGroup;

static lck_mtx_t *sOSReportLock = lck_mtx_alloc_init(IOLockGroup, LCK_ATTR_NULL);

/* Use kernel_debug() to log a backtrace */
void
trace_backtrace(uint32_t debugid, uint32_t debugid2, uintptr_t size, uintptr_t data)
{
	void *bt[16];
	const unsigned cnt = sizeof(bt) / sizeof(bt[0]);
	unsigned i;
	int found = 0;

	OSBacktrace(bt, cnt);

	/* find first non-kernel frame */
	for (i = 3; i < cnt && bt[i]; i++) {
		if (bt[i] > (void*)&etext) {
			found = 1;
			break;
		}
	}
	/*
	 * if there are non-kernel frames, only log these
	 * otherwise, log everything but the first two
	 */
	if (!found) {
		i = 2;
	}

#define safe_bt(a) (uintptr_t)(a<cnt ? bt[a] : NULL)
	kernel_debug(debugid, data, size, safe_bt(i), safe_bt(i + 1), 0);
	kernel_debug(debugid2, safe_bt(i + 2), safe_bt(i + 3), safe_bt(i + 4), safe_bt(i + 5), 0);
}

/* Report a message with a 4 entry backtrace - very slow */
void
OSReportWithBacktrace(const char *str, ...)
{
	char buf[128];
	void *bt[9] = {};
	const unsigned cnt = sizeof(bt) / sizeof(bt[0]);
	va_list listp;

	// Ignore the our and our callers stackframes, skipping frames 0 & 1
	(void) OSBacktrace(bt, cnt);

	va_start(listp, str);
	vsnprintf(buf, sizeof(buf), str, listp);
	va_end(listp);

	lck_mtx_lock(sOSReportLock);
	{
		boolean_t old_doprnt_hide_pointers = doprnt_hide_pointers;
		doprnt_hide_pointers = FALSE;
		printf("%s\nBacktrace 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n", buf,
		    (unsigned long) VM_KERNEL_UNSLIDE(bt[2]), (unsigned long) VM_KERNEL_UNSLIDE(bt[3]),
		    (unsigned long) VM_KERNEL_UNSLIDE(bt[4]), (unsigned long) VM_KERNEL_UNSLIDE(bt[5]),
		    (unsigned long) VM_KERNEL_UNSLIDE(bt[6]), (unsigned long) VM_KERNEL_UNSLIDE(bt[7]),
		    (unsigned long) VM_KERNEL_UNSLIDE(bt[8]));
		kmod_dump_log((vm_offset_t *) &bt[2], cnt - 2, TRUE);
		doprnt_hide_pointers = old_doprnt_hide_pointers;
	}
	lck_mtx_unlock(sOSReportLock);
}

static vm_offset_t minstackaddr = min_valid_stack_address();
static vm_offset_t maxstackaddr = max_valid_stack_address();


#if __x86_64__
#define x86_64_RETURN_OFFSET 8
static unsigned int
x86_64_validate_raddr(vm_offset_t raddr)
{
	return (raddr > VM_MIN_KERNEL_AND_KEXT_ADDRESS) &&
	       (raddr < VM_MAX_KERNEL_ADDRESS);
}
static unsigned int
x86_64_validate_stackptr(vm_offset_t stackptr)
{
	/* Existence and alignment check
	 */
	if (!stackptr || (stackptr & 0x7) || !x86_64_validate_raddr(stackptr)) {
		return 0;
	}

	/* Is a virtual->physical translation present?
	 */
	if (!kvtophys(stackptr)) {
		return 0;
	}

	/* Check if the return address lies on the same page;
	 * If not, verify that a translation exists.
	 */
	if (((PAGE_SIZE - (stackptr & PAGE_MASK)) < x86_64_RETURN_OFFSET) &&
	    !kvtophys(stackptr + x86_64_RETURN_OFFSET)) {
		return 0;
	}
	return 1;
}
#endif

void
OSPrintBacktrace(void)
{
	void * btbuf[20];
	int tmp = OSBacktrace(btbuf, 20);
	int i;
	for (i = 0; i < tmp; i++) {
		kprintf("bt[%.2d] = %p\n", i, btbuf[i]);
	}
}

unsigned
OSBacktrace(void **bt, unsigned maxAddrs)
{
	unsigned frame;
	if (!current_thread()) {
		return 0;
	}

#if   __x86_64__
#define SANE_x86_64_FRAME_SIZE (kernel_stack_size >> 1)
	vm_offset_t stackptr, stackptr_prev, raddr;
	unsigned frame_index = 0;
/* Obtain current frame pointer */

	__asm__ volatile ("movq %%rbp, %0" : "=m" (stackptr));

	if (!x86_64_validate_stackptr(stackptr)) {
		goto pad;
	}

	raddr = *((vm_offset_t *) (stackptr + x86_64_RETURN_OFFSET));

	if (!x86_64_validate_raddr(raddr)) {
		goto pad;
	}

	bt[frame_index++] = (void *) raddr;

	for (; frame_index < maxAddrs; frame_index++) {
		stackptr_prev = stackptr;
		stackptr = *((vm_offset_t *) stackptr_prev);

		if (!x86_64_validate_stackptr(stackptr)) {
			break;
		}
		/* Stack grows downwards */
		if (stackptr < stackptr_prev) {
			break;
		}

		if ((stackptr - stackptr_prev) > SANE_x86_64_FRAME_SIZE) {
			break;
		}

		raddr = *((vm_offset_t *) (stackptr + x86_64_RETURN_OFFSET));

		if (!x86_64_validate_raddr(raddr)) {
			break;
		}

		bt[frame_index] = (void *) raddr;
	}
pad:
	frame = frame_index;

	for (; frame_index < maxAddrs; frame_index++) {
		bt[frame_index] = (void *) NULL;
	}
#elif __arm__ || __arm64__
	uint32_t i = 0;
	uintptr_t frameb[2];
	uintptr_t fp = 0;

	// get the current frame pointer for this thread
#if defined(__arm__)
#define OSBacktraceFrameAlignOK(x) (((x) & 0x3) == 0)
	__asm__ volatile ("mov %0,r7" : "=r" (fp));
#elif defined(__arm64__)
#define OSBacktraceFrameAlignOK(x) (((x) & 0xf) == 0)
	__asm__ volatile ("mov %0, fp" : "=r" (fp));
#else
#error Unknown architecture.
#endif

	// now crawl up the stack recording the link value of each frame
	do {
		// check bounds
		if ((fp == 0) || (!OSBacktraceFrameAlignOK(fp)) || (fp > VM_MAX_KERNEL_ADDRESS) || (fp < VM_MIN_KERNEL_AND_KEXT_ADDRESS)) {
			break;
		}
		// safely read frame
#ifdef __arm64__
		if (copyinframe(fp, (char*)frameb, TRUE) != 0) {
#else
		if (copyinframe(fp, (char*)frameb) != 0) {
#endif
			break;
		}

		// No need to use copyin as this is always a kernel address, see check above
#if defined(HAS_APPLE_PAC)
		/* return addresses on stack signed by arm64e ABI */
		bt[i] = ptrauth_strip((void*)frameb[1], ptrauth_key_return_address); // link register
#else
		bt[i] = (void*)frameb[1]; // link register
#endif
		fp = frameb[0];
	} while (++i < maxAddrs);
	frame = i;
#else
#error arch
#endif
	return frame;
}
