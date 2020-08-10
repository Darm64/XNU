/*
 * Copyright (c) 1999-2015 Apple Inc. All rights reserved.
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
/* Copyright (c) 1992 NeXT Computer, Inc.  All rights reserved.
 */

#include "SYS.h"

#if defined(__i386__)

/*
 * i386 needs custom assembly to transform the return from syscalls
 * into a proper stack for a function call out to cerror{,_nocancel}.
 */

LABEL(tramp_cerror)
	mov		%esp, %edx
	andl	$0xfffffff0, %esp
	subl	$16, %esp
	movl	%edx, 4(%esp)
	movl	%eax, (%esp)
	CALL_EXTERN(_cerror)
	movl	4(%esp), %esp
	ret

LABEL(tramp_cerror_nocancel)
	mov		%esp, %edx
	andl	$0xfffffff0, %esp
	subl	$16, %esp
	movl	%edx, 4(%esp)
	movl	%eax, (%esp)
	CALL_EXTERN(_cerror_nocancel)
	movl	4(%esp), %esp
	ret

LABEL(__sysenter_trap)
	popl %edx
	movl %esp, %ecx
	sysenter

	.globl _i386_get_ldt
	ALIGN
_i386_get_ldt:
	movl    $6,%eax
	MACHDEP_SYSCALL_TRAP
	jnb		2f
	jmp		tramp_cerror
2:	ret


	.globl _i386_set_ldt
	ALIGN
_i386_set_ldt:
	movl    $5,%eax
	MACHDEP_SYSCALL_TRAP
	jnb		2f
	jmp		tramp_cerror
2:	ret

	ALIGN
	.globl __thread_set_tsd_base
__thread_set_tsd_base:
	pushl   4(%esp)
	pushl   $0
	movl    $3,%eax
	MACHDEP_SYSCALL_TRAP
	addl    $8,%esp
	ret

#elif defined(__x86_64__)

	.globl _i386_get_ldt
	ALIGN
_i386_get_ldt:
	movl    $SYSCALL_CONSTRUCT_MDEP(6), %eax
	MACHDEP_SYSCALL_TRAP
	jnb		2f
	movq	%rax, %rdi
	jmp		_cerror
2:	ret


	.globl _i386_set_ldt
	ALIGN
_i386_set_ldt:
	movl    $SYSCALL_CONSTRUCT_MDEP(5), %eax
	MACHDEP_SYSCALL_TRAP
	jnb		2f
	movq	%rax, %rdi
	jmp		_cerror
2:	ret

	ALIGN
	.globl __thread_set_tsd_base
__thread_set_tsd_base:
	movl	$0, %esi	// 0 as the second argument
	movl    $ SYSCALL_CONSTRUCT_MDEP(3), %eax	// Machine-dependent syscall number 3
	MACHDEP_SYSCALL_TRAP
	ret

#elif defined(__arm__)

	.align 2
	.globl __thread_set_tsd_base
__thread_set_tsd_base:
	mov	r3, #2
	mov	r12, #0x80000000
	swi	#SWI_SYSCALL
	bx	lr

#elif defined(__arm64__)

	.align 2
	.globl __thread_set_tsd_base
__thread_set_tsd_base:
	mov	x3, #2
	mov	x16, #0x80000000
	svc 	#SWI_SYSCALL
	ret

#else
#error unknown architecture
#endif

.subsections_via_symbols
