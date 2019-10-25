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

#include <sys/spa.h>
#include <sys/zio.h>
#include <sys/spa_impl.h>
#include <sys/zio_compress.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_context.h>
#include <sys/arc.h>
#include <sys/refcount.h>
#include <sys/vdev.h>
#include <sys/vdev_trim.h>
#include <sys/vdev_impl.h>
#include <sys/dsl_pool.h>
#include <sys/zio_checksum.h>
#include <sys/multilist.h>
#include <sys/abd.h>
#include <sys/zil.h>
#include <sys/fm/fs/zfs.h>
#include <sys/eventhandler.h>
#include <sys/callb.h>
#include <sys/kstat.h>
#include <sys/zthr.h>
#include <zfs_fletcher.h>
#include <sys/arc_impl.h>
#include <sys/trace_defs.h>
#include <sys/aggsum.h>
#include <sys/cityhash.h>

extern struct vfsops zfs_vfsops;

/* vmem_size typemask */
#define	VMEM_ALLOC	0x01
#define	VMEM_FREE	0x02
#define	VMEM_MAXFREE	0x10
typedef size_t		vmem_size_t;
extern vmem_size_t vmem_size(vmem_t *vm, int typemask);

uint_t zfs_arc_free_target = 0;

int64_t last_free_memory;
free_memory_reason_t last_free_reason;

int64_t
arc_available_memory(void)
{
	int64_t lowest = INT64_MAX;
	int64_t n __unused;
	free_memory_reason_t r = FMR_UNKNOWN;

#ifdef _KERNEL
	/*
	 * Cooperate with pagedaemon when it's time for it to scan
	 * and reclaim some pages.
	 */
	n = PAGESIZE * ((int64_t)freemem - zfs_arc_free_target);
	if (n < lowest) {
		lowest = n;
		r = FMR_LOTSFREE;
	}
#if defined(__i386) || !defined(UMA_MD_SMALL_ALLOC)
	/*
	 * If we're on an i386 platform, it's possible that we'll exhaust the
	 * kernel heap space before we ever run out of available physical
	 * memory.  Most checks of the size of the heap_area compare against
	 * tune.t_minarmem, which is the minimum available real memory that we
	 * can have in the system.  However, this is generally fixed at 25 pages
	 * which is so low that it's useless.  In this comparison, we seek to
	 * calculate the total heap-size, and reclaim if more than 3/4ths of the
	 * heap is allocated.  (Or, in the calculation, if less than 1/4th is
	 * free)
	 */
	n = uma_avail() - (long)(uma_limit() / 4);
	if (n < lowest) {
		lowest = n;
		r = FMR_HEAP_ARENA;
	}
#endif

	/*
	 * If zio data pages are being allocated out of a separate heap segment,
	 * then enforce that the size of available vmem for this arena remains
	 * above about 1/4th (1/(2^arc_zio_arena_free_shift)) free.
	 *
	 * Note that reducing the arc_zio_arena_free_shift keeps more virtual
	 * memory (in the zio_arena) free, which can avoid memory
	 * fragmentation issues.
	 */
	if (zio_arena != NULL) {
		n = (int64_t)vmem_size(zio_arena, VMEM_FREE) -
		    (vmem_size(zio_arena, VMEM_ALLOC) >>
		    arc_zio_arena_free_shift);
		if (n < lowest) {
			lowest = n;
			r = FMR_ZIO_ARENA;
		}
	}

#else	/* _KERNEL */
	/* Every 100 calls, free a small amount */
	if (spa_get_random(100) == 0)
		lowest = -1024;
#endif	/* _KERNEL */

	last_free_memory = lowest;
	last_free_reason = r;
	DTRACE_PROBE2(arc__available_memory, int64_t, lowest, int, r);
	return (lowest);
}

/*
 * Helper function for arc_prune_async() it is responsible for safely
 * handling the execution of a registered arc_prune_func_t.
 */
static void
arc_prune_task(void *arg)
{
	int64_t nr_scan = *(int64_t *)arg;

	free(arg, M_TEMP);
	vnlru_free(nr_scan, &zfs_vfsops);
}

/*
 * Notify registered consumers they must drop holds on a portion of the ARC
 * buffered they reference.  This provides a mechanism to ensure the ARC can
 * honor the arc_meta_limit and reclaim otherwise pinned ARC buffers.  This
 * is analogous to dnlc_reduce_cache() but more generic.
 *
 * This operation is performed asynchronously so it may be safely called
 * in the context of the arc_reclaim_thread().  A reference is taken here
 * for each registered arc_prune_t and the arc_prune_task() is responsible
 * for releasing it once the registered arc_prune_func_t has completed.
 */
void
arc_prune_async(int64_t adjust)
{

	int64_t *adjustptr;

	if ((adjustptr = malloc(sizeof (int64_t), M_TEMP, M_NOWAIT)) == NULL)
		return;

	*adjustptr = adjust;
	taskq_dispatch(arc_prune_taskq, arc_prune_task, adjustptr, TQ_SLEEP);
	ARCSTAT_BUMP(arcstat_prune);
}

uint64_t
arc_all_memory(void)
{
	return ((uint64_t)ptob(physmem));
}

int
arc_memory_throttle(spa_t *spa, uint64_t reserve, uint64_t txg)
{
	return (0);
}

uint64_t
arc_free_memory(void)
{
	/* XXX */
	return (0);
}

static eventhandler_tag arc_event_lowmem = NULL;

static void
arc_lowmem(void *arg __unused, int howto __unused)
{
	int64_t free_memory, to_free;

	arc_no_grow = B_TRUE;
	arc_warm = B_TRUE;
	arc_growtime = gethrtime() + SEC2NSEC(arc_grow_retry);
	free_memory = arc_available_memory();
	to_free = (arc_c >> arc_shrink_shift) - MIN(free_memory, 0);
	DTRACE_PROBE2(arc__needfree, int64_t, free_memory, int64_t, to_free);
	arc_reduce_target_size(to_free);

	mutex_enter(&arc_adjust_lock);
	arc_adjust_needed = B_TRUE;
	zthr_wakeup(arc_adjust_zthr);

	/*
	 * It is unsafe to block here in arbitrary threads, because we can come
	 * here from ARC itself and may hold ARC locks and thus risk a deadlock
	 * with ARC reclaim thread.
	 */
	if (curproc == pageproc)
		(void) cv_wait(&arc_adjust_waiters_cv, &arc_adjust_lock);
	mutex_exit(&arc_adjust_lock);
}

void
arc_lowmem_init(void)
{
	arc_event_lowmem = EVENTHANDLER_REGISTER(vm_lowmem, arc_lowmem, NULL,
	    EVENTHANDLER_PRI_FIRST);

}

void
arc_lowmem_fini(void)
{
	if (arc_event_lowmem != NULL)
		EVENTHANDLER_DEREGISTER(vm_lowmem, arc_event_lowmem);
}