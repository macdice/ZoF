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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 * Copyright (c) 2006-2010 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * All rights reserved.
 *
 * Portions Copyright 2010 Robert Milkowski
 *
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2012, 2017 by Delphix. All rights reserved.
 * Copyright (c) 2013, Joyent, Inc. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 */

/* Portions Copyright 2011 Martin Matuska <mm@FreeBSD.org> */

/*
 * ZFS volume emulation driver.
 *
 * Makes a DMU object look like a volume of arbitrary size, up to 2^64 bytes.
 * Volumes are accessed through the symbolic links named:
 *
 * /dev/zvol/dsk/<pool_name>/<dataset_name>
 * /dev/zvol/rdsk/<pool_name>/<dataset_name>
 *
 * These links are created by the /dev filesystem (sdev_zvolops.c).
 * Volumes are persistent through reboot.  No user command needs to be
 * run before opening and using a device.
 *
 * FreeBSD notes.
 * On FreeBSD ZVOLs are simply GEOM providers like any other storage device
 * in the system.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/kmem.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/zap.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/disk.h>
#include <sys/dmu_traverse.h>
#include <sys/dnode.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_dir.h>
#include <sys/dkio.h>
#include <sys/byteorder.h>
#include <sys/sunddi.h>
#include <sys/dirent.h>
#include <sys/policy.h>
#include <sys/queue.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_ioctl.h>
#include <sys/zil.h>
#include <sys/refcount.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_rlock.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zvol.h>
#include <sys/zil_impl.h>
#include <sys/dbuf.h>
#include <sys/dmu_tx.h>
#include <sys/zfeature.h>
#include <sys/zio_checksum.h>
#include <sys/zil_impl.h>
#include <sys/filio.h>

#include <geom/geom.h>
#include <sys/zvol.h>
#include <sys/zvol_impl.h>

#include "zfs_namecheck.h"

struct proc *zfsproc;
extern uint_t zfs_geom_probe_vdev_key;
extern volatile int geom_inhibited;

/*
 * This lock protects the zfsdev_state structure from being modified
 * while it's being used, e.g. an open that comes in before a create
 * finishes.  It also protects temporary opens of the dataset so that,
 * e.g., an open doesn't get a spurious EBUSY.
 */

struct g_class zfs_zvol_class = {
	.name = "ZFS::ZVOL",
	.version = G_VERSION,
};

DECLARE_GEOM_CLASS(zfs_zvol_class, zfs_zvol);

#define	ZVOL_DUMPSIZE		"dumpsize"

static uint32_t zvol_minors;

#ifdef ZVOL_LOCK_DEBUG
#define ZVOL_RW_READER RW_WRITER
#else
#define ZVOL_RW_READER RW_READER
#endif

SYSCTL_DECL(_vfs_zfs);
SYSCTL_NODE(_vfs_zfs, OID_AUTO, vol, CTLFLAG_RW, 0, "ZFS VOLUME");
SYSCTL_INT(_vfs_zfs_vol, OID_AUTO, mode, CTLFLAG_RWTUN, &zvol_volmode, 0,
    "Expose as GEOM providers (1), device files (2) or neither");
static boolean_t zpool_on_zvol = B_FALSE;
SYSCTL_INT(_vfs_zfs_vol, OID_AUTO, recursive, CTLFLAG_RWTUN, &zpool_on_zvol, 0,
    "Allow zpools to use zvols as vdevs (DANGEROUS)");

typedef struct zvol_extent {
	list_node_t	ze_node;
	dva_t		ze_dva;		/* dva associated with this extent */
	uint64_t	ze_nblks;	/* number of blocks in extent */
} zvol_extent_t;

/*
 * zvol maximum transfer in one DMU tx.
 */
int zvol_maxphys = DMU_MAX_ACCESS/2;

/*
 * Toggle unmap functionality.
 */
boolean_t zvol_unmap_enabled = B_TRUE;

SYSCTL_INT(_vfs_zfs_vol, OID_AUTO, unmap_enabled, CTLFLAG_RWTUN,
    &zvol_unmap_enabled, 0,
    "Enable UNMAP functionality");

static d_open_t		zvol_d_open;
static d_close_t	zvol_d_close;
static d_read_t		zvol_read;
static d_write_t	zvol_write;
static d_ioctl_t	zvol_d_ioctl;
static d_strategy_t	zvol_strategy;

static struct cdevsw zvol_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	zvol_d_open,
	.d_close =	zvol_d_close,
	.d_read =	zvol_read,
	.d_write =	zvol_write,
	.d_ioctl =	zvol_d_ioctl,
	.d_strategy =	zvol_strategy,
	.d_name =	"zvol",
	.d_flags =	D_DISK | D_TRACKCLOSE,
};

static void zvol_geom_run(zvol_state_t *zv);
static void zvol_geom_destroy(zvol_state_t *zv);
static int zvol_geom_access(struct g_provider *pp, int acr, int acw, int ace);
static void zvol_geom_start(struct bio *bp);
static void zvol_geom_worker(void *arg);


