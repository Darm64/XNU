/*
 * Copyright (c) 2000-2010 Apple Inc. All rights reserved.
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
 
#include <i386/asm.h>
#include <i386/rtclock_asm.h>
#include <i386/proc_reg.h>
#include <i386/eflags.h>
       
#include <i386/postcode.h>
#include <i386/apic.h>
#include <i386/vmx/vmx_asm.h>
#include <assym.s>

/*
**      ml_get_timebase()
**
**      Returns TSC in RAX
**
*/
ENTRY(ml_get_timebase)

	lfence
	rdtsc
	lfence
        shlq	$32,%rdx 
        orq	%rdx,%rax
			
	ret

/*
 *  	Convert between various timer units 
 *
 *	This code converts 64-bit time units to other units.
 *	For example, the TSC is converted to HPET units.
 *
 *	Time is a 64-bit integer that is some number of ticks.
 *	Conversion is 64-bit fixed point number which is composed
 *	of a 32 bit integer and a 32 bit fraction. 
 *
 *	The time ticks are multiplied by the conversion factor.  The
 *	calculations are done as a 128-bit value but both the high
 *	and low words are dropped.  The high word is overflow and the
 *	low word is the fraction part of the result.
 *
 *	We return a 64-bit value.
 *
 *	Note that we can use this function to multiply 2 conversion factors.
 *	We do this in order to calculate the multiplier used to convert
 *	directly between any two units.
 *
 *	uint64_t tmrCvt(uint64_t time,		// %rdi
 *			uint64_t conversion)	// %rsi
 *
 */
ENTRY(tmrCvt)
	cmpq	$1,%rsi				/* check for unity fastpath */
	je	1f
	movq	%rdi,%rax
	mulq	%rsi				/* result is %rdx:%rax */
	shrdq   $32,%rdx,%rax			/* %rdx:%rax >>= 32 */
	ret
1:
	mov	%rdi,%rax
	ret

 /*
 * void _rtc_nanotime_adjust(
 *		uint64_t        tsc_base_delta,	// %rdi
 *		rtc_nanotime_t  *dst);		// %rsi
 */
ENTRY(_rtc_nanotime_adjust)
	movl	RNT_GENERATION(%rsi),%eax	/* get current generation */
	movl	$0,RNT_GENERATION(%rsi)		/* flag data as being updated */
	addq	%rdi,RNT_TSC_BASE(%rsi)

	incl	%eax				/* next generation */
	jnz	1f
	incl	%eax				/* skip 0, which is a flag */
1:	movl	%eax,RNT_GENERATION(%rsi)	/* update generation */

	ret

/*
 * uint64_t _rtc_nanotime_read(rtc_nanotime_t *rntp);
 *
 * This is the same as the commpage nanotime routine, except that it uses the
 * kernel internal "rtc_nanotime_info" data instead of the commpage data.
 * These two copies of data are kept in sync by rtc_clock_napped().
 *
 * Warning!  There are several copies of this code in the trampolines found in
 * osfmk/x86_64/idt64.s, coming from the various TIMER macros in rtclock_asm.h.
 * They're all kept in sync by using the RTC_NANOTIME_READ() macro.
 *
 * The algorithm we use is:
 *
 *	ns = ((((rdtsc - rnt_tsc_base)<<rnt_shift)*rnt_tsc_scale) / 2**32) + rnt_ns_base;
 *
 * rnt_shift, a constant computed during initialization, is the smallest value for which:
 *
 *	(tscFreq << rnt_shift) > SLOW_TSC_THRESHOLD
 *
 * Where SLOW_TSC_THRESHOLD is about 10e9.  Since most processor's tscFreqs are greater
 * than 1GHz, rnt_shift is usually 0.  rnt_tsc_scale is also a 32-bit constant:
 *
 *	rnt_tsc_scale = (10e9 * 2**32) / (tscFreq << rnt_shift);
 *
 * On 64-bit processors this algorithm could be simplified by doing a 64x64 bit
 * multiply of rdtsc by tscFCvtt2n:
 *
 *	ns = (((rdtsc - rnt_tsc_base) * tscFCvtt2n) / 2**32) + rnt_ns_base;
 *
 * We don't do so in order to use the same algorithm in 32- and 64-bit mode.
 * When U32 goes away, we should reconsider.
 *
 * Since this routine is not synchronized and can be called in any context, 
 * we use a generation count to guard against seeing partially updated data.
 * In addition, the _rtc_nanotime_store() routine zeroes the generation before
 * updating the data, and stores the nonzero generation only after all fields
 * have been stored.  Because IA32 guarantees that stores by one processor
 * must be seen in order by another, we can avoid using a lock.  We spin while
 * the generation is zero.
 *
 * unint64_t _rtc_nanotime_read(
 *			rtc_nanotime_t *rntp);		// %rdi
 *
 */
