/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#define	__APPLE_API_PRIVATE
#include <machine/cpu_capabilities.h>
#undef	__APPLE_API_PRIVATE

#if defined(__i386__) || defined(__x86_64__)

/* Initialize the "_cpu_capabilities" vector on x86 processors. */

int _cpu_has_altivec = 0;     // DEPRECATED
int _cpu_capabilities = 0;

void
_init_cpu_capabilities( void )
{
	_cpu_capabilities = (int)_get_cpu_capabilities();
}

#elif defined(__arm__) || defined(__arm64__)

extern int _get_cpu_capabilities(void);

int _cpu_capabilities = 0;
int _cpu_has_altivec = 0;		// DEPRECATED: use _cpu_capabilities instead

void
_init_cpu_capabilities( void )
{
}

#endif