/* START */
/*ARGSUSED*/
static int
zvol_open(struct g_provider *pp, int flag, int count)
{
	zvol_state_t *zv;
	int err = 0;
	boolean_t drop_suspend = B_TRUE;

	if (!zpool_on_zvol && tsd_get(zfs_geom_probe_vdev_key) != NULL) {
		/*
		 * if zfs_geom_probe_vdev_key is set, that means that zfs is
		 * attempting to probe geom providers while looking for a
		 * replacement for a missing VDEV.  In this case, the
		 * spa_namespace_lock will not be held, but it is still illegal
		 * to use a zvol as a vdev.  Deadlocks can result if another
		 * thread has spa_namespace_lock
		 */
		return (SET_ERROR(EOPNOTSUPP));
	}

	zv = pp->private;
	if (zv == NULL) {
		rw_exit(&zvol_state_lock);
		return (SET_ERROR(ENXIO));
	}

	rw_enter(&zvol_state_lock, ZVOL_RW_READER);
	mutex_enter(&zv->zv_state_lock);

	/*
	 * make sure zvol is not suspended during first open
	 * (hold zv_suspend_lock) and respect proper lock acquisition
	 * ordering - zv_suspend_lock before zv_state_lock
	 */
	if (zv->zv_open_count == 0) {
		if (!rw_tryenter(&zv->zv_suspend_lock, ZVOL_RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, ZVOL_RW_READER);
			mutex_enter(&zv->zv_state_lock);
			/* check to see if zv_suspend_lock is needed */
			if (zv->zv_open_count != 0) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	} else {
		drop_suspend = B_FALSE;
	}
	rw_exit(&zvol_state_lock);
	if (zv->zv_open_count == 0) {
		err = zvol_first_open(zv, !(flag & FWRITE));
		if (err)
			goto out_mutex;
		pp->mediasize = zv->zv_volsize;
		pp->stripeoffset = 0;
		pp->stripesize = zv->zv_volblocksize;
	}

	/*
	 * Check for a bad on-disk format version now since we
	 * lied about owning the dataset readonly before.
	 */
	if ((flag & FWRITE) && ((zv->zv_flags & ZVOL_RDONLY) ||
	    dmu_objset_incompatible_encryption_version(zv->zv_objset))) {
		err = EROFS;
		goto out_open_count;
	}
	if (zv->zv_flags & ZVOL_EXCL) {
		err = EBUSY;
		goto out_open_count;
	}
#ifdef FEXCL
	if (flag & FEXCL) {
		if (zv->zv_open_count != 0) {
			err = EBUSY;
			goto out_open_count;
		}
		zv->zv_flags |= ZVOL_EXCL;
	}
#endif

	zv->zv_open_count += count;
	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	return (0);
 out_open_count:
	if (zv->zv_open_count == 0)
		zvol_last_close(zv);
 out_mutex:
	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	return (SET_ERROR(err));
}

/*ARGSUSED*/
static int
zvol_close(struct g_provider *pp, int flag, int count)
{
	zvol_state_t *zv;
	int error = 0;
	boolean_t drop_suspend = B_TRUE;

	ASSERT(!RW_LOCK_HELD(&zvol_state_lock));
	rw_enter(&zvol_state_lock, ZVOL_RW_READER);
	zv = pp->private;
	if (zv == NULL) {
		rw_exit(&zvol_state_lock);
		return (SET_ERROR(ENXIO));
	}

	ASSERT(!MUTEX_HELD(&zv->zv_state_lock));
	mutex_enter(&zv->zv_state_lock);
	if (zv->zv_flags & ZVOL_EXCL) {
		ASSERT(zv->zv_open_count == 1);
		zv->zv_flags &= ~ZVOL_EXCL;
	}

	/*
	 * If the open count is zero, this is a spurious close.
	 * That indicates a bug in the kernel / DDI framework.
	 */
	ASSERT(zv->zv_open_count > 0);
	/*
	 * make sure zvol is not suspended during last close
	 * (hold zv_suspend_lock) and respect proper lock acquisition
	 * ordering - zv_suspend_lock before zv_state_lock
	 */
	if ((zv->zv_open_count - count) == 0) {
		if (!rw_tryenter(&zv->zv_suspend_lock, ZVOL_RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, ZVOL_RW_READER);
			mutex_enter(&zv->zv_state_lock);
			/* check to see if zv_suspend_lock is needed */
			if (zv->zv_open_count != 1) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	} else {
		drop_suspend = B_FALSE;
	}
	rw_exit(&zvol_state_lock);

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	//ASSERT(zv->zv_open_count != 1 /* || RW_READ_HELD(&zv->zv_suspend_lock) */ );

	/*
	 * You may get multiple opens, but only one close.
	 */
	zv->zv_open_count -= count;

	if (zv->zv_open_count == 0)
		zvol_last_close(zv);

	mutex_exit(&zv->zv_state_lock);

	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	return (error);
}

void
zvol_strategy(struct bio *bp)
{
	zvol_state_t *zv;
	uint64_t off, volsize;
	size_t resid;
	char *addr;
	objset_t *os;
	locked_range_t *lr;
	int error = 0;
	boolean_t doread = 0;
	boolean_t is_dumpified;
	boolean_t sync;

	if (bp->bio_to)
		zv = bp->bio_to->private;
	else
		zv = bp->bio_dev->si_drv2;

	if (zv == NULL) {
		error = SET_ERROR(ENXIO);
		goto out;
	}

	if (bp->bio_cmd != BIO_READ && (zv->zv_flags & ZVOL_RDONLY)) {
		error = SET_ERROR(EROFS);
		goto out;
	}

	switch (bp->bio_cmd) {
	case BIO_FLUSH:
		goto sync;
	case BIO_READ:
		doread = 1;
	case BIO_WRITE:
	case BIO_DELETE:
		break;
	default:
		error = EOPNOTSUPP;
		goto out;
	}

	off = bp->bio_offset;
	volsize = zv->zv_volsize;

	os = zv->zv_objset;
	ASSERT(os != NULL);

	addr = bp->bio_data;
	resid = bp->bio_length;

	if (resid > 0 && (off < 0 || off >= volsize)) {
		error = SET_ERROR(EIO);
		goto out;
	}

	is_dumpified = B_FALSE;
	sync = !doread && !is_dumpified &&
	    zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS;

	/*
	 * There must be no buffer changes when doing a dmu_sync() because
	 * we can't change the data whilst calculating the checksum.
	 */
	lr = rangelock_enter(&zv->zv_rangelock, off, resid,
	    doread ? RL_READER : RL_WRITER);

	if (bp->bio_cmd == BIO_DELETE) {
		dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error != 0) {
			dmu_tx_abort(tx);
		} else {
			zvol_log_truncate(zv, tx, off, resid, sync);
			dmu_tx_commit(tx);
			error = dmu_free_long_range(zv->zv_objset, ZVOL_OBJ,
			    off, resid);
			resid = 0;
		}
		goto unlock;
	}
	while (resid != 0 && off < volsize) {
		size_t size = MIN(resid, zvol_maxphys);
		if (doread) {
			error = dmu_read(os, ZVOL_OBJ, off, size, addr,
			    DMU_READ_PREFETCH);
		} else {
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_write(tx, ZVOL_OBJ, off, size);
			error = dmu_tx_assign(tx, TXG_WAIT);
			if (error) {
				dmu_tx_abort(tx);
			} else {
				dmu_write(os, ZVOL_OBJ, off, size, addr, tx);
				zvol_log_write(zv, tx, off, size, sync);
				dmu_tx_commit(tx);
			}
		}
		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
		off += size;
		addr += size;
		resid -= size;
	}
unlock:
	rangelock_exit(lr);

	bp->bio_completed = bp->bio_length - resid;
	if (bp->bio_completed < bp->bio_length && off > volsize)
		error = EINVAL;

	if (sync) {
sync:
		zil_commit(zv->zv_zilog, ZVOL_OBJ);
	}
out:
	if (bp->bio_to)
		g_io_deliver(bp, error);
	else
		biofinish(bp, NULL, error);
}

int
zvol_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	zvol_state_t *zv;
	uint64_t volsize;
	locked_range_t *lr;
	int error = 0;

	zv = dev->si_drv2;

	volsize = zv->zv_volsize;
	/* uio_loffset == volsize isn't an error as its required for EOF processing. */
	if (uio->uio_resid > 0 &&
	    (uio->uio_loffset < 0 || uio->uio_loffset > volsize))
		return (SET_ERROR(EIO));

	lr = rangelock_enter(&zv->zv_rangelock, uio->uio_loffset, uio->uio_resid,
	    RL_READER);
	while (uio->uio_resid > 0 && uio->uio_loffset < volsize) {
		uint64_t bytes = MIN(uio->uio_resid, DMU_MAX_ACCESS >> 1);

		/* don't read past the end */
		if (bytes > volsize - uio->uio_loffset)
			bytes = volsize - uio->uio_loffset;

		error =  dmu_read_uio_dnode(zv->zv_dn, uio, bytes);
		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
	}
	rangelock_exit(lr);
	return (error);
}

int
zvol_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	zvol_state_t *zv;
	uint64_t volsize;
	locked_range_t *lr;
	int error = 0;
	boolean_t sync;

	zv = dev->si_drv2;

	volsize = zv->zv_volsize;
	/* uio_loffset == volsize isn't an error as its required for EOF processing. */
	if (uio->uio_resid > 0 &&
	    (uio->uio_loffset < 0 || uio->uio_loffset > volsize))
		return (SET_ERROR(EIO));

	sync = (ioflag & IO_SYNC) ||
	    (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS);

	lr = rangelock_enter(&zv->zv_rangelock, uio->uio_loffset, uio->uio_resid,
	    RL_WRITER);
	while (uio->uio_resid > 0 && uio->uio_loffset < volsize) {
		uint64_t bytes = MIN(uio->uio_resid, DMU_MAX_ACCESS >> 1);
		uint64_t off = uio->uio_loffset;
		dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);

		if (bytes > volsize - off)	/* don't write past the end */
			bytes = volsize - off;

		dmu_tx_hold_write(tx, ZVOL_OBJ, off, bytes);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			break;
		}
		error = dmu_write_uio_dnode(zv->zv_dn, uio, bytes, tx);
		if (error == 0)
			zvol_log_write(zv, tx, off, bytes, sync);
		dmu_tx_commit(tx);

		if (error)
			break;
	}
	rangelock_exit(lr);
	if (sync)
		zil_commit(zv->zv_zilog, ZVOL_OBJ);
	return (error);
}

