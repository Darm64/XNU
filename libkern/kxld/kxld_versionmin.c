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
#include "kxld_versionmin.h"

/*******************************************************************************
*******************************************************************************/
void
kxld_versionmin_init_from_macho(KXLDversionmin *versionmin, struct version_min_command *src)
{
	check(versionmin);
	check(src);
	check((src->cmd == LC_VERSION_MIN_MACOSX) || (src->cmd == LC_VERSION_MIN_IPHONEOS) || (src->cmd == LC_VERSION_MIN_TVOS) || (src->cmd == LC_VERSION_MIN_WATCHOS));

	switch (src->cmd) {
	case LC_VERSION_MIN_MACOSX:
		versionmin->platform = kKxldVersionMinMacOSX;
		break;
	case LC_VERSION_MIN_IPHONEOS:
		versionmin->platform = kKxldVersionMiniPhoneOS;
		break;
	case LC_VERSION_MIN_TVOS:
		versionmin->platform = kKxldVersionMinAppleTVOS;
		break;
	case LC_VERSION_MIN_WATCHOS:
		versionmin->platform = kKxldVersionMinWatchOS;
		break;
	}

	versionmin->version = src->version;
	versionmin->has_versionmin = TRUE;
}

void
kxld_versionmin_init_from_build_cmd(KXLDversionmin *versionmin, struct build_version_command *src)
{
	check(versionmin);
	check(src);
	switch (src->platform) {
	case PLATFORM_MACOS:
		versionmin->platform = kKxldVersionMinMacOSX;
		break;
	case PLATFORM_IOS:
		versionmin->platform = kKxldVersionMiniPhoneOS;
		break;
	case PLATFORM_TVOS:
		versionmin->platform = kKxldVersionMinAppleTVOS;
		break;
	case PLATFORM_WATCHOS:
		versionmin->platform = kKxldVersionMinWatchOS;
		break;
	default:
		return;
	}
	versionmin->version = src->minos;
	versionmin->has_versionmin = TRUE;
}

/*******************************************************************************
*******************************************************************************/
void
kxld_versionmin_clear(KXLDversionmin *versionmin)
{
	bzero(versionmin, sizeof(*versionmin));
}

/*******************************************************************************
*******************************************************************************/
u_long
kxld_versionmin_get_macho_header_size(__unused const KXLDversionmin *versionmin)
{
	/* TODO: eventually we can just use struct build_version_command */
	return sizeof(struct version_min_command);
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_versionmin_export_macho(const KXLDversionmin *versionmin, u_char *buf,
    u_long *header_offset, u_long header_size)
{
	kern_return_t rval = KERN_FAILURE;
	struct version_min_command *versionminhdr = NULL;

	check(versionmin);
	check(buf);
	check(header_offset);


	require_action(sizeof(*versionminhdr) <= header_size - *header_offset, finish,
	    rval = KERN_FAILURE);
	versionminhdr = (struct version_min_command *) ((void *) (buf + *header_offset));
	bzero(versionminhdr, sizeof(*versionminhdr));
	*header_offset += sizeof(*versionminhdr);

	switch (versionmin->platform) {
	case kKxldVersionMinMacOSX:
		versionminhdr->cmd = LC_VERSION_MIN_MACOSX;
		break;
	case kKxldVersionMiniPhoneOS:
		versionminhdr->cmd = LC_VERSION_MIN_IPHONEOS;
		break;
	case kKxldVersionMinAppleTVOS:
		versionminhdr->cmd = LC_VERSION_MIN_TVOS;
		break;
	case kKxldVersionMinWatchOS:
		versionminhdr->cmd = LC_VERSION_MIN_WATCHOS;
		break;
	default:
		goto finish;
	}
	versionminhdr->cmdsize = (uint32_t) sizeof(*versionminhdr);
	versionminhdr->version = versionmin->version;
	versionminhdr->sdk = 0;

	rval = KERN_SUCCESS;

finish:
	return rval;
}