ENTRY(_rtc_nanotime_read)

	PAL_RTC_NANOTIME_READ_FAST()

	ret
    
/*
 * extern uint64_t _rtc_tsc_to_nanoseconds(
 *          uint64_t    value,              // %rdi
 *          pal_rtc_nanotime_t	*rntp);     // %rsi
 *
 * Converts TSC units to nanoseconds, using an abbreviated form of the above
 * algorithm.  Note that while we could have simply used tmrCvt(value,tscFCvtt2n),
 * which would avoid the need for this asm, doing so is a bit more risky since
 * we'd be using a different algorithm with possibly different rounding etc.
 */

ENTRY(_rtc_tsc_to_nanoseconds)
	movq    %rdi,%rax			/* copy value (in TSC units) to convert */
	movl    RNT_SHIFT(%rsi),%ecx
	movl    RNT_SCALE(%rsi),%edx
	shlq    %cl,%rax			/* tscUnits << shift */
	mulq    %rdx				/* (tscUnits << shift) * scale */
	shrdq   $32,%rdx,%rax			/* %rdx:%rax >>= 32 */
	ret
    

/*
 *  typedef void (*thread_continue_t)(void *param, wait_result_t)
 *
 *	void call_continuation( thread_continue_t continuation,
 *	            			void *param,
 *				            wait_result_t wresult,
 *                          bool enable interrupts)
 */

Entry(call_continuation)

	movq    %rdi, %r12  /* continuation */
    movq    %rsi, %r13  /* continuation param */
    movq    %rdx, %r14  /* wait result */

	movq	%gs:CPU_KERNEL_STACK,%rsp	/* set the stack */
	xorq	%rbp,%rbp			/* zero frame pointer */

    test    %ecx, %ecx
    jz 1f
    mov     $1, %edi
    call   _ml_set_interrupts_enabled
1:

	movq	%r12,%rcx			/* continuation */
	movq	%r13,%rdi			/* continuation param */
	movq	%r14,%rsi			/* wait result */

	call	*%rcx				/* call continuation */
	movq	%gs:CPU_ACTIVE_THREAD,%rdi
	call	EXT(thread_terminate)


Entry(x86_init_wrapper)
	xor	%rbp, %rbp
	movq	%rsi, %rsp
	callq	*%rdi

#if CONFIG_VMX

/*
 *	__vmxon -- Enter VMX Operation
 *	int __vmxon(addr64_t v);
 */
Entry(__vmxon)
	FRAME
	push	%rdi
	
	mov	$(VMX_FAIL_INVALID), %ecx
	mov	$(VMX_FAIL_VALID), %edx
	mov	$(VMX_SUCCEED), %eax
	vmxon	(%rsp)
	cmovcl 	%ecx, %eax	/* CF = 1, ZF = 0 */
	cmovzl	%edx, %eax	/* CF = 0, ZF = 1 */

	pop	%rdi
	EMARF
	ret

/*
 *	__vmxoff -- Leave VMX Operation
 *	int __vmxoff(void);
 */
Entry(__vmxoff)
	FRAME
	
	mov	$(VMX_FAIL_INVALID), %ecx
	mov	$(VMX_FAIL_VALID), %edx
	mov	$(VMX_SUCCEED), %eax
	vmxoff
	cmovcl 	%ecx, %eax	/* CF = 1, ZF = 0 */
	cmovzl	%edx, %eax	/* CF = 0, ZF = 1 */

	EMARF
	ret

#endif /* CONFIG_VMX */

/*
 *	mfence -- Memory Barrier
 *	Use out-of-line assembly to get
 *	standard x86-64 ABI guarantees
 *	about what the caller's codegen
 *	has in registers vs. memory
 */
Entry(do_mfence)
	mfence
	ret