static void
zvol_geom_run(zvol_state_t *zv)
{
	struct g_provider *pp;

	pp = zv->zv_provider;
	g_error_provider(pp, 0);

	kproc_kthread_add(zvol_geom_worker, zv, &zfsproc, NULL, 0, 0,
	    "zfskern", "zvol %s", pp->name + sizeof (ZVOL_DRIVER));
}

static void
zvol_geom_destroy(zvol_state_t *zv)
{
	struct g_provider *pp;

	g_topology_assert();

	mutex_enter(&zv->zv_state_lock);
	VERIFY(zv->zv_state == 2);
	mutex_exit(&zv->zv_state_lock);
	pp = zv->zv_provider;
	zv->zv_provider = NULL;
	pp->private = NULL;
	g_wither_geom(pp->geom, ENXIO);
}

static int
zvol_geom_access(struct g_provider *pp, int acr, int acw, int ace)
{
	int count, error, flags;

	g_topology_assert();

	/*
	 * To make it easier we expect either open or close, but not both
	 * at the same time.
	 */
	KASSERT((acr >= 0 && acw >= 0 && ace >= 0) ||
	    (acr <= 0 && acw <= 0 && ace <= 0),
	    ("Unsupported access request to %s (acr=%d, acw=%d, ace=%d).",
	    pp->name, acr, acw, ace));

	if (pp->private == NULL) {
		if (acr <= 0 && acw <= 0 && ace <= 0)
			return (0);
		return (pp->error);
	}

	/*
	 * We don't pass FEXCL flag to zvol_open()/zvol_close() if ace != 0,
	 * because GEOM already handles that and handles it a bit differently.
	 * GEOM allows for multiple read/exclusive consumers and ZFS allows
	 * only one exclusive consumer, no matter if it is reader or writer.
	 * I like better the way GEOM works so I'll leave it for GEOM to
	 * decide what to do.
	 */

	count = acr + acw + ace;
	if (count == 0)
		return (0);

	flags = 0;
	if (acr != 0 || ace != 0)
		flags |= FREAD;
	if (acw != 0)
		flags |= FWRITE;

	g_topology_unlock();
	if (count > 0)
		error = zvol_open(pp, flags, count);
	else
		error = zvol_close(pp, flags, -count);
	g_topology_lock();
	return (error);
}

static void
zvol_geom_start(struct bio *bp)
{
	zvol_state_t *zv;
	boolean_t first;

	zv = bp->bio_to->private;
	ASSERT(zv != NULL);
	rw_enter(&zv->zv_suspend_lock, ZVOL_RW_READER);
	switch (bp->bio_cmd) {
		case BIO_FLUSH:
		case BIO_WRITE:
		case BIO_DELETE:
		/*
		 * Open a ZIL if this is the first time we have written to this
		 * zvol. We protect zv->zv_zilog with zv_suspend_lock rather
		 * than zv_state_lock so that we don't need to acquire an
		 * additional lock in this path.
		 */
		if (zv->zv_zilog == NULL) {
			rw_exit(&zv->zv_suspend_lock);
			rw_enter(&zv->zv_suspend_lock, RW_WRITER);
			if (zv->zv_zilog == NULL) {
				zv->zv_zilog = zil_open(zv->zv_objset,
				    zvol_get_data);
				zv->zv_flags |= ZVOL_WRITTEN_TO;
			}
			rw_downgrade(&zv->zv_suspend_lock);
		}
		default:
			;
	}

	switch (bp->bio_cmd) {
	case BIO_FLUSH:
		if (!THREAD_CAN_SLEEP())
			goto enqueue;
		zil_commit(zv->zv_zilog, ZVOL_OBJ);
		g_io_deliver(bp, 0);
		break;
	case BIO_READ:
	case BIO_WRITE:
	case BIO_DELETE:
		if (!THREAD_CAN_SLEEP())
			goto enqueue;
		zvol_strategy(bp);
		break;
	case BIO_GETATTR: {
		spa_t *spa = dmu_objset_spa(zv->zv_objset);
		uint64_t refd, avail, usedobjs, availobjs;

		if (g_handleattr_int(bp, "GEOM::candelete", 1))
			goto out;
		if (strcmp(bp->bio_attribute, "blocksavail") == 0) {
			dmu_objset_space(zv->zv_objset, &refd, &avail,
			    &usedobjs, &availobjs);
			if (g_handleattr_off_t(bp, "blocksavail",
			    avail / DEV_BSIZE))
				goto out;
		} else if (strcmp(bp->bio_attribute, "blocksused") == 0) {
			dmu_objset_space(zv->zv_objset, &refd, &avail,
			    &usedobjs, &availobjs);
			if (g_handleattr_off_t(bp, "blocksused",
			    refd / DEV_BSIZE))
				goto out;
		} else if (strcmp(bp->bio_attribute, "poolblocksavail") == 0) {
			avail = metaslab_class_get_space(spa_normal_class(spa));
			avail -= metaslab_class_get_alloc(spa_normal_class(spa));
			if (g_handleattr_off_t(bp, "poolblocksavail",
			    avail / DEV_BSIZE))
				goto out;
		} else if (strcmp(bp->bio_attribute, "poolblocksused") == 0) {
			refd = metaslab_class_get_alloc(spa_normal_class(spa));
			if (g_handleattr_off_t(bp, "poolblocksused",
			    refd / DEV_BSIZE))
				goto out;
		}
		/* FALLTHROUGH */
	}
	default:
		g_io_deliver(bp, EOPNOTSUPP);
		break;
	}
 out:
	rw_exit(&zv->zv_suspend_lock);
	return;

enqueue:
	rw_exit(&zv->zv_suspend_lock);
	mutex_enter(&zv->zv_state_lock);
	first = (bioq_first(&zv->zv_queue) == NULL);
	bioq_insert_tail(&zv->zv_queue, bp);
	mutex_exit(&zv->zv_state_lock);
	if (first)
		wakeup_one(&zv->zv_queue);
}

