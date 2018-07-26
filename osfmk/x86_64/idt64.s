/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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
#include <assym.s>
#include <debug.h>
#include <i386/eflags.h>
#include <i386/rtclock_asm.h>
#include <i386/trap.h>
#define _ARCH_I386_ASM_HELP_H_	/* Prevent inclusion of user header */
#include <mach/i386/syscall_sw.h>
#include <i386/postcode.h>
#include <i386/proc_reg.h>
#include <mach/exception_types.h>

#if DEBUG
#define	DEBUG_IDT64 		1	
#endif

/*
 * This is the low-level trap and interrupt handling code associated with
 * the IDT. It also includes system call handlers for sysenter/syscall.
 * The IDT itself is defined in mp_desc.c.
 *
 * Code here is structured as follows:
 *
 * stubs 	Code called directly from an IDT vector.
 *		All entry points have the "idt64_" prefix and they are built
 *		using macros expanded by the inclusion of idt_table.h.
 *		This code performs vector-dependent identification and jumps
 * 		into the dispatch code.
 *
 * dispatch	The dispatch code is responsible for saving the thread state
 *		(which is either 64-bit or 32-bit) and then jumping to the
 *		class handler identified by the stub.
 *
 * returns	Code to restore state and return to the previous context.
 *
 * handlers	There are several classes of handlers:
 *   interrupt	- asynchronous events typically from external devices
 *   trap	- synchronous events due to thread execution
 *   syscall	- synchronous system call request
 *   fatal	- fatal traps
 */
/*
 * Indices of handlers for each exception type.
 */
#define	HNDL_ALLINTRS		0
#define	HNDL_ALLTRAPS		1
#define	HNDL_SYSENTER		2
#define	HNDL_SYSCALL		3
#define	HNDL_UNIX_SCALL		4
#define	HNDL_MACH_SCALL		5
#define	HNDL_MDEP_SCALL		6
#define	HNDL_DOUBLE_FAULT	7
#define	HNDL_MACHINE_CHECK	8

/* Begin double-mapped descriptor section */
	
.section	__HIB, __desc
.globl EXT(idt64_hndl_table0)
EXT(idt64_hndl_table0):
	.quad EXT(ks_dispatch)
	.quad EXT(ks_64bit_return)
	.quad 0 /* Populated with CPU shadow displacement*/
	.quad EXT(ks_return)

EXT(idt64_hndl_table1):
	.quad	EXT(hndl_allintrs)
	.quad	EXT(hndl_alltraps)
	.quad	EXT(hndl_sysenter)
	.quad	EXT(hndl_syscall)
	.quad	EXT(hndl_unix_scall)
	.quad	EXT(hndl_mach_scall)
	.quad	EXT(hndl_mdep_scall)
	.quad	EXT(hndl_double_fault)
	.quad	EXT(hndl_machine_check)
.text


/* The wrapper for all non-special traps/interrupts */
/* Everything up to PUSH_FUNCTION is just to output 
 * the interrupt number out to the postcode display
 */
#if DEBUG_IDT64
#define IDT_ENTRY_WRAPPER(n, f)			 \
	push	%rax				;\
	POSTCODE2(0x6400+n)			;\
	pop	%rax				;\
	pushq	$(f)				;\
	pushq	$(n)				;\
	jmp L_dispatch
#else
#define IDT_ENTRY_WRAPPER(n, f)			 \
	pushq	$(f)				;\
	pushq	$(n)				;\
	jmp L_dispatch
#endif

/* A trap that comes with an error code already on the stack */
#define TRAP_ERR(n, f)				 \
	Entry(f)				;\
	IDT_ENTRY_WRAPPER(n, HNDL_ALLTRAPS)

/* A normal trap */
#define TRAP(n, f)				 \
	Entry(f)				;\
	pushq	$0          			;\
	IDT_ENTRY_WRAPPER(n, HNDL_ALLTRAPS)

#define USER_TRAP TRAP

