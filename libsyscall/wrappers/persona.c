/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
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
#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/persona.h>

#include "strings.h"

/* syscall entry point */
int __persona(uint32_t operation, uint32_t flags, struct kpersona_info *info, uid_t *id, size_t *idlen, char *path);

int
kpersona_alloc(struct kpersona_info *info, uid_t *id)
{
	size_t idlen = 1;
	return __persona(PERSONA_OP_ALLOC, 0, info, id, &idlen, NULL);
}

int
kpersona_palloc(struct kpersona_info *info, uid_t *id, char path[MAXPATHLEN])
{
	size_t idlen = 1;
	return __persona(PERSONA_OP_PALLOC, 0, info, id, &idlen, path);
}

int
kpersona_dealloc(uid_t id)
{
	size_t idlen = 1;
	return __persona(PERSONA_OP_DEALLOC, 0, NULL, &id, &idlen, NULL);
}

int
kpersona_get(uid_t *id)
{
	size_t idlen = 1;
	return __persona(PERSONA_OP_GET, 0, NULL, id, &idlen, NULL);
}

int
kpersona_getpath(uid_t id, char path[MAXPATHLEN])
{
	size_t idlen = 1;
	return __persona(PERSONA_OP_GETPATH, 0, NULL, &id, &idlen, path);
}

int
kpersona_info(uid_t id, struct kpersona_info *info)
{
	size_t idlen = 1;
	return __persona(PERSONA_OP_INFO, 0, info, &id, &idlen, NULL);
}

int
kpersona_pidinfo(pid_t pid, struct kpersona_info *info)
{
	size_t idlen = 1;
	uid_t id = (uid_t)pid;
	return __persona(PERSONA_OP_PIDINFO, 0, info, &id, &idlen, NULL);
}

int
kpersona_find(const char *name, uid_t uid, uid_t *id, size_t *idlen)
{
	int ret;
	struct kpersona_info kinfo;
	kinfo.persona_info_version = PERSONA_INFO_V1;
	kinfo.persona_id = uid;
	kinfo.persona_type = 0;
	kinfo.persona_gid = 0;
	kinfo.persona_ngroups = 0;
	kinfo.persona_groups[0] = 0;
	kinfo.persona_name[0] = 0;
	if (name) {
		strlcpy(kinfo.persona_name, name, sizeof(kinfo.persona_name));
	}
	ret = __persona(PERSONA_OP_FIND, 0, &kinfo, id, idlen, NULL);
	if (ret < 0) {
		return ret;
	}
	return (int)(*idlen);
}

int
kpersona_find_by_type(int persona_type, uid_t *id, size_t *idlen)
{
	int ret;
	struct kpersona_info kinfo;
	kinfo.persona_info_version = PERSONA_INFO_V1;
	kinfo.persona_type = persona_type;
	kinfo.persona_id = -1;
	kinfo.persona_gid = 0;
	kinfo.persona_ngroups = 0;
	kinfo.persona_groups[0] = 0;
	kinfo.persona_name[0] = 0;
	ret = __persona(PERSONA_OP_FIND_BY_TYPE, 0, &kinfo, id, idlen, NULL);
	if (ret < 0) {
		return ret;
	}
	return (int)(*idlen);
}
