/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
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
#include <string.h>
#include <mach-o/loader.h>
#include <sys/types.h>

#define DEBUG_ASSERT_COMPONENT_NAME_STRING "kxld"
#include <AssertMacros.h>

#include "kxld_util.h"
#include "kxld_srcversion.h"

/*******************************************************************************
*******************************************************************************/
void
kxld_srcversion_init_from_macho(KXLDsrcversion *srcversion, struct source_version_command *src)
{
	check(srcversion);
	check(src);

	srcversion->version = src->version;
	srcversion->has_srcversion = TRUE;
}

/*******************************************************************************
*******************************************************************************/
void
kxld_srcversion_clear(KXLDsrcversion *srcversion)
{
	bzero(srcversion, sizeof(*srcversion));
}

/*******************************************************************************
*******************************************************************************/
u_long
kxld_srcversion_get_macho_header_size(void)
{
	return sizeof(struct source_version_command);
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_srcversion_export_macho(const KXLDsrcversion *srcversion, u_char *buf,
    u_long *header_offset, u_long header_size)
{
	kern_return_t rval = KERN_FAILURE;
	struct source_version_command *srcversionhdr = NULL;

	check(srcversion);
	check(buf);
	check(header_offset);

	require_action(sizeof(*srcversionhdr) <= header_size - *header_offset, finish,
	    rval = KERN_FAILURE);
	srcversionhdr = (struct source_version_command *) ((void *) (buf + *header_offset));
	*header_offset += sizeof(*srcversionhdr);

	srcversionhdr->cmd = LC_SOURCE_VERSION;
	srcversionhdr->cmdsize = (uint32_t) sizeof(*srcversionhdr);
	srcversionhdr->version = srcversion->version;

	rval = KERN_SUCCESS;

finish:
	return rval;
}