/* An interrupt */
#define INTERRUPT(n)			 	\
	Entry(_intr_ ## n)			;\
	pushq	$0          			;\
	IDT_ENTRY_WRAPPER(n, HNDL_ALLINTRS)

/* A trap with a special-case handler, hence we don't need to define anything */
#define TRAP_SPC(n, f)
#define TRAP_IST1(n, f)
#define TRAP_IST2(n, f)
#define USER_TRAP_SPC(n, f)

/* Begin double-mapped text section */
.section __HIB, __text
/* Generate all the stubs */
#include "idt_table.h"

Entry(idt64_page_fault)
	pushq	$(HNDL_ALLTRAPS)
	push	$(T_PAGE_FAULT)
	jmp	L_dispatch

Entry(idt64_debug)
	push	$0			/* error code */
	pushq	$(HNDL_ALLTRAPS)
	pushq	$(T_DEBUG)
	jmp	L_dispatch
/*
 * Legacy interrupt gate System call handlers.
 * These are entered via a syscall interrupt. The system call number in %rax
 * is saved to the error code slot in the stack frame. We then branch to the
 * common state saving code.
 */

#ifndef UNIX_INT
#error NO UNIX INT!!!
#endif
Entry(idt64_unix_scall)
	pushq	%rax			/* save system call number */
	pushq	$(HNDL_UNIX_SCALL)
	pushq	$(UNIX_INT)
	jmp	L_dispatch
	
Entry(idt64_mach_scall)
	pushq	%rax			/* save system call number */
	pushq	$(HNDL_MACH_SCALL)
	pushq	$(MACH_INT)
	jmp	L_dispatch
	
Entry(idt64_mdep_scall)
	pushq	%rax			/* save system call number */
	pushq	$(HNDL_MDEP_SCALL)
	pushq	$(MACHDEP_INT)
	jmp	L_dispatch

/*
 * For GP/NP/SS faults, we use the IST1 stack.
 * For faults from user-space, we have to copy the machine state to the
 * PCB stack and then dispatch as normal.
 * For faults in kernel-space, we need to scrub for kernel exit faults and
 * treat these as user-space faults. But for all other kernel-space faults
 * we continue to run on the IST1 stack and we dispatch to handle the fault
 * as fatal.
 */
Entry(idt64_gen_prot)
	pushq	$(HNDL_ALLTRAPS)
	pushq	$(T_GENERAL_PROTECTION)
	jmp	L_dispatch

Entry(idt64_stack_fault)
	pushq	$(HNDL_ALLTRAPS)
	pushq	$(T_STACK_FAULT)
	jmp	L_dispatch

Entry(idt64_segnp)
	pushq	$(HNDL_ALLTRAPS)
	pushq	$(T_SEGMENT_NOT_PRESENT)
	jmp	L_dispatch

/*
 * Fatal exception handlers:
 */
Entry(idt64_db_task_dbl_fault)
	pushq	$(HNDL_DOUBLE_FAULT)
	pushq	$(T_DOUBLE_FAULT)
	jmp	L_dispatch

Entry(idt64_db_task_stk_fault)
	pushq	$(HNDL_DOUBLE_FAULT)
	pushq	$(T_STACK_FAULT)
	jmp	L_dispatch

Entry(idt64_mc)
	push	$(0)			/* Error */
	pushq	$(HNDL_MACHINE_CHECK)
	pushq	$(T_MACHINE_CHECK)
	jmp	L_dispatch

/*
 * NMI
 * This may or may not be fatal but extreme care is required
 * because it may fall when control was already in another trampoline.
 *
 * We get here on IST2 stack which is used for NMIs only.
 */
Entry(idt64_nmi)
	push	%rax				/* save RAX to ISF64_ERR */
	push	%rcx				/* save RCX to ISF64_TRAPFN */
	push	%rdx				/* save RDX to ISF64_TRAPNO */
	jmp	L_dispatch

Entry(idt64_double_fault)
	pushq	$(HNDL_DOUBLE_FAULT)
	pushq	$(T_DOUBLE_FAULT)
	jmp	L_dispatch

Entry(hi64_syscall)
Entry(idt64_syscall)
	swapgs
     /* Use RAX as a temporary by shifting its contents into R11[32:63]
      * The systemcall number is defined to be a 32-bit quantity, as is
      * RFLAGS.
      */
	shlq	$32, %rax
	or 	%rax, %r11
.globl EXT(dblsyscall_patch_point)
EXT(dblsyscall_patch_point):
//	movabsq	$0x12345678ABCDEFFFULL, %rax
     /* Generate offset to the double-mapped per-CPU data shadow
      * into RAX
      */
	leaq	EXT(idt64_hndl_table0)(%rip), %rax
	mov	16(%rax), %rax
	mov     %rsp, %gs:CPU_UBER_TMP(%rax)  /* save user stack */
	mov     %gs:CPU_ESTACK(%rax), %rsp  /* switch stack to per-cpu estack */
	sub	$(ISF64_SIZE), %rsp

	/*
	 * Synthesize an ISF frame on the exception stack
	 */
	movl	$(USER_DS), ISF64_SS(%rsp)
	mov	%rcx, ISF64_RIP(%rsp)		/* rip */

	mov	%gs:CPU_UBER_TMP(%rax), %rcx
	mov	%rcx, ISF64_RSP(%rsp)		/* user stack --changed */

	mov	%r11, %rax
	shrq	$32, %rax		/* Restore RAX */
	mov	%r11d, %r11d		/* Clear r11[32:63] */

	mov	%r11, ISF64_RFLAGS(%rsp)	/* rflags */
	movl	$(SYSCALL_CS), ISF64_CS(%rsp)	/* cs - a pseudo-segment */
	mov	%rax, ISF64_ERR(%rsp)		/* err/rax - syscall code */
	movq	$(HNDL_SYSCALL), ISF64_TRAPFN(%rsp)
	movq	$(T_SYSCALL), ISF64_TRAPNO(%rsp)	/* trapno */
	swapgs
	jmp	L_dispatch			/* this can only be 64-bit */

Entry(hi64_sysenter)
Entry(idt64_sysenter)
	/* Synthesize an interrupt stack frame onto the
	 * exception stack.
	 */
	push	$(USER_DS)		/* ss */
	push	%rcx			/* uesp */
	pushf				/* flags */
	/*
	 * Clear, among others, the Nested Task (NT) flags bit;
	 * this is zeroed by INT, but not by SYSENTER.
	 */
	push	$0
	popf
	push	$(SYSENTER_CS)		/* cs */ 
L_sysenter_continue:
	push	%rdx			/* eip */
	push	%rax			/* err/eax - syscall code */
	pushq	$(HNDL_SYSENTER)
	pushq	$(T_SYSENTER)
	orl	$(EFL_IF), ISF64_RFLAGS(%rsp)
	jmp L_dispatch

/*
 * Common dispatch point.
 * Determine what mode has been interrupted and save state accordingly.
 * Here with:
 *	rsp	from user-space:   interrupt state in PCB, or
 *		from kernel-space: interrupt state in kernel or interrupt stack
 *	GSBASE	from user-space:   pthread area, or
 *		from kernel-space: cpu_data
 */

L_dispatch:
	pushq	%rax
	testb	$3, 8+ISF64_CS(%rsp)
	jz	1f
     	swapgs
	leaq	EXT(idt64_hndl_table0)(%rip), %rax
     	mov	16(%rax), %rax

	mov	%gs:CPU_TASK_CR3(%rax), %rax 
	mov	%rax, %cr3
#if	DEBUG
	mov	%rax, %gs:CPU_ENTRY_CR3
#endif
1:
	/* The text/data relationship here must be preserved in the doublemap, and the contents must be remapped */
	leaq	EXT(idt64_hndl_table0)(%rip), %rax
	/* Indirect branch to non-doublemapped trampolines */
	jmp *(%rax)
/* User return: register restoration and address space switch sequence */
Entry(ks_64bit_return)
	mov	R64_R14(%r15), %r14
	mov	R64_R13(%r15), %r13
	mov	R64_R12(%r15), %r12
	mov	R64_R11(%r15), %r11
	mov	R64_R10(%r15), %r10
	mov	R64_R9(%r15),  %r9
	mov	R64_R8(%r15),  %r8
	mov	R64_RSI(%r15), %rsi
	mov	R64_RDI(%r15), %rdi
	mov	R64_RBP(%r15), %rbp
	mov	R64_RDX(%r15), %rdx
	mov	R64_RCX(%r15), %rcx
	mov	R64_RBX(%r15), %rbx
	mov	R64_RAX(%r15), %rax
	/* Switch to per-CPU exception stack */
	mov	%gs:CPU_ESTACK, %rsp

	/* Synthesize interrupt stack frame from PCB savearea to exception stack */
	push	R64_SS(%r15)
	push	R64_RSP(%r15)
	push	R64_RFLAGS(%r15)
	push	R64_CS(%r15)
	push	R64_RIP(%r15)

	mov	R64_R15(%r15), %r15
	cmpq	$(KERNEL64_CS), 8(%rsp)
	jz	1f
	/* Discover user cr3/ASID */
	push	%rax
	mov	%gs:CPU_UCR3, %rax
#if	DEBUG
	mov	%rax, %gs:CPU_EXIT_CR3
#endif
	mov	%rax, %cr3
	/* Continue execution on the shared/doublemapped trampoline */
	pop	%rax
	swapgs
1:
	cmpl	$(SYSCALL_CS), 8(%rsp) /* test for exit via SYSRET */
	je      L_sysret
EXT(ret64_iret):
        iretq				/* return from interrupt */
L_sysret:
	/*
	 * Here to restore rcx/r11/rsp and perform the sysret back to user-space.
	 * 	rcx	user rip
	 *	r11	user rflags
	 *	rsp	user stack pointer
	 */
	pop	%rcx
	add	$8, %rsp
	pop	%r11
	pop	%rsp
	sysretq				/* return from system call */
/* End of double-mapped TEXT */
.text

Entry(ks_dispatch)
	popq	%rax
	cmpl	$(KERNEL64_CS), ISF64_CS(%rsp)
	je	EXT(ks_dispatch_kernel)

	mov 	%rax, %gs:CPU_UBER_TMP
	mov 	%gs:CPU_UBER_ISF, %rax
	add 	$(ISF64_SIZE), %rax

	xchg	%rsp, %rax
/* Memory to memory moves (aint x86 wonderful):
 * Transfer the exception frame from the per-CPU exception stack to the
 * 'PCB' stack programmed at cswitch.
 */
	push	ISF64_SS(%rax)
	push	ISF64_RSP(%rax)
	push	ISF64_RFLAGS(%rax)
	push	ISF64_CS(%rax)
	push	ISF64_RIP(%rax)
	push	ISF64_ERR(%rax)
	push	ISF64_TRAPFN(%rax)
	push 	ISF64_TRAPNO(%rax)
	mov	%gs:CPU_UBER_TMP, %rax
	jmp	EXT(ks_dispatch_user)

Entry (ks_return)
     jmp .

Entry(ks_dispatch_user)
	cmpl	$(TASK_MAP_32BIT), %gs:CPU_TASK_MAP
	je	L_dispatch_U32		/* 32-bit user task */

L_dispatch_U64:
	subq	$(ISS64_OFFSET), %rsp
	mov	%r15, R64_R15(%rsp)
	mov	%rsp, %r15
	mov	%gs:CPU_KERNEL_STACK, %rsp
	jmp	L_dispatch_64bit

Entry(ks_dispatch_kernel)
	subq	$(ISS64_OFFSET), %rsp
	mov	%r15, R64_R15(%rsp)
	mov	%rsp, %r15

/*
 * Here for 64-bit user task or kernel
 */
L_dispatch_64bit:
	movl	$(SS_64), SS_FLAVOR(%r15)

	/*
	 * Save segment regs - for completeness since theyre not used.
	 */
	movl	%fs, R64_FS(%r15)
	movl	%gs, R64_GS(%r15)

	/* Save general-purpose registers */
	mov	%rax, R64_RAX(%r15)
	mov	%rbx, R64_RBX(%r15)
	mov	%rcx, R64_RCX(%r15)
	mov	%rdx, R64_RDX(%r15)
	mov	%rbp, R64_RBP(%r15)
	mov	%rdi, R64_RDI(%r15)
	mov	%rsi, R64_RSI(%r15)
	mov	%r8,  R64_R8(%r15)
	mov	%r9,  R64_R9(%r15)
	mov	%r10, R64_R10(%r15)
	mov	%r11, R64_R11(%r15)
	mov	%r12, R64_R12(%r15)
	mov	%r13, R64_R13(%r15)
	mov	%r14, R64_R14(%r15)

	/* cr2 is significant only for page-faults */
	mov	%cr2, %rax
	mov	%rax, R64_CR2(%r15)

	mov	R64_TRAPNO(%r15), %ebx	/* %ebx := trapno for later */
	mov	R64_TRAPFN(%r15), %rdx	/* %rdx := trapfn for later */
	mov	R64_CS(%r15), %esi	/* %esi := cs for later */

	jmp	L_common_dispatch

L_64bit_entry_reject:
	/*
	 * Here for a 64-bit user attempting an invalid kernel entry.
	 */
	movq	$(HNDL_ALLTRAPS), ISF64_TRAPFN(%rsp)
	movq	$(T_INVALID_OPCODE), ISF64_TRAPNO(%rsp)
	jmp 	L_dispatch_U64
	
Entry(ks_32bit_entry_check)
	/*
	 * Check we're not a confused 64-bit user.
	 */
	cmpl	$(TASK_MAP_32BIT), %gs:CPU_TASK_MAP
	jne	L_64bit_entry_reject
	/* fall through to 32-bit handler: */

L_dispatch_U32: /* 32-bit user task */
	subq	$(ISS64_OFFSET), %rsp
	mov	%rsp, %r15
	mov	%gs:CPU_KERNEL_STACK, %rsp
	movl	$(SS_32), SS_FLAVOR(%r15)

	/*
	 * Save segment regs
	 */
	movl	%ds, R32_DS(%r15)
	movl	%es, R32_ES(%r15)
	movl	%fs, R32_FS(%r15)
	movl	%gs, R32_GS(%r15)

	/*
	 * Save general 32-bit registers
	 */
	mov	%eax, R32_EAX(%r15)
	mov	%ebx, R32_EBX(%r15)
	mov	%ecx, R32_ECX(%r15)
	mov	%edx, R32_EDX(%r15)
	mov	%ebp, R32_EBP(%r15)
	mov	%esi, R32_ESI(%r15)
	mov	%edi, R32_EDI(%r15)

	/* Unconditionally save cr2; only meaningful on page faults */
	mov	%cr2, %rax
	mov	%eax, R32_CR2(%r15)

	/*
	 * Copy registers already saved in the machine state 
	 * (in the interrupt stack frame) into the compat save area.
	 */
	mov	R64_RIP(%r15), %eax
	mov	%eax, R32_EIP(%r15)
	mov	R64_RFLAGS(%r15), %eax
	mov	%eax, R32_EFLAGS(%r15)
	mov	R64_RSP(%r15), %eax
	mov	%eax, R32_UESP(%r15)
	mov	R64_SS(%r15), %eax
	mov	%eax, R32_SS(%r15)
L_dispatch_U32_after_fault:
	mov	R64_CS(%r15), %esi		/* %esi := %cs for later */
	mov	%esi, R32_CS(%r15)
	mov	R64_TRAPNO(%r15), %ebx		/* %ebx := trapno for later */
	mov	%ebx, R32_TRAPNO(%r15)
	mov	R64_ERR(%r15), %eax
	mov	%eax, R32_ERR(%r15)
	mov	R64_TRAPFN(%r15), %rdx		/* %rdx := trapfn for later */

L_common_dispatch:
	cld		/* Ensure the direction flag is clear in the kernel */
	cmpl    $0, EXT(pmap_smap_enabled)(%rip)
	je	1f
	clac		/* Clear EFLAGS.AC if SMAP is present/enabled */
1:
	/*
	 * On entering the kernel, we typically don't switch CR3
	 * because the kernel shares the user's address space.
	 * But we mark the kernel's cr3 as "active" for TLB coherency evaluation
	 * If, however, the CPU's invalid TLB flag is set, we have to invalidate the TLB
	 * since the kernel pagetables were changed while we were in userspace.
	 *
	 * For threads with a mapped pagezero (some WINE games) on non-SMAP platforms,
	 * we switch to the kernel's address space on entry. Also, 
	 * if the global no_shared_cr3 is TRUE we do switch to the kernel's cr3
	 * so that illicit accesses to userspace can be trapped.
	 */
	mov	%gs:CPU_KERNEL_CR3, %rcx
	mov	%rcx, %gs:CPU_ACTIVE_CR3
	test	$3, %esi			/* user/kernel? */
	jz	2f				/* skip cr3 reload from kernel */
	xor	%rbp, %rbp
	cmpl	$0, %gs:CPU_PAGEZERO_MAPPED
	jnz	11f
	cmpl	$0, EXT(no_shared_cr3)(%rip)
	je	2f
11:
	xor	%eax, %eax
	movw	%gs:CPU_KERNEL_PCID, %ax
	or	%rax, %rcx
	mov	%rcx, %cr3			/* load kernel cr3 */
	jmp	4f				/* and skip tlb flush test */
2:
	mov	%gs:CPU_ACTIVE_CR3+4, %rcx
	shr	$32, %rcx
	testl	%ecx, %ecx
	jz	4f
	movl	$0, %gs:CPU_TLB_INVALID
	mov	%cr4, %rcx	/* RMWW CR4, for lack of an alternative*/
	and	$(~CR4_PGE), %rcx
	mov	%rcx, %cr4
	or	$(CR4_PGE), %rcx
	mov	%rcx, %cr4
4:
	mov	%gs:CPU_ACTIVE_THREAD, %rcx	/* Get the active thread */
	testq	%rcx, %rcx
	je	5f
	movl	$-1, TH_IOTIER_OVERRIDE(%rcx)	/* Reset IO tier override to -1 before handling trap */
	cmpq	$0, TH_PCB_IDS(%rcx)	/* Is there a debug register state? */
	je	5f
	xor	%ecx, %ecx		/* If so, reset DR7 (the control) */
	mov	%rcx, %dr7
5:
	incl	%gs:hwIntCnt(,%ebx,4)		// Bump the trap/intr count
	/* Dispatch the designated handler */
	leaq	EXT(idt64_hndl_table1)(%rip), %rax
	jmp	*(%rax, %rdx, 8)

/*
 * Control is passed here to return to user.
 */ 
Entry(return_to_user)
	TIME_TRAP_UEXIT

Entry(ret_to_user)
// XXX 'Be nice to tidy up this debug register restore sequence...
	mov	%gs:CPU_ACTIVE_THREAD, %rdx
	movq	TH_PCB_IDS(%rdx),%rax	/* Obtain this thread's debug state */
	
	test	%rax, %rax		/* Is there a debug register context? */
	je	2f 			/* branch if not */
	cmpl	$(TASK_MAP_32BIT), %gs:CPU_TASK_MAP /* Are we a 32-bit task? */
	jne	1f
	movl	DS_DR0(%rax), %ecx	/* If so, load the 32 bit DRs */
	movq	%rcx, %dr0
	movl	DS_DR1(%rax), %ecx
	movq	%rcx, %dr1
	movl	DS_DR2(%rax), %ecx
	movq	%rcx, %dr2
	movl	DS_DR3(%rax), %ecx
	movq	%rcx, %dr3
	movl	DS_DR7(%rax), %ecx
	movq 	%rcx, %gs:CPU_DR7
	jmp 	2f
1:
	mov	DS64_DR0(%rax), %rcx	/* Load the full width DRs*/
	mov	%rcx, %dr0
	mov	DS64_DR1(%rax), %rcx
	mov	%rcx, %dr1
	mov	DS64_DR2(%rax), %rcx
	mov	%rcx, %dr2
	mov	DS64_DR3(%rax), %rcx
	mov	%rcx, %dr3
	mov	DS64_DR7(%rax), %rcx
	mov 	%rcx, %gs:CPU_DR7
2:
	/*
	 * On exiting the kernel there's typically no need to switch cr3 since we're
	 * already running in the user's address space which includes the
	 * kernel. We now mark the task's cr3 as active, for TLB coherency.
	 * If the target address space has a pagezero mapping present, or
	 * if no_shared_cr3 is set, we do need to switch cr3 at this point.
	 */
	mov	%gs:CPU_TASK_CR3, %rcx
	mov	%rcx, %gs:CPU_ACTIVE_CR3
	cmpl	$0, %gs:CPU_PAGEZERO_MAPPED
	jnz	L_cr3_switch_island
	movl	EXT(no_shared_cr3)(%rip), %eax
	test	%eax, %eax		/* -no_shared_cr3 */
	jnz	L_cr3_switch_island

L_cr3_switch_return:
	mov	%gs:CPU_DR7, %rax	/* Is there a debug control register?*/
	cmp	$0, %rax
	je	4f
	mov	%rax, %dr7		/* Set DR7 */
	movq	$0, %gs:CPU_DR7
4:
	cmpl	$(SS_64), SS_FLAVOR(%r15)	/* 64-bit state? */
	je	L_64bit_return

L_32bit_return:
#if DEBUG_IDT64
	cmpl	$(SS_32), SS_FLAVOR(%r15)	/* 32-bit state? */
	je	1f
	cli
	POSTCODE2(0x6432)
	CCALL1(panic_idt64, %r15)
1:
#endif /* DEBUG_IDT64 */

	/*
	 * Restore registers into the machine state for iret.
	 * Here on fault stack and PCB address in R11.
	 */
	movl	R32_EIP(%r15), %eax
	movl	%eax, R64_RIP(%r15)
	movl	R32_EFLAGS(%r15), %eax
	movl	%eax, R64_RFLAGS(%r15)
	movl	R32_CS(%r15), %eax
	movl	%eax, R64_CS(%r15)
	movl	R32_UESP(%r15), %eax
	movl	%eax, R64_RSP(%r15)
	movl	R32_SS(%r15), %eax
	movl	%eax, R64_SS(%r15)

	/* Validate DS/ES/FS/GS segment selectors with the Load Access Rights instruction prior to restoration */
	/* Exempt "known good" statically configured selectors, e.g. USER_DS and 0 */
	cmpl	$(USER_DS), R32_DS(%r15)
	jz	22f
	cmpl	$0, R32_DS(%r15)
	jz 	22f
	larw	R32_DS(%r15), %ax
	jz	22f
	movl	$(USER_DS), R32_DS(%r15)
22:
	cmpl	$(USER_DS), R32_ES(%r15)
	jz	33f
	cmpl	$0, R32_ES(%r15)
	jz 	33f
	larw	R32_ES(%r15), %ax
	jz	33f
	movl	$(USER_DS), R32_ES(%r15)
33:
	cmpl	$(USER_DS), R32_FS(%r15)
	jz	44f
	cmpl	$0, R32_FS(%r15)
	jz 	44f
	larw	R32_FS(%r15), %ax
	jz	44f
	movl	$(USER_DS), R32_FS(%r15)
44:
	cmpl	$(USER_CTHREAD), R32_GS(%r15)
	jz	55f
	cmpl	$0, R32_GS(%r15)
	jz 	55f
	larw	R32_GS(%r15), %ax
	jz	55f
	movl	$(USER_CTHREAD), R32_GS(%r15)
55:
	/*
	 * Restore general 32-bit registers
	 */
	movl	R32_EAX(%r15), %eax
	movl	R32_EBX(%r15), %ebx
	movl	R32_ECX(%r15), %ecx
	movl	R32_EDX(%r15), %edx
	movl	R32_EBP(%r15), %ebp
	movl	R32_ESI(%r15), %esi
	movl	R32_EDI(%r15), %edi

	/*
	 * Restore segment registers. A segment exception taken here will
	 * push state on the IST1 stack and will not affect the "PCB stack".
	 */
	mov	%r15, %rsp		/* Set the PCB as the stack */
	swapgs

	xor	%r8, %r8
	xor	%r9, %r9
	xor	%r10, %r10
	xor	%r11, %r11
	xor	%r12, %r12
	xor	%r13, %r13
	xor	%r14, %r14
	xor	%r15, %r15

EXT(ret32_set_ds):	
	movw	R32_DS(%rsp), %ds
EXT(ret32_set_es):
	movw	R32_ES(%rsp), %es
EXT(ret32_set_fs):
	movw	R32_FS(%rsp), %fs
EXT(ret32_set_gs):
	movw	R32_GS(%rsp), %gs

	/* pop compat frame + trapno, trapfn and error */	
	add	$(ISS64_OFFSET)+8+8+8, %rsp
	cmpl	$(SYSENTER_CS),ISF64_CS-8-8-8(%rsp)
					/* test for fast entry/exit */
	je      L_fast_exit
EXT(ret32_iret):
	iretq				/* return from interrupt */

L_fast_exit:
	pop	%rdx			/* user return eip */
	pop	%rcx			/* pop and toss cs */
	andl	$(~EFL_IF), (%rsp)	/* clear interrupts enable, sti below */
	popf				/* flags - carry denotes failure */
	pop	%rcx			/* user return esp */
	sti				/* interrupts enabled after sysexit */
	sysexitl			/* 32-bit sysexit */

L_cr3_switch_island:
	xor	%eax, %eax
	movw	%gs:CPU_ACTIVE_PCID, %ax
	or	%rax, %rcx
	mov	%rcx, %cr3
	jmp	L_cr3_switch_return

ret_to_kernel:
#if DEBUG_IDT64
	cmpl	$(SS_64), SS_FLAVOR(%r15)	/* 64-bit state? */
	je	1f
	cli
	POSTCODE2(0x6464)
	CCALL1(panic_idt64, %r15)
	hlt
1:
	cmpl	$(KERNEL64_CS), R64_CS(%r15)
	je	2f
	CCALL1(panic_idt64, %r15)
	hlt
2:
#endif

L_64bit_return:
	/*
	 * Restore general 64-bit registers.
	 * Here on fault stack and PCB address in R15.
	 */
	leaq	EXT(idt64_hndl_table0)(%rip), %rax
	jmp *8(%rax)

Entry(ks_idt64_debug_kernel)
	/*
	 * trap came from kernel mode
	 */

	push	%rax			/* save %rax temporarily */
	lea	EXT(idt64_sysenter)(%rip), %rax
	cmp	%rax, ISF64_RIP+8(%rsp)
	pop	%rax
	jne	EXT(ks_dispatch_kernel)
	/*
	 * Interrupt stack frame has been pushed on the temporary stack.
	 * We have to switch to pcb stack and patch up the saved state.
	 */ 
	mov	%rcx, ISF64_ERR(%rsp)	/* save %rcx in error slot */
	mov	ISF64_SS+8(%rsp), %rcx	/* top of temp stack -> pcb stack */
	xchg	%rcx,%rsp		/* switch to pcb stack */
	push	$(USER_DS)		/* ss */
	push	ISF64_ERR(%rcx)		/* saved %rcx into rsp slot */
	push	ISF64_RFLAGS(%rcx)	/* rflags */
	push	$(SYSENTER_TF_CS)	/* cs - not SYSENTER_CS for iret path */
	mov	ISF64_ERR(%rcx),%rcx	/* restore %rcx */
	jmp	L_sysenter_continue	/* continue sysenter entry */
	
Entry(ks_trap_check_kernel_exit)
	testb   $3,ISF64_CS(%rsp)
	jz	L_kernel_gpf

	/* Here for fault from user-space. Copy interrupt state to PCB. */
	swapgs
	push	%rax
	mov	%rcx, %gs:CPU_UBER_TMP		/* save user RCX  */
	mov	%gs:CPU_UBER_ISF, %rcx		/* PCB stack addr */
	mov	ISF64_SS+8(%rsp), %rax
	mov	%rax, ISF64_SS(%rcx)
	mov	ISF64_RSP+8(%rsp), %rax
	mov	%rax, ISF64_RSP(%rcx)
	mov	ISF64_RFLAGS+8(%rsp), %rax
	mov	%rax, ISF64_RFLAGS(%rcx)
	mov	ISF64_CS+8(%rsp), %rax
	mov	%rax, ISF64_CS(%rcx)
	mov	ISF64_RIP+8(%rsp), %rax
	mov	%rax, ISF64_RIP(%rcx)
	mov	ISF64_ERR+8(%rsp), %rax
	mov	%rax, ISF64_ERR(%rcx)
	mov	ISF64_TRAPFN+8(%rsp), %rax
	mov	%rax, ISF64_TRAPFN(%rcx)
	mov	ISF64_TRAPNO+8(%rsp), %rax
	mov	%rax, ISF64_TRAPNO(%rcx)
	pop	%rax
	mov	%gs:CPU_UBER_TMP, %rsp		/* user RCX into RSP */
	xchg	%rcx, %rsp			/* to PCB stack with user RCX */
	jmp	EXT(ks_dispatch_user)

L_kernel_gpf:
	/* Here for GPF from kernel_space. Check for recoverable cases. */
	push	%rax
	leaq	EXT(ret32_iret)(%rip), %rax
	cmp	%rax, 8+ISF64_RIP(%rsp)
	je	L_fault_iret
	leaq	EXT(ret64_iret)(%rip), %rax
	cmp	%rax, 8+ISF64_RIP(%rsp)
	je	L_fault_iret
	leaq	EXT(ret32_set_ds)(%rip), %rax
	cmp	%rax, 8+ISF64_RIP(%rsp)
	je	L_32bit_fault_set_seg
	leaq	EXT(ret32_set_es)(%rip), %rax
	cmp	%rax, 8+ISF64_RIP(%rsp)
	je	L_32bit_fault_set_seg
	leaq	EXT(ret32_set_fs)(%rip), %rax
	cmp	%rax, 8+ISF64_RIP(%rsp)
	je	L_32bit_fault_set_seg
	leaq	EXT(ret32_set_gs)(%rip), %rax
	cmp	%rax, 8+ISF64_RIP(%rsp)
	je	L_32bit_fault_set_seg
	jmp	EXT(ks_kernel_trap)
	/* Fall through */

Entry(ks_kernel_trap)
	/*
	 * Here after taking an unexpected trap from kernel mode - perhaps
	 * while running in the trampolines hereabouts.
	 * Note: %rax has been pushed on stack.
	 * Make sure we're not on the PCB stack, if so move to the kernel stack.
	 * This is likely a fatal condition.
	 * But first, ensure we have the kernel gs base active...
	 */
	push	%rcx
	push	%rdx
	mov	$(MSR_IA32_GS_BASE), %ecx
	rdmsr					/* read kernel gsbase */
	test	$0x80000000, %edx		/* test MSB of address */
	jne	1f
	swapgs					/* so swap */
1:
	pop	%rdx
	pop	%rcx

	movq	%gs:CPU_UBER_ISF, %rax		/* PCB stack addr */
	subq	%rsp, %rax
	cmpq	$(PAGE_SIZE), %rax		/* current stack in PCB? */
	jb	2f				/*  - yes, deal with it */
	pop	%rax				/*  - no, restore %rax */
	jmp	EXT(ks_dispatch_kernel)
2:
	/*
	 *  Here if %rsp is in the PCB
	 *  Copy the interrupt stack frame from PCB stack to kernel stack
	 */
	movq	%gs:CPU_KERNEL_STACK, %rax
	xchgq	%rax, %rsp
	pushq	8+ISF64_SS(%rax)
	pushq	8+ISF64_RSP(%rax)
	pushq	8+ISF64_RFLAGS(%rax)
	pushq	8+ISF64_CS(%rax)
	pushq	8+ISF64_RIP(%rax)
	pushq	8+ISF64_ERR(%rax)
	pushq	8+ISF64_TRAPFN(%rax)
	pushq	8+ISF64_TRAPNO(%rax)
	movq	(%rax), %rax
	jmp	EXT(ks_dispatch_kernel)


/*
 * GP/NP fault on IRET: CS or SS is in error.
 * User GSBASE is active.
 * On IST1 stack containing:
 *  (rax saved above, which is immediately popped)
 *  0  ISF64_TRAPNO:	trap code (NP or GP)
 *  8  ISF64_TRAPFN:	trap function
 *  16 ISF64_ERR:	segment number in error (error code)
 *  24 ISF64_RIP:	kernel RIP
 *  32 ISF64_CS:	kernel CS
 *  40 ISF64_RFLAGS:	kernel RFLAGS 
 *  48 ISF64_RSP:	kernel RSP
 *  56 ISF64_SS:	kernel SS
 * On the PCB stack, pointed to by the kernel's RSP is:
 *   0			user RIP
 *   8			user CS
 *  16			user RFLAGS
 *  24			user RSP
 *  32 			user SS
 *
 * We need to move the kernel's TRAPNO, TRAPFN and ERR to the PCB and handle
 * as a user fault with:
 *  0  ISF64_TRAPNO:	trap code (NP or GP)
 *  8  ISF64_TRAPFN:	trap function
 *  16 ISF64_ERR:	segment number in error (error code)
 *  24			user RIP
 *  32			user CS
 *  40			user RFLAGS
 *  48			user RSP
 *  56 			user SS
 */
L_fault_iret:
	pop	%rax			/* recover saved %rax */
	mov	%rax, ISF64_RIP(%rsp)	/* save rax (we don`t need saved rip) */
	mov	ISF64_RSP(%rsp), %rax
	xchg	%rax, %rsp		/* switch to PCB stack */
	push	ISF64_ERR(%rax)
	push	ISF64_TRAPFN(%rax)
	push	ISF64_TRAPNO(%rax)
	mov	ISF64_RIP(%rax), %rax	/* restore rax */
					/* now treat as fault from user */
	jmp	L_dispatch

/*
 * Fault restoring a segment register.  All of the saved state is still
 * on the stack untouched since we haven't yet moved the stack pointer.
 * On IST1 stack containing:
 *  (rax saved above, which is immediately popped)
 *  0  ISF64_TRAPNO:	trap code (NP or GP)
 *  8  ISF64_TRAPFN:	trap function
 *  16 ISF64_ERR:	segment number in error (error code)
 *  24 ISF64_RIP:	kernel RIP
 *  32 ISF64_CS:	kernel CS
 *  40 ISF64_RFLAGS:	kernel RFLAGS 
 *  48 ISF64_RSP:	kernel RSP
 *  56 ISF64_SS:	kernel SS
 * On the PCB stack, pointed to by the kernel's RSP is:
 *  0  			user trap code
 *  8  			user trap function
 *  16			user err 
 *  24			user RIP
 *  32			user CS
 *  40			user RFLAGS
 *  48			user RSP
 *  56 			user SS
 */
L_32bit_fault_set_seg:
	swapgs
	pop	%rax			/* toss saved %rax from stack */
	mov	ISF64_TRAPNO(%rsp), %rax
	mov	ISF64_TRAPFN(%rsp), %rcx
	mov	ISF64_ERR(%rsp), %rdx
	mov	ISF64_RSP(%rsp), %rsp	/* reset stack to saved state */
	mov	%rax,R64_TRAPNO(%rsp)
	mov	%rcx,R64_TRAPFN(%rsp)
	mov	%rdx,R64_ERR(%rsp)
					/* now treat as fault from user */
					/* except that all the state is */
					/* already saved - we just have to */
					/* move the trapno and error into */
					/* the compatibility frame */
	jmp	L_dispatch_U32_after_fault


Entry(ks_idt64_nmi_kernel)
	/* From user-space: copy interrupt state to user PCB */
	swapgs
	mov	%gs:CPU_UBER_ISF, %rcx		/* PCB stack addr */
	add	$(ISF64_SIZE), %rcx		/* adjust to base of ISF */	
	swapgs					/* swap back for L_dispatch */
	jmp	4f				/* Copy state to PCB */

1:
	/*
	* From kernel-space:
	 * Determine whether the kernel or user GS is set.
	 * Set the kernel and ensure that we'll swap back correctly at IRET.
	 */
	mov	$(MSR_IA32_GS_BASE), %ecx
	rdmsr					/* read kernel gsbase */
	test	$0x80000000, %edx		/* test MSB of address */
	jne	2f
	swapgs					/* so swap */
	movl	$1, ISF64_CS+4(%rsp)		/* and set flag in CS slot */
2:
	/*
	 * Determine whether we're on the kernel or interrupt stack
	 * when the NMI hit.
	 */
	mov	ISF64_RSP(%rsp), %rcx
	mov	%gs:CPU_KERNEL_STACK, %rax
	xor	%rcx, %rax
	and	EXT(kernel_stack_mask)(%rip), %rax
	test	%rax, %rax		/* are we on the kernel stack? */
	je	3f			/* yes */

	mov	%gs:CPU_INT_STACK_TOP, %rax
	dec	%rax			/* intr stack top is byte above max */
	xor	%rcx, %rax
	and	EXT(kernel_stack_mask)(%rip), %rax
	test	%rax, %rax		/* are we on the interrupt stack? */
	je	3f			/* yes */

	mov    %gs:CPU_KERNEL_STACK, %rcx
3:
	/* 16-byte-align kernel/interrupt stack for state push */
	and	$0xFFFFFFFFFFFFFFF0, %rcx

4:
	/*
	 * Copy state from NMI stack (RSP) to the save area (RCX) which is
	 * the PCB for user or kernel/interrupt stack from kernel.
	 * ISF64_ERR(RSP)    saved RAX
	 * ISF64_TRAPFN(RSP) saved RCX
	 * ISF64_TRAPNO(RSP) saved RDX
	 */
	xchg	%rsp, %rcx			/* set for pushes */
	push	ISF64_SS(%rcx)
	push	ISF64_RSP(%rcx)
	push	ISF64_RFLAGS(%rcx)
	push	ISF64_CS(%rcx)
	push	ISF64_RIP(%rcx)
	push	$(0)				/* error code 0 */
	push	$(HNDL_ALLINTRS)		/* trapfn allintrs */
	push	$(T_NMI)			/* trapno T_NMI */
	mov	ISF64_ERR(%rcx), %rax
	mov	ISF64_TRAPNO(%rcx), %rdx
	mov	ISF64_TRAPFN(%rcx), %rcx
	jmp	L_dispatch


/* All 'exceptions' enter hndl_alltraps, with:
 *	r15	x86_saved_state_t address
 *	rsp	kernel stack if user-space, otherwise interrupt or kernel stack
 *	esi	cs at trap
 * 
 * The rest of the state is set up as:	
 *	both rsp and r15 are 16-byte aligned
 *	interrupts disabled
 *	direction flag cleared
 */
Entry(hndl_alltraps)
	mov	%esi, %eax
	testb	$3, %al
	jz	trap_from_kernel

	TIME_TRAP_UENTRY

	/* Check for active vtimers in the current task */
	mov	%gs:CPU_ACTIVE_THREAD, %rcx
	movl	$-1, TH_IOTIER_OVERRIDE(%rcx)	/* Reset IO tier override to -1 before handling trap/exception */
	mov	TH_TASK(%rcx), %rbx
	TASK_VTIMER_CHECK(%rbx, %rcx)

	CCALL1(user_trap, %r15)			/* call user trap routine */
	/* user_trap() unmasks interrupts */
	cli					/* hold off intrs - critical section */
	xorl	%ecx, %ecx			/* don't check if we're in the PFZ */


Entry(return_from_trap)
	movq	%gs:CPU_ACTIVE_THREAD,%r15	/* Get current thread */
	movl	$-1, TH_IOTIER_OVERRIDE(%r15)	/* Reset IO tier override to -1 before returning to userspace */
	cmpl	$0, TH_RWLOCK_COUNT(%r15)	/* Check if current thread has pending RW locks held */
	jz	1f
	xorq	%rbp, %rbp		/* clear framepointer */
	mov	%r15, %rdi		/* Set RDI to current thread */
	CCALL(lck_rw_clear_promotions_x86)	/* Clear promotions if needed */
1:	
	movq	TH_PCB_ISS(%r15), %r15 		/* PCB stack */
	movl	%gs:CPU_PENDING_AST,%eax
	testl	%eax,%eax
	je	EXT(return_to_user)		/* branch if no AST */

L_return_from_trap_with_ast:
	testl	%ecx, %ecx		/* see if we need to check for an EIP in the PFZ */
	je	2f			/* no, go handle the AST */
	cmpl	$(SS_64), SS_FLAVOR(%r15)	/* are we a 64-bit task? */
	je	1f
					/* no... 32-bit user mode */
	movl	R32_EIP(%r15), %edi
	xorq	%rbp, %rbp		/* clear framepointer */
	CCALL(commpage_is_in_pfz32)
	testl	%eax, %eax
	je	2f			/* not in the PFZ... go service AST */
	movl	%eax, R32_EBX(%r15)	/* let the PFZ know we've pended an AST */
	jmp	EXT(return_to_user)
1:
	movq	R64_RIP(%r15), %rdi
	xorq	%rbp, %rbp		/* clear framepointer */
	CCALL(commpage_is_in_pfz64)
	testl	%eax, %eax
	je	2f			/* not in the PFZ... go service AST */
	movl	%eax, R64_RBX(%r15)	/* let the PFZ know we've pended an AST */
	jmp	EXT(return_to_user)
2:

	xorq	%rbp, %rbp		/* clear framepointer */
	CCALL(ast_taken_user)		/* handle all ASTs (enables interrupts, may return via continuation) */

	cli
	mov	%rsp, %r15		/* AST changes stack, saved state */
	xorl	%ecx, %ecx		/* don't check if we're in the PFZ */
	jmp	EXT(return_from_trap)	/* and check again (rare) */

/*
 * Trap from kernel mode.  No need to switch stacks.
 * Interrupts must be off here - we will set them to state at time of trap
 * as soon as it's safe for us to do so and not recurse doing preemption
 * 
 */
trap_from_kernel:
	movq	%r15, %rdi		/* saved state addr */
	pushq   R64_RIP(%r15)           /* Simulate a CALL from fault point */
	pushq   %rbp                    /* Extend framepointer chain */
	movq    %rsp, %rbp
	CCALLWITHSP(kernel_trap)	/* to kernel trap routine */
	popq    %rbp
	addq    $8, %rsp
	mov	%rsp, %r15		/* DTrace slides stack/saved-state */
	cli

	movl	%gs:CPU_PENDING_AST,%eax	/* get pending asts */
	testl	$(AST_URGENT),%eax		/* any urgent preemption? */
	je	ret_to_kernel			/* no, nothing to do */
	cmpl	$(T_PREEMPT),R64_TRAPNO(%r15)
	je	ret_to_kernel			/* T_PREEMPT handled in kernel_trap() */
	testl	$(EFL_IF),R64_RFLAGS(%r15)	/* interrupts disabled? */
	je	ret_to_kernel
	cmpl	$0,%gs:CPU_PREEMPTION_LEVEL	/* preemption disabled? */
	jne	ret_to_kernel
	movq	%gs:CPU_KERNEL_STACK,%rax
	movq	%rsp,%rcx
	xorq	%rax,%rcx
	andq	EXT(kernel_stack_mask)(%rip),%rcx
	testq	%rcx,%rcx		/* are we on the kernel stack? */
	jne	ret_to_kernel		/* no, skip it */

	CCALL(ast_taken_kernel)         /* take the AST */

	mov	%rsp, %r15		/* AST changes stack, saved state */
	jmp	ret_to_kernel


/*
 * All interrupts on all tasks enter here with:
 *	r15	 x86_saved_state_t
 *	rsp	 kernel or interrupt stack
 *	esi	 cs at trap
 *
 *	both rsp and r15 are 16-byte aligned
 *	interrupts disabled
 *	direction flag cleared
 */
Entry(hndl_allintrs)
	/*
	 * test whether already on interrupt stack
	 */
	movq	%gs:CPU_INT_STACK_TOP,%rcx
	cmpq	%rsp,%rcx
	jb	1f
	leaq	-INTSTACK_SIZE(%rcx),%rdx
	cmpq	%rsp,%rdx
	jb	int_from_intstack
1:
	xchgq	%rcx,%rsp		/* switch to interrupt stack */

	mov	%cr0,%rax		/* get cr0 */
	orl	$(CR0_TS),%eax		/* or in TS bit */
	mov	%rax,%cr0		/* set cr0 */

	pushq	%rcx			/* save pointer to old stack */
	pushq	%gs:CPU_INT_STATE	/* save previous intr state */
	movq	%r15,%gs:CPU_INT_STATE	/* set intr state */
	
	TIME_INT_ENTRY			/* do timing */

	/* Check for active vtimers in the current task */
	mov	%gs:CPU_ACTIVE_THREAD, %rcx
	mov	TH_TASK(%rcx), %rbx
	TASK_VTIMER_CHECK(%rbx, %rcx)

	incl	%gs:CPU_PREEMPTION_LEVEL
	incl	%gs:CPU_INTERRUPT_LEVEL

	CCALL1(interrupt, %r15)		/* call generic interrupt routine */

.globl	EXT(return_to_iret)
LEXT(return_to_iret)			/* (label for kdb_kintr and hardclock) */

	decl	%gs:CPU_INTERRUPT_LEVEL
	decl	%gs:CPU_PREEMPTION_LEVEL

	TIME_INT_EXIT			/* do timing */

	popq	%gs:CPU_INT_STATE 	/* reset/clear intr state pointer */
	popq	%rsp			/* switch back to old stack */

	movq	%gs:CPU_ACTIVE_THREAD,%rax
	movq	TH_PCB_FPS(%rax),%rax	/* get pcb's ifps */
	cmpq	$0,%rax			/* Is there a context */
	je	1f			/* Branch if not */
	movl	FP_VALID(%rax),%eax	/* Load fp_valid */
	cmpl	$0,%eax			/* Check if valid */
	jne	1f			/* Branch if valid */
	clts				/* Clear TS */
	jmp	2f
1:
	mov	%cr0,%rax		/* get cr0 */
	orl	$(CR0_TS),%eax		/* or in TS bit */
	mov	%rax,%cr0		/* set cr0 */
2:
	/* Load interrupted code segment into %eax */
	movl	R32_CS(%r15),%eax	/* assume 32-bit state */
	cmpl	$(SS_64),SS_FLAVOR(%r15)/* 64-bit? */	
#if DEBUG_IDT64
	jne	4f
	movl	R64_CS(%r15),%eax	/* 64-bit user mode */
	jmp	3f
4:
	cmpl    $(SS_32),SS_FLAVOR(%r15)
	je	3f
	POSTCODE2(0x6431)
	CCALL1(panic_idt64, %r15)
	hlt
#else
	jne	3f
	movl	R64_CS(%r15),%eax	/* 64-bit user mode */
#endif
3:
	testb	$3,%al			/* user mode, */
	jnz	ast_from_interrupt_user	/* go handle potential ASTs */
	/*
	 * we only want to handle preemption requests if
	 * the interrupt fell in the kernel context
	 * and preemption isn't disabled
	 */
	movl	%gs:CPU_PENDING_AST,%eax	
	testl	$(AST_URGENT),%eax		/* any urgent requests? */
	je	ret_to_kernel			/* no, nothing to do */

	cmpl	$0,%gs:CPU_PREEMPTION_LEVEL	/* preemption disabled? */
	jne	ret_to_kernel			/* yes, skip it */

	/*
	 * Take an AST from kernel space.  We don't need (and don't want)
	 * to do as much as the case where the interrupt came from user
	 * space.
	 */
	CCALL(ast_taken_kernel)

	mov	%rsp, %r15		/* AST changes stack, saved state */
	jmp	ret_to_kernel


/*
 * nested int - simple path, can't preempt etc on way out
 */
int_from_intstack:
	incl	%gs:CPU_PREEMPTION_LEVEL
	incl	%gs:CPU_INTERRUPT_LEVEL
	incl	%gs:CPU_NESTED_ISTACK

	push	%gs:CPU_INT_STATE
	mov	%r15, %gs:CPU_INT_STATE

	CCALL1(interrupt, %r15)

	pop	%gs:CPU_INT_STATE

	decl	%gs:CPU_INTERRUPT_LEVEL
	decl	%gs:CPU_PREEMPTION_LEVEL
	decl	%gs:CPU_NESTED_ISTACK

	jmp	ret_to_kernel

/*
 *	Take an AST from an interrupted user
 */
ast_from_interrupt_user:
	movl	%gs:CPU_PENDING_AST,%eax
	testl	%eax,%eax		/* pending ASTs? */
	je	EXT(ret_to_user)	/* no, nothing to do */

	TIME_TRAP_UENTRY

	movl	$1, %ecx		/* check if we're in the PFZ */
	jmp	L_return_from_trap_with_ast	/* return */


/* Syscall dispatch routines! */

/*
 *
 * 32bit Tasks
 * System call entries via INTR_GATE or sysenter:
 *
 *	r15	 x86_saved_state32_t
 *	rsp	 kernel stack
 *
 *	both rsp and r15 are 16-byte aligned
 *	interrupts disabled
 *	direction flag cleared
 */

Entry(hndl_sysenter)
	/*
	 * We can be here either for a mach syscall or a unix syscall,
	 * as indicated by the sign of the code:
	 */
	movl	R32_EAX(%r15),%eax
	testl	%eax,%eax
	js	EXT(hndl_mach_scall)		/* < 0 => mach */
						/* > 0 => unix */
	
Entry(hndl_unix_scall)

        TIME_TRAP_UENTRY

	movq	%gs:CPU_ACTIVE_THREAD,%rcx	/* get current thread     */
	movq	TH_TASK(%rcx),%rbx		/* point to current task  */
	incl	TH_SYSCALLS_UNIX(%rcx)		/* increment call count   */

	/* Check for active vtimers in the current task */
	TASK_VTIMER_CHECK(%rbx,%rcx)

	sti

	CCALL1(unix_syscall, %r15)
	/*
	 * always returns through thread_exception_return
	 */


Entry(hndl_mach_scall)
	TIME_TRAP_UENTRY

	movq	%gs:CPU_ACTIVE_THREAD,%rcx	/* get current thread     */
	movq	TH_TASK(%rcx),%rbx		/* point to current task  */
	incl	TH_SYSCALLS_MACH(%rcx)		/* increment call count   */

	/* Check for active vtimers in the current task */
	TASK_VTIMER_CHECK(%rbx,%rcx)

	sti

	CCALL1(mach_call_munger, %r15)
	/*
	 * always returns through thread_exception_return
	 */


Entry(hndl_mdep_scall)
	TIME_TRAP_UENTRY

	/* Check for active vtimers in the current task */
	movq	%gs:CPU_ACTIVE_THREAD,%rcx	/* get current thread     */
	movq	TH_TASK(%rcx),%rbx		/* point to current task  */
	TASK_VTIMER_CHECK(%rbx,%rcx)

	sti

	CCALL1(machdep_syscall, %r15)
	/*
	 * always returns through thread_exception_return
	 */

/*
 * 64bit Tasks
 * System call entries via syscall only:
 *
 *	r15	 x86_saved_state64_t
 *	rsp	 kernel stack
 *
 *	both rsp and r15 are 16-byte aligned
 *	interrupts disabled
 *	direction flag cleared
 */

Entry(hndl_syscall)
	TIME_TRAP_UENTRY

	movq	%gs:CPU_ACTIVE_THREAD,%rcx	/* get current thread     */
	movl	$-1, TH_IOTIER_OVERRIDE(%rcx)	/* Reset IO tier override to -1 before handling syscall */
	movq	TH_TASK(%rcx),%rbx		/* point to current task  */

	/* Check for active vtimers in the current task */
	TASK_VTIMER_CHECK(%rbx,%rcx)

	/*
	 * We can be here either for a mach, unix machdep or diag syscall,
	 * as indicated by the syscall class:
	 */
	movl	R64_RAX(%r15), %eax		/* syscall number/class */
	movl	%eax, %edx
	andl	$(SYSCALL_CLASS_MASK), %edx	/* syscall class */
	cmpl	$(SYSCALL_CLASS_MACH<<SYSCALL_CLASS_SHIFT), %edx
	je	EXT(hndl_mach_scall64)
	cmpl	$(SYSCALL_CLASS_UNIX<<SYSCALL_CLASS_SHIFT), %edx
	je	EXT(hndl_unix_scall64)
	cmpl	$(SYSCALL_CLASS_MDEP<<SYSCALL_CLASS_SHIFT), %edx
	je	EXT(hndl_mdep_scall64)
	cmpl	$(SYSCALL_CLASS_DIAG<<SYSCALL_CLASS_SHIFT), %edx
	je	EXT(hndl_diag_scall64)

	/* Syscall class unknown */
	sti
	CCALL3(i386_exception, $(EXC_SYSCALL), %rax, $1)
	/* no return */


Entry(hndl_unix_scall64)
	incl	TH_SYSCALLS_UNIX(%rcx)		/* increment call count   */
	sti

	CCALL1(unix_syscall64, %r15)
	/*
	 * always returns through thread_exception_return
	 */


Entry(hndl_mach_scall64)
	incl	TH_SYSCALLS_MACH(%rcx)		/* increment call count   */
	sti

	CCALL1(mach_call_munger64, %r15)
	/*
	 * always returns through thread_exception_return
	 */



Entry(hndl_mdep_scall64)
	sti

	CCALL1(machdep_syscall64, %r15)
	/*
	 * always returns through thread_exception_return
	 */

Entry(hndl_diag_scall64)
	CCALL1(diagCall64, %r15)	// Call diagnostics
	test	%eax, %eax		// What kind of return is this?
	je	1f			// - branch if bad (zero)
	jmp	EXT(return_to_user)	// Normal return, do not check asts...
1:
	sti
	CCALL3(i386_exception, $EXC_SYSCALL, $0x6000, $1)
	/* no return */
/* TODO assert at all 'C' entry points that we're never operating on the fault stack's alias mapping */
Entry(hndl_machine_check)
	/* Adjust SP and savearea to their canonical, non-aliased addresses */
	subq	EXT(dblmap_dist)(%rip), %rsp
	subq	EXT(dblmap_dist)(%rip), %r15
	CCALL1(panic_machine_check64, %r15)
	hlt

Entry(hndl_double_fault)
	subq	EXT(dblmap_dist)(%rip), %rsp
	subq	EXT(dblmap_dist)(%rip), %r15
	CCALL1(panic_double_fault64, %r15)
	hlt