static void
zvol_geom_worker(void *arg)
{
	zvol_state_t *zv;
	struct bio *bp;

	thread_lock(curthread);
	sched_prio(curthread, PRIBIO);
	thread_unlock(curthread);

	zv = arg;
	for (;;) {
		mutex_enter(&zv->zv_state_lock);
		bp = bioq_takefirst(&zv->zv_queue);
		if (bp == NULL) {
			if (zv->zv_state == 1) {
				zv->zv_state = 2;
				wakeup(&zv->zv_state);
				mutex_exit(&zv->zv_state_lock);
				kthread_exit();
			}
			msleep(&zv->zv_queue, &zv->zv_state_lock, PRIBIO | PDROP,
			    "zvol:io", 0);
			continue;
		}
		mutex_exit(&zv->zv_state_lock);
		/*
		 * To be released in the I/O function. See the comment on
		 * rangelock_enter() below.
		 */
		rw_enter(&zv->zv_suspend_lock, ZVOL_RW_READER);
		switch (bp->bio_cmd) {
		case BIO_FLUSH:
			zil_commit(zv->zv_zilog, ZVOL_OBJ);
			g_io_deliver(bp, 0);
			break;
		case BIO_READ:
		case BIO_WRITE:
		case BIO_DELETE:
			zvol_strategy(bp);
			break;
		default:
			g_io_deliver(bp, EOPNOTSUPP);
			break;
		}
		rw_exit(&zv->zv_suspend_lock);
	}
}

static int
zvol_d_open(struct cdev *dev, int flags, int fmt, struct thread *td)
{
	zvol_state_t *zv = dev->si_drv2;
	int err = 0;
	boolean_t drop_suspend = B_TRUE;

	rw_enter(&zvol_state_lock, ZVOL_RW_READER);
	mutex_enter(&zv->zv_state_lock);
	/*
	 * make sure zvol is not suspended during first open
	 * (hold zv_suspend_lock) and respect proper lock acquisition
	 * ordering - zv_suspend_lock before zv_state_lock
	 */
	if (zv->zv_open_count == 0) {
		if (!rw_tryenter(&zv->zv_suspend_lock, ZVOL_RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, ZVOL_RW_READER);
			mutex_enter(&zv->zv_state_lock);
			/* check to see if zv_suspend_lock is needed */
			if (zv->zv_open_count != 0) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	} else {
		drop_suspend = B_FALSE;
	}

	rw_exit(&zvol_state_lock);
	if (zv->zv_open_count == 0)
		err = zvol_first_open(zv, !(flags & FWRITE));

	if (err) {
		goto out_locked;
		return (err);
	}
	if ((flags & FWRITE) && (zv->zv_flags & ZVOL_RDONLY)) {
		err = EROFS;
		goto out_opened;
	}
	if (zv->zv_flags & ZVOL_EXCL) {
		err = EBUSY;
		goto out_opened;
	}
#ifdef FEXCL
	if (flags & FEXCL) {
		if (zv->zv_open_count != 0) {
			err = EBUSY;
			goto out_opened;
		}
		zv->zv_flags |= ZVOL_EXCL;
	}
#endif

	zv->zv_open_count++;
	if (flags & (FSYNC | FDSYNC)) {
		zv->zv_sync_cnt++;
		if (zv->zv_sync_cnt == 1)
			zil_async_to_sync(zv->zv_zilog, ZVOL_OBJ);
	}
	mutex_exit(&zv->zv_state_lock);
	return (0);

 out_opened:
	if (zv->zv_open_count == 0)
		zvol_last_close(zv);

 out_locked:
	mutex_exit(&zv->zv_state_lock);
	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);
	return SET_ERROR(err);
}

