/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */


#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>
#endif

const char *
getexecname(void)
{
	static char execname[PATH_MAX + 1] = "";
	static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	char *ptr = NULL;
	ssize_t rc;

	(void) pthread_mutex_lock(&mtx);

	if (strlen(execname) == 0) {
#ifdef __FreeBSD__
		int error, name[4];
		size_t len;

		name[0] = CTL_KERN;
		name[1] = KERN_PROC;
		name[2] = KERN_PROC_PATHNAME;
		name[3] = -1;
		len = PATH_MAX;
		error = sysctl(name, nitems(name), execname, &len, NULL, 0);
		if (error != 0) {
			rc = -1;
		} else {
			rc = len;
		}
#else
		rc = readlink("/proc/self/exe",
		    execname, sizeof (execname) - 1);
#endif
		if (rc == -1) {
			execname[0] = '\0';
		} else {
			execname[rc] = '\0';
			ptr = execname;
		}
	} else {
		ptr = execname;
	}

	(void) pthread_mutex_unlock(&mtx);
	return (ptr);
}
