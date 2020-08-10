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
#ifndef _PEXPERT_PROTOS_H_
#define _PEXPERT_PROTOS_H_

#ifdef PEXPERT_KERNEL_PRIVATE


#include <mach/mach_types.h>
#include <mach/vm_types.h>
#include <mach/boolean.h>
#include <stdarg.h>
#include <string.h>
#include <kern/assert.h>

#include <pexpert/machine/protos.h>

//------------------------------------------------------------------------
// from ppc/misc_protos.h
extern void printf(const char *fmt, ...);

extern void interrupt_enable(void);
extern void interrupt_disable(void);
#define bcopy_nc bcopy

//------------------------------------------------------------------------
//from kern/misc_protos.h
extern void
_doprnt(
	const char     *fmt,
	va_list                 *argp,
	void                    (*putc)(char),
	int                     radix);

extern void
_doprnt_log(
	const char     *fmt,
	va_list                 *argp,
	void                    (*putc)(char),
	int                     radix);

#include <machine/io_map_entries.h>

//------------------------------------------------------------------------
// ??
//typedef int kern_return_t;
void Debugger(const char *message);

#include <kern/cpu_number.h>
#include <kern/cpu_data.h>

//------------------------------------------------------------------------
// from kgdb/kgdb_defs.h
#define kgdb_printf printf

#include <mach/machine/vm_types.h>
#include <device/device_types.h>
#include <kern/kalloc.h>

//------------------------------------------------------------------------

// from iokit/IOStartIOKit.cpp
extern void StartIOKit( void * p1, void * p2, void * p3, void * p4);

// from iokit/Families/IOFramebuffer.cpp
extern unsigned char appleClut8[256 * 3];


#endif /* PEXPERT_KERNEL_PRIVATE */

#endif /* _PEXPERT_PROTOS_H_ */