static int
zvol_d_close(struct cdev *dev, int flags, int fmt, struct thread *td)
{
	zvol_state_t *zv = dev->si_drv2;
	boolean_t drop_suspend = B_TRUE;

	rw_enter(&zvol_state_lock, ZVOL_RW_READER);
	mutex_enter(&zv->zv_state_lock);
	if (zv->zv_flags & ZVOL_EXCL) {
		ASSERT(zv->zv_open_count == 1);
		zv->zv_flags &= ~ZVOL_EXCL;
	}

	/*
	 * If the open count is zero, this is a spurious close.
	 * That indicates a bug in the kernel / DDI framework.
	 */
	ASSERT(zv->zv_open_count > 0);
	/*
	 * make sure zvol is not suspended during last close
	 * (hold zv_suspend_lock) and respect proper lock acquisition
	 * ordering - zv_suspend_lock before zv_state_lock
	 */
	if (zv->zv_open_count == 1) {
		if (!rw_tryenter(&zv->zv_suspend_lock, ZVOL_RW_READER)) {
			mutex_exit(&zv->zv_state_lock);
			rw_enter(&zv->zv_suspend_lock, ZVOL_RW_READER);
			mutex_enter(&zv->zv_state_lock);
			/* check to see if zv_suspend_lock is needed */
			if (zv->zv_open_count != 1) {
				rw_exit(&zv->zv_suspend_lock);
				drop_suspend = B_FALSE;
			}
		}
	} else {
		drop_suspend = B_FALSE;
	}
	rw_exit(&zvol_state_lock);
	/*
	 * You may get multiple opens, but only one close.
	 */
	zv->zv_open_count--;
	if (flags & (FSYNC | FDSYNC))
		zv->zv_sync_cnt--;

	if (zv->zv_open_count == 0)
		zvol_last_close(zv);
	mutex_exit(&zv->zv_state_lock);

	if (drop_suspend)
		rw_exit(&zv->zv_suspend_lock);

	return (0);
}

static int
zvol_d_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
	zvol_state_t *zv;
	locked_range_t *lr;
	off_t offset, length;
	int i, error;
	boolean_t sync;

	zv = dev->si_drv2;

	error = 0;
	KASSERT(zv->zv_open_count > 0,
	    ("Device with zero access count in zvol_d_ioctl"));

	i = IOCPARM_LEN(cmd);
	switch (cmd) {
	case DIOCGSECTORSIZE:
		*(u_int *)data = DEV_BSIZE;
		break;
	case DIOCGMEDIASIZE:
		*(off_t *)data = zv->zv_volsize;
		break;
	case DIOCGFLUSH:
		zil_commit(zv->zv_zilog, ZVOL_OBJ);
		break;
	case DIOCGDELETE:
		if (!zvol_unmap_enabled)
			break;

		offset = ((off_t *)data)[0];
		length = ((off_t *)data)[1];
		if ((offset % DEV_BSIZE) != 0 || (length % DEV_BSIZE) != 0 ||
		    offset < 0 || offset >= zv->zv_volsize ||
		    length <= 0) {
			printf("%s: offset=%jd length=%jd\n", __func__, offset,
			    length);
			error = EINVAL;
			break;
		}

		lr = rangelock_enter(&zv->zv_rangelock, offset, length, RL_WRITER);
		dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error != 0) {
			sync = FALSE;
			dmu_tx_abort(tx);
		} else {
			sync = (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS);
			zvol_log_truncate(zv, tx, offset, length, sync);
			dmu_tx_commit(tx);
			error = dmu_free_long_range(zv->zv_objset, ZVOL_OBJ,
			    offset, length);
		}
		rangelock_exit(lr);
		if (sync)
			zil_commit(zv->zv_zilog, ZVOL_OBJ);
		break;
	case DIOCGSTRIPESIZE:
		*(off_t *)data = zv->zv_volblocksize;
		break;
	case DIOCGSTRIPEOFFSET:
		*(off_t *)data = 0;
		break;
	case DIOCGATTR: {
		spa_t *spa = dmu_objset_spa(zv->zv_objset);
		struct diocgattr_arg *arg = (struct diocgattr_arg *)data;
		uint64_t refd, avail, usedobjs, availobjs;

		if (strcmp(arg->name, "GEOM::candelete") == 0)
			arg->value.i = 1;
		else if (strcmp(arg->name, "blocksavail") == 0) {
			dmu_objset_space(zv->zv_objset, &refd, &avail,
			    &usedobjs, &availobjs);
			arg->value.off = avail / DEV_BSIZE;
		} else if (strcmp(arg->name, "blocksused") == 0) {
			dmu_objset_space(zv->zv_objset, &refd, &avail,
			    &usedobjs, &availobjs);
			arg->value.off = refd / DEV_BSIZE;
		} else if (strcmp(arg->name, "poolblocksavail") == 0) {
			avail = metaslab_class_get_space(spa_normal_class(spa));
			avail -= metaslab_class_get_alloc(spa_normal_class(spa));
			arg->value.off = avail / DEV_BSIZE;
		} else if (strcmp(arg->name, "poolblocksused") == 0) {
			refd = metaslab_class_get_alloc(spa_normal_class(spa));
			arg->value.off = refd / DEV_BSIZE;
		} else
			error = ENOIOCTL;
		break;
	}
	case FIOSEEKHOLE:
	case FIOSEEKDATA: {
		off_t *off = (off_t *)data;
		uint64_t noff;
		boolean_t hole;

		hole = (cmd == FIOSEEKHOLE);
		noff = *off;
		error = dmu_offset_next(zv->zv_objset, ZVOL_OBJ, hole, &noff);
		*off = noff;
		break;
	}
	default:
		error = ENOIOCTL;
	}

	return (error);
}

boolean_t
zvol_is_zvol(const char *device)
{
	return (device && strncmp(device, ZVOL_DIR, strlen(ZVOL_DIR)) == 0);
}


/* PUBLIC */
							  
