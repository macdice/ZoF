/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 * Copyright 2013 Xin Li <delphij@FreeBSD.org>. All rights reserved.
 * Copyright 2013 Martin Matuska <mm@FreeBSD.org>. All rights reserved.
 * Portions Copyright 2005, 2010, Oracle and/or its affiliates.
 * All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cred.h>
#include <sys/dmu.h>
#include <sys/zio.h>
#include <sys/nvpair.h>
#include <sys/dsl_deleg.h>
#include <sys/zfs_ioctl.h>
#include "zfs_namecheck.h"
#include <os/freebsd/zfs/sys/zfs_ioctl_compat.h>

/*
 * FreeBSD zfs_cmd compatibility with older binaries
 * appropriately remap/extend the zfs_cmd_t structure
 */
void
zfs_cmd_compat_get(zfs_cmd_t *zc, caddr_t addr, const int cflag)
{

}

int
zcmd_ioctl_compat(int fd, int request, zfs_cmd_t *zc, const int cflag)
{
	int ret;
	void *zc_c;
	unsigned long ncmd;
	zfs_iocparm_t zp;

	switch (cflag) {
	case ZFS_CMD_COMPAT_NONE:
		ncmd = _IOWR('Z', request, zfs_iocparm_t);
		zp.zfs_cmd = (uint64_t)zc;
		zp.zfs_cmd_size = sizeof (zfs_cmd_t);
		zp.zfs_ioctl_version = ZFS_IOCVER_ZOF;
		return (ioctl(fd, ncmd, &zp));
	default:
		abort();
		return (EINVAL);
	}

	ret = ioctl(fd, ncmd, zc_c);
	zfs_cmd_compat_get(zc, (caddr_t)zc_c, cflag);
	free(zc_c);

	return (ret);
}

/*
 * This is FreeBSD version of ioctl, because Solaris' ioctl() updates
 * zc_nvlist_dst_size even if an error is returned, on FreeBSD if an
 * error is returned zc_nvlist_dst_size won't be updated.
 */
int
zcmd_ioctl(int fd, unsigned long request, zfs_cmd_t *zc)
{
	size_t oldsize;
	int ret, cflag = ZFS_CMD_COMPAT_NONE;

	oldsize = zc->zc_nvlist_dst_size;
	ret = zcmd_ioctl_compat(fd, request, zc, cflag);

	if (ret == 0 && oldsize < zc->zc_nvlist_dst_size) {
		ret = -1;
		errno = ENOMEM;
	}

	return (ret);
}
