/*
 * Copyright (c) 2000-2015 Apple Computer, Inc. All rights reserved.
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

#include <libkern/OSAtomic.h>
#include <kern/debug.h>
#include <machine/atomic.h>

enum {
	false   = 0,
	true    = 1
};

#ifndef NULL
#define NULL ((void *)0)
#endif

#define ATOMIC_DEBUG DEBUG

#if ATOMIC_DEBUG
#define ALIGN_TEST(p, t) do{if((uintptr_t)p&(sizeof(t)-1)) panic("Unaligned atomic pointer %p\n",p);}while(0)
#else
#define ALIGN_TEST(p, t) do{}while(0)
#endif

/*
 * atomic operations
 *	These are _the_ atomic operations, now implemented via compiler built-ins.
 *	It is expected that this C implementation is a candidate for Link-Time-
 *	Optimization inlining, whereas the assembler implementations they replace
 *	were not.
 */

#undef OSCompareAndSwap8
Boolean
OSCompareAndSwap8(UInt8 oldValue, UInt8 newValue, volatile UInt8 *address)
{
	return os_atomic_cmpxchg(address, oldValue, newValue, acq_rel);
}

#undef OSCompareAndSwap16
Boolean
OSCompareAndSwap16(UInt16 oldValue, UInt16 newValue, volatile UInt16 *address)
{
	return os_atomic_cmpxchg(address, oldValue, newValue, acq_rel);
}

#undef OSCompareAndSwap
Boolean
OSCompareAndSwap(UInt32 oldValue, UInt32 newValue, volatile UInt32 *address)
{
	ALIGN_TEST(address, UInt32);
	return os_atomic_cmpxchg(address, oldValue, newValue, acq_rel);
}

#undef OSCompareAndSwap64
Boolean
OSCompareAndSwap64(UInt64 oldValue, UInt64 newValue, volatile UInt64 *address)
{
	/*
	 * _Atomic uint64 requires 8-byte alignment on all architectures.
	 * This silences the compiler cast warning.  ALIGN_TEST() verifies
	 * that the cast was legal, if defined.
	 */
	_Atomic UInt64 *aligned_addr = (_Atomic UInt64 *)(uintptr_t)address;

	ALIGN_TEST(address, UInt64);
	return os_atomic_cmpxchg(aligned_addr, oldValue, newValue, acq_rel);
}

#undef OSCompareAndSwapPtr
Boolean
OSCompareAndSwapPtr(void *oldValue, void *newValue, void * volatile *address)
{
	return os_atomic_cmpxchg(address, oldValue, newValue, acq_rel);
}

SInt8
OSAddAtomic8(SInt32 amount, volatile SInt8 *address)
{
	return os_atomic_add_orig(address, amount, relaxed);
}

SInt16
OSAddAtomic16(SInt32 amount, volatile SInt16 *address)
{
	return os_atomic_add_orig(address, amount, relaxed);
}

#undef OSAddAtomic
SInt32
OSAddAtomic(SInt32 amount, volatile SInt32 *address)
{
	ALIGN_TEST(address, UInt32);
	return os_atomic_add_orig(address, amount, relaxed);
}

#undef OSAddAtomic64
SInt64
OSAddAtomic64(SInt64 amount, volatile SInt64 *address)
{
	_Atomic SInt64* aligned_address = (_Atomic SInt64*)(uintptr_t)address;

	ALIGN_TEST(address, SInt64);
	return os_atomic_add_orig(aligned_address, amount, relaxed);
}

#undef OSAddAtomicLong
long
OSAddAtomicLong(long theAmount, volatile long *address)
{
	return os_atomic_add_orig(address, theAmount, relaxed);
}

#undef OSIncrementAtomic
SInt32
OSIncrementAtomic(volatile SInt32 * value)
{
	return os_atomic_inc_orig(value, relaxed);
}

#undef OSDecrementAtomic
SInt32
OSDecrementAtomic(volatile SInt32 * value)
{
	return os_atomic_dec_orig(value, relaxed);
}

#undef OSBitAndAtomic
UInt32
OSBitAndAtomic(UInt32 mask, volatile UInt32 * value)
{
	return os_atomic_and_orig(value, mask, relaxed);
}

#undef OSBitOrAtomic
UInt32
OSBitOrAtomic(UInt32 mask, volatile UInt32 * value)
{
	return os_atomic_or_orig(value, mask, relaxed);
}

#undef OSBitXorAtomic
UInt32
OSBitXorAtomic(UInt32 mask, volatile UInt32 * value)
{
	return os_atomic_xor_orig(value, mask, relaxed);
}

static Boolean
OSTestAndSetClear(UInt32 bit, Boolean wantSet, volatile UInt8 * startAddress)
{
	UInt8           mask = 1;
	UInt8           oldValue, newValue;
	UInt8           wantValue;
	UInt8           *address;

	address = (UInt8 *)(uintptr_t)(startAddress + (bit / 8));
	mask <<= (7 - (bit % 8));
	wantValue = wantSet ? mask : 0;

	return !os_atomic_rmw_loop(address, oldValue, newValue, relaxed, {
		if ((oldValue & mask) == wantValue) {
		        os_atomic_rmw_loop_give_up(break);
		}
		newValue = (oldValue & ~mask) | wantValue;
	});
}

Boolean
OSTestAndSet(UInt32 bit, volatile UInt8 * startAddress)
{
	return OSTestAndSetClear(bit, true, startAddress);
}

Boolean
OSTestAndClear(UInt32 bit, volatile UInt8 * startAddress)
{
	return OSTestAndSetClear(bit, false, startAddress);
}

/*
 * silly unaligned versions
 */

SInt8
OSIncrementAtomic8(volatile SInt8 * value)
{
	return os_atomic_inc_orig(value, relaxed);
}

SInt8
OSDecrementAtomic8(volatile SInt8 * value)
{
	return os_atomic_dec_orig(value, relaxed);
}

UInt8
OSBitAndAtomic8(UInt32 mask, volatile UInt8 * value)
{
	return os_atomic_and_orig(value, mask, relaxed);
}

UInt8
OSBitOrAtomic8(UInt32 mask, volatile UInt8 * value)
{
	return os_atomic_or_orig(value, mask, relaxed);
}

UInt8
OSBitXorAtomic8(UInt32 mask, volatile UInt8 * value)
{
	return os_atomic_xor_orig(value, mask, relaxed);
}

SInt16
OSIncrementAtomic16(volatile SInt16 * value)
{
	return OSAddAtomic16(1, value);
}

SInt16
OSDecrementAtomic16(volatile SInt16 * value)
{
	return OSAddAtomic16(-1, value);
}

UInt16
OSBitAndAtomic16(UInt32 mask, volatile UInt16 * value)
{
	return os_atomic_and_orig(value, mask, relaxed);
}

UInt16
OSBitOrAtomic16(UInt32 mask, volatile UInt16 * value)
{
	return os_atomic_or_orig(value, mask, relaxed);
}

UInt16
OSBitXorAtomic16(UInt32 mask, volatile UInt16 * value)
{
	return os_atomic_xor_orig(value, mask, relaxed);
}