void
zvol_rename_minor(zvol_state_t *zv, const char *newname)
{
	struct g_geom *gp;
	struct g_provider *pp;
	struct cdev *dev;

	ASSERT(RW_LOCK_HELD(&zvol_state_lock));
	ASSERT(MUTEX_HELD(&zv->zv_state_lock));

	if (zv->zv_volmode == ZFS_VOLMODE_GEOM) {
		g_topology_lock();
		pp = zv->zv_provider;
		ASSERT(pp != NULL);
		gp = pp->geom;
		ASSERT(gp != NULL);

		zv->zv_provider = NULL;
		g_wither_provider(pp, ENXIO);

		pp = g_new_providerf(gp, "%s/%s", ZVOL_DRIVER, newname);
		pp->flags |= G_PF_DIRECT_RECEIVE | G_PF_DIRECT_SEND;
		pp->sectorsize = DEV_BSIZE;
		pp->mediasize = zv->zv_volsize;
		pp->private = zv;
		zv->zv_provider = pp;
		g_error_provider(pp, 0);
		g_topology_unlock();
	} else if (zv->zv_volmode == ZFS_VOLMODE_DEV) {
		struct make_dev_args args;

		if ((dev = zv->zv_dev) != NULL) {
			zv->zv_dev = NULL;
			destroy_dev(dev);
			if (zv->zv_open_count > 0) {
				zv->zv_flags &= ~ZVOL_EXCL;
				zv->zv_open_count = 0;
				/* XXX  need suspend lock but lock order */
				zvol_last_close(zv);
			}
		}

		make_dev_args_init(&args);
		args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
		args.mda_devsw = &zvol_cdevsw;
		args.mda_cr = NULL;
		args.mda_uid = UID_ROOT;
		args.mda_gid = GID_OPERATOR;
		args.mda_mode = 0640;
		args.mda_si_drv2 = zv;
		if (make_dev_s(&args, &zv->zv_dev,
		    "%s/%s", ZVOL_DRIVER, newname) == 0)
			zv->zv_dev->si_iosize_max = MAXPHYS;
	}
	strlcpy(zv->zv_name, newname, sizeof (zv->zv_name));
}

/*
 * Setup zv after we just own the zv->objset
 */
int
zvol_setup_zv(zvol_state_t *zv)
{
	uint64_t volsize;
	int error;
	uint64_t ro;
	objset_t *os = zv->zv_objset;

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(RW_LOCK_HELD(&zv->zv_suspend_lock));

	zv->zv_zilog = NULL;
	zv->zv_flags &= ~ZVOL_WRITTEN_TO;

	error = dsl_prop_get_integer(zv->zv_name, "readonly", &ro, NULL);
	if (error)
		return (SET_ERROR(error));

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize);
	if (error)
		return (SET_ERROR(error));

	error = dnode_hold(os, ZVOL_OBJ, FTAG, &zv->zv_dn);
	if (error)
		return (SET_ERROR(error));

	//set_capacity(zv->zv_disk, volsize >> 9);
	zv->zv_volsize = volsize;

	if (ro || dmu_objset_is_snapshot(os) ||
	    !spa_writeable(dmu_objset_spa(os))) {
		zv->zv_flags |= ZVOL_RDONLY;
	} else {
		zv->zv_flags &= ~ZVOL_RDONLY;
	}
	return (0);
}

/*
 * Remove minor node for the specified volume.
 */
