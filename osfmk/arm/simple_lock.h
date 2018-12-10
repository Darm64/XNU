/*
 * Copyright (c) 2007 Apple Inc. All rights reserved.
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

#ifdef	KERNEL_PRIVATE

#ifndef	_ARM_SIMPLE_LOCK_TYPES_H_
#define	_ARM_SIMPLE_LOCK_TYPES_H_

#ifdef	KERNEL_PRIVATE
#include <mach/boolean.h>
#include <kern/kern_types.h>

#include <sys/appleapiopts.h>
#ifdef  MACH_KERNEL_PRIVATE
#include <arm/hw_lock_types.h>
#include <arm/locks.h>
#include <mach_ldebug.h>
#endif

#ifdef MACH_KERNEL_PRIVATE

typedef uint32_t hw_lock_bit_t;

extern void	hw_lock_bit(
				hw_lock_bit_t *,
				unsigned int);

extern void	hw_lock_bit_nopreempt(
				hw_lock_bit_t *,
				unsigned int);

extern void	hw_unlock_bit(
				hw_lock_bit_t *,
				unsigned int);

extern void	hw_unlock_bit_nopreempt(
				hw_lock_bit_t *,
				unsigned int);

extern unsigned int hw_lock_bit_try(
				hw_lock_bit_t *,
				unsigned int);

extern unsigned int hw_lock_bit_to(
				hw_lock_bit_t *,
				unsigned int,
				uint32_t);

#define hw_lock_bit_held(l,b) (((*(l))&(1<<b))!=0)


extern uint32_t LockTimeOut;			/* Number of hardware ticks of a lock timeout */
extern uint32_t LockTimeOutUsec;		/* Number of microseconds for lock timeout */

/*
 * USLOCK_DEBUG is broken on ARM and has been disabled.
 * There are no callers to any of the usld_lock functions and data structures
 * don't match between between usimple_lock_data_t and lck_spin_t
*/

/*
#if MACH_LDEBUG
#define USLOCK_DEBUG 1
#else
#define USLOCK_DEBUG 0
#endif
*/

#if     !USLOCK_DEBUG

typedef lck_spin_t usimple_lock_data_t, *usimple_lock_t;

#else

typedef struct uslock_debug {
	void			*lock_pc;	/* pc where lock operation began    */
	void			*lock_thread;	/* thread that acquired lock */
	unsigned long	duration[2];
	unsigned short	state;
	unsigned char	lock_cpu;
	void			*unlock_thread;	/* last thread to release lock */
	unsigned char	unlock_cpu;
	void			*unlock_pc;	/* pc where lock operation ended    */
} uslock_debug;

typedef struct {
	hw_lock_data_t	interlock;	/* must be first... see lock.c */
	unsigned short	lock_type;	/* must be second... see lock.c */
#define USLOCK_TAG	0x5353
	uslock_debug	debug;
} usimple_lock_data_t, *usimple_lock_t;

#endif	/* USLOCK_DEBUG */

#else

#if defined(__arm__)
typedef	struct slock {
	unsigned int	lock_data[10];
} usimple_lock_data_t, *usimple_lock_t;
#elif defined(__arm64__)
/* 
 * ARM64_TODO: this is quite a waste of space (and a 
 * poorly packed data structure).  See if anyone's 
 * using these outside of osfmk.
 * NOTE: only osfmk uses this structure in xnu-2624
 */
typedef	struct slock {
	uint64_t lock_data[9];
} usimple_lock_data_t, *usimple_lock_t;
#else
#error Unknown architecture.
#endif

#endif	/* MACH_KERNEL_PRIVATE */

#define	USIMPLE_LOCK_NULL	((usimple_lock_t) 0)

#if !defined(decl_simple_lock_data)

typedef usimple_lock_data_t	*simple_lock_t;
typedef usimple_lock_data_t	simple_lock_data_t;

#define	decl_simple_lock_data(class,name) \
	class	simple_lock_data_t	name;

#endif	/* !defined(decl_simple_lock_data) */

#ifdef	MACH_KERNEL_PRIVATE

#define MACHINE_SIMPLE_LOCK

extern void	arm_usimple_lock_init(simple_lock_t, __unused unsigned short);

#define simple_lock_init(l,t)	arm_usimple_lock_init(l,t)
#define simple_lock(l)			lck_spin_lock(l)
#define simple_lock_nopreempt(l)	lck_spin_lock_nopreempt(l)
#define simple_unlock(l)		lck_spin_unlock(l)
#define simple_unlock_nopreempt(l)	lck_spin_unlock_nopreempt(l)
#define simple_lock_try(l)		lck_spin_try_lock(l)
#define simple_lock_try_nopreempt(l)	lck_spin_try_lock_nopreempt(l)
#define simple_lock_try_lock_loop(l)	simple_lock(l)
#define simple_lock_addr(l)		(&(l))
#define simple_lock_assert(l,t)	lck_spin_assert(l,t)
#define kdp_simple_lock_is_acquired(l) kdp_lck_spin_is_acquired(l)

#endif	/* MACH_KERNEL_PRIVATE */
#endif	/* KERNEL_PRIVATE */

#endif /* !_ARM_SIMPLE_LOCK_TYPES_H_ */

#endif	/* KERNEL_PRIVATE */
