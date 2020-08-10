/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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
#include <kern/thread.h>
#include <kern/spl.h>
#include <machine/machine_routines.h>

/*
 *  spl routines
 */

__private_extern__ spl_t
splhigh(
	void)
{
	return ml_set_interrupts_enabled(FALSE);
}

__private_extern__ spl_t
splsched(
	void)
{
	return ml_set_interrupts_enabled(FALSE);
}

__private_extern__ spl_t
splclock(
	void)
{
	return ml_set_interrupts_enabled(FALSE);
}

__private_extern__ void
spllo(
	void)
{
	(void)ml_set_interrupts_enabled(TRUE);
}

__private_extern__ void
splx(
	spl_t l)
{
	ml_set_interrupts_enabled((boolean_t) l);
}