void
zvol_free(void *arg)
{
	zvol_state_t *zv = arg;

	ASSERT(!RW_LOCK_HELD(&zv->zv_suspend_lock));
	ASSERT(!MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(zv->zv_open_count == 0);

	ZFS_LOG(1, "ZVOL %s destroyed.", zv->zv_name);

	rw_destroy(&zv->zv_suspend_lock);
	zfs_rangelock_fini(&zv->zv_rangelock);
	if (zv->zv_volmode == ZFS_VOLMODE_GEOM) {
		g_topology_lock();
		zvol_geom_destroy(zv);
		g_topology_unlock();
	} else if (zv->zv_volmode == ZFS_VOLMODE_DEV) {
		if (zv->zv_dev != NULL)
			destroy_dev(zv->zv_dev);
	}

	mutex_destroy(&zv->zv_state_lock);
	kmem_free(zv, sizeof (zvol_state_t));
	zvol_minors--;
}

/*
 * Create a minor node (plus a whole lot more) for the specified volume.
 */
int
zvol_create_minor_impl(const char *name)
{
	zvol_state_t *zv;
	objset_t *os;
	dmu_object_info_t *doi;
	struct g_provider *pp;
	struct g_geom *gp;
	uint64_t volsize;
	uint64_t volmode, hash;
	int error;

	ZFS_LOG(1, "Creating ZVOL %s...", name);

	hash = zvol_name_hash(name);
	if ((zv = zvol_find_by_name_hash(name, hash, RW_NONE)) != NULL) {
		ASSERT(MUTEX_HELD(&zv->zv_state_lock));
		mutex_exit(&zv->zv_state_lock);
		return (SET_ERROR(EEXIST));
	}

	DROP_GIANT();
	/* lie and say we're read-only */
	error = dmu_objset_own(name, DMU_OST_ZVOL, B_TRUE, B_TRUE, FTAG, &os);
	doi = kmem_alloc(sizeof (dmu_object_info_t), KM_SLEEP);

	if (error)
		goto out_doi;

	error = dmu_object_info(os, ZVOL_OBJ, doi);
	if (error)
		goto out_dmu_objset_disown;

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize);
	if (error)
		goto out_dmu_objset_disown;

	/*
	 * zvol_alloc equivalent ...
	 */
	zv = kmem_zalloc(sizeof (*zv), KM_SLEEP);
	zv->zv_state = 0;
	error = dsl_prop_get_integer(name,
	    zfs_prop_to_name(ZFS_PROP_VOLMODE), &volmode, NULL);
	if (error != 0 || volmode == ZFS_VOLMODE_DEFAULT)
		volmode = zvol_volmode;

	zv->zv_volmode = volmode;
	mutex_init(&zv->zv_state_lock, NULL, MUTEX_DEFAULT, NULL);
	if (zv->zv_volmode == ZFS_VOLMODE_GEOM) {
		g_topology_lock();
		gp = g_new_geomf(&zfs_zvol_class, "zfs::zvol::%s", name);
		gp->start = zvol_geom_start;
		gp->access = zvol_geom_access;
		pp = g_new_providerf(gp, "%s/%s", ZVOL_DRIVER, name);
		pp->flags |= G_PF_DIRECT_RECEIVE | G_PF_DIRECT_SEND;
		pp->sectorsize = DEV_BSIZE;
		pp->mediasize = 0;
		pp->private = zv;

		zv->zv_provider = pp;
		bioq_init(&zv->zv_queue);
	} else if (zv->zv_volmode == ZFS_VOLMODE_DEV) {
		struct make_dev_args args;

		make_dev_args_init(&args);
		args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
		args.mda_devsw = &zvol_cdevsw;
		args.mda_cr = NULL;
		args.mda_uid = UID_ROOT;
		args.mda_gid = GID_OPERATOR;
		args.mda_mode = 0640;
		args.mda_si_drv2 = zv;
		error = make_dev_s(&args, &zv->zv_dev,
		    "%s/%s", ZVOL_DRIVER, name);
		if (error != 0) {
			mutex_destroy(&zv->zv_state_lock);
			kmem_free(zv, sizeof (*zv));
			dmu_objset_disown(os, 1, FTAG);
			goto out_dmu_objset_disown;
		}
		zv->zv_dev->si_iosize_max = MAXPHYS;
	}
	(void) strlcpy(zv->zv_name, name, MAXPATHLEN);
	rw_init(&zv->zv_suspend_lock, NULL, RW_DEFAULT, NULL);
	zfs_rangelock_init(&zv->zv_rangelock, NULL, NULL);

	if (dmu_objset_is_snapshot(os) || !spa_writeable(dmu_objset_spa(os)))
		zv->zv_flags |= ZVOL_RDONLY;

	zv->zv_volblocksize = doi->doi_data_block_size;
	zv->zv_volsize = volsize;
	zv->zv_objset = os;

	if (spa_writeable(dmu_objset_spa(os))) {
		if (zil_replay_disable)
			zil_destroy(dmu_objset_zil(os), B_FALSE);
		else
			zil_replay(os, zv, zvol_replay_vector);
	}

	/* XXX do prefetch */

	zv->zv_objset = NULL;
 out_dmu_objset_disown:
	dmu_objset_disown(os, 1, FTAG);

	if (zv->zv_volmode == ZFS_VOLMODE_GEOM) {
		if (error == 0)
			zvol_geom_run(zv);
		g_topology_unlock();
	}
 out_doi:
	kmem_free(doi, sizeof (dmu_object_info_t));
	if (error == 0) {
		rw_enter(&zvol_state_lock, RW_WRITER);
		zvol_insert(zv);
		zvol_minors++;
		rw_exit(&zvol_state_lock);
	}
	PICKUP_GIANT();
	ZFS_LOG(1, "ZVOL %s created.", name);
	return (0);
}

static void
zvol_size_changed(zvol_state_t *zv, uint64_t volsize)
{
	zv->zv_volsize = volsize;
	if (zv->zv_volmode == ZFS_VOLMODE_GEOM) {
		struct g_provider *pp;

		pp = zv->zv_provider;
		if (pp == NULL)
			return;
		g_topology_lock();

		/*
		 * Do not invoke resize event when initial size was zero.
		 * ZVOL initializes the size on first open, this is not
		 * real resizing.
		 */
		if (pp->mediasize == 0)
			pp->mediasize = zv->zv_volsize;
		else
			g_resize_provider(pp, zv->zv_volsize);
		g_topology_unlock();
	}
}


void
zvol_os_clear_private(zvol_state_t *zv)
{
	struct g_provider *pp;

	ASSERT(RW_LOCK_HELD(&zvol_state_lock));
	if (zv->zv_provider) {
		zv->zv_state = 1;
		pp = zv->zv_provider;
		pp->private = NULL;
		wakeup_one(&zv->zv_queue);
		while (zv->zv_state != 2)
			msleep(&zv->zv_state, &zv->zv_state_lock, 0, "zvol:w", 0);
		ASSERT(!RW_LOCK_HELD(&zv->zv_suspend_lock));
	}
}

int
zvol_os_update_volsize(zvol_state_t *zv, uint64_t volsize)
{
	zvol_size_changed(zv, volsize);
	return (0);
}
int
zvol_busy(void)
{
	return (zvol_minors != 0);
}

int
zvol_os_init(void)
{
	return (0);
}

void
zvol_os_fini(void)
{

}
