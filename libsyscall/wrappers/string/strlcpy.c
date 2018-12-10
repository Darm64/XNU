/*
 * Copyright (c) 2011 Apple, Inc. All rights reserved.
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

#include "strings.h"

__attribute__((visibility("hidden")))
size_t
_libkernel_strlcpy(char * restrict dst, const char * restrict src, size_t maxlen) {
    const size_t srclen = _libkernel_strlen(src);
    if (srclen < maxlen) {
        _libkernel_memmove(dst, src, srclen+1);
    } else if (maxlen != 0) {
        _libkernel_memmove(dst, src, maxlen-1);
        dst[maxlen-1] = '\0';
    }
    return srclen;
}
