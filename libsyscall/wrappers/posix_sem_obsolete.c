/*
 * Copyright (c) 2013 Apple Inc. All rights reserved.
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

#include <sys/errno.h>
#include <sys/semaphore.h>

/*
 * system call stubs are no longer generated for these from
 * syscalls.master. Instead, provide simple stubs here.
 */

int
sem_destroy(sem_t *s __unused)
{
	errno = ENOSYS;
	return -1;
}

int
sem_getvalue(sem_t * __restrict __unused s, int * __restrict __unused x)
{
	errno = ENOSYS;
	return -1;
}

int
sem_init(sem_t * __unused s, int __unused x, unsigned int __unused y)
{
	errno = ENOSYS;
	return -1;
}
