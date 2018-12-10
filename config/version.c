/*
 * Copyright (c) 2004-2006 Apple Computer, Inc. All rights reserved.
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

/* version.c
 * This file is a C source template for version.c, which is generated
 * on every make of xnu.  This template is processed by the script
 * xnu/config/newvers.pl based on the version information in the file
 * xnu/config/MasterVersion.
 */

#include <libkern/version.h>

// for what(1):
const char __kernelVersionString[] __attribute__((used)) = "@(#)VERSION: " OSTYPE " Kernel Version ###KERNEL_VERSION_LONG###: ###KERNEL_BUILD_DATE###; ###KERNEL_BUILDER###:###KERNEL_BUILD_OBJROOT###";
const char version[] = OSTYPE " Kernel Version ###KERNEL_VERSION_LONG###: ###KERNEL_BUILD_DATE###; ###KERNEL_BUILDER###:###KERNEL_BUILD_OBJROOT###";
const int  version_major = VERSION_MAJOR;
const int  version_minor = VERSION_MINOR;
const int  version_revision = VERSION_REVISION;
const int  version_stage = VERSION_STAGE;
const int  version_prerelease_level = VERSION_PRERELEASE_LEVEL;
const char version_variant[] = VERSION_VARIANT;
const char osbuild_config[] = "###KERNEL_BUILD_CONFIG###";
const char osbuilder[] = "###KERNEL_BUILDER###";
const char osrelease[] = OSRELEASE;
const char ostype[] = OSTYPE;
char osversion[OSVERSIZE];

__private_extern__ const char compiler_version[] = __VERSION__;
