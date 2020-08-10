/*
 * Copyright (c) 2003,2013,2017 Apple Inc. All rights reserved.
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

#include "kpi_interfacefilter.h"

#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/kern_event.h>
#include <net/dlil.h>

#undef iflt_attach
errno_t
iflt_attach(
	ifnet_t interface,
	const struct iff_filter *filter,
	interface_filter_t *filter_ref);


errno_t
iflt_attach_internal(
	ifnet_t interface,
	const struct iff_filter *filter,
	interface_filter_t *filter_ref)
{
	if (interface == NULL) {
		return ENOENT;
	}

	return dlil_attach_filter(interface, filter, filter_ref,
	           DLIL_IFF_INTERNAL);
}

errno_t
iflt_attach(
	ifnet_t interface,
	const struct iff_filter *filter,
	interface_filter_t *filter_ref)
{
	if (interface == NULL) {
		return ENOENT;
	}

	return dlil_attach_filter(interface, filter, filter_ref, 0);
}

void
iflt_detach(
	interface_filter_t filter_ref)
{
	dlil_detach_filter(filter_ref);
}
