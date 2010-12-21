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
 * Copyright (c) 2010, Joyent, Inc. All rights reserved.
 */

#include <sys/spa.h>
#include <sys/vdev_impl.h>
#include <sys/zfs_zone.h>

#ifndef _KERNEL

/*
 * Stubs for when compiling for user-land.
 */

void
zfs_zone_io_throttle(zfs_zone_iop_type_t type, uint64_t size)
{
}

void
zfs_zone_zio_init(zio_t *zp)
{
}

void
zfs_zone_zio_start(zio_t *zp)
{
}

void
zfs_zone_zio_done(zio_t *zp)
{
}

#else

/*
 * The real code.
 */

#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/atomic.h>
#include <sys/zio.h>
#include <sys/zone.h>
#include <sys/avl.h>
#include <sys/sdt.h>
#include <sys/ddi.h>

/*
 * The zone throttle delays read and write operations from certain zones based
 * on each zone's IO utilitzation.  Once a cycle (defined by ZONE_CYCLE_TIME
 * below), the delays for each zone are recalculated based on the utilization
 * over the previous window.
 */
boolean_t	zfs_zone_delay_enable = B_TRUE;	/* enable IO throttle */
uint16_t	zfs_zone_delay_step = 5;	/* amount to change delay */
uint16_t	zfs_zone_delay_ceiling = 100;	/* longest possible delay */

hrtime_t	zfs_zone_last_checked = 0;

/*
 * Our timestamps are in usecs.  Our system avg. cycle is 1sec or 1m usecs.
 * Our zone counter update cycle is 2sec or 2m usecs.  We use a longer
 * cycle for that because some ops can see a little over 2 seconds of
 * latency when they are being starved by another zone.
 */
#define	CYCLE_TIME	1000000
#define	ZONE_CYCLE_TIME	2000000
#define	ZONE_ZFS_100MS	100000

typedef struct {
	hrtime_t	cycle_start;
	int		cycle_cnt;
	hrtime_t	cycle_lat;
	hrtime_t	sys_avg_lat;
} sys_lat_cycle_t;

typedef struct {
	hrtime_t zi_now;
	uint_t zi_avgrlat;
	uint_t zi_avgwlat;
	uint64_t zi_totutil;
	uint64_t zi_avgutil;
	int zi_active;
} zoneio_stats_t;

static sys_lat_cycle_t	rd_lat;
static sys_lat_cycle_t	wr_lat;

/*
 * This uses gethrtime() but returns a value in usecs.
 */
#define	GET_USEC_TIME	(gethrtime() / 1000)

/*
 * Keep track of the zone's ZFS IOPs.
 *
 * If the number of ops is >1 then we can just use that value.  However,
 * if the number of ops is <2 then we might have a zone which is trying to do
 * IO but is not able to get any ops through the system.  We don't want to lose
 * track of this zone so we factor in its decayed count into the current count.
 *
 * Each cycle (CYCLE_TIME) we want to update the decayed count.  However,
 * since this calculation is driven by IO activity and since IO does not happen
 * at fixed intervals, we use a timestamp to see when the last update was made.
 * If it was more than one cycle ago, then we need to decay the historical
 * count by the proper number of additional cycles in which no IO was performed.
 *
 * Return true if we actually computed a new historical count.
 * If we're still within an active cycle there is nothing to do, return false.
 */
static hrtime_t
compute_historical_zone_cnt(hrtime_t now, sys_zio_cntr_t *cp)
{
	hrtime_t delta;
	int	gen_cnt;

	/*
	 * Check if its time to recompute a new zone count.
	 * If we're still collecting data for the current cycle, return false.
	 */
	delta = now - cp->cycle_start;
	if (delta < ZONE_CYCLE_TIME)
		return (delta);

	/* A previous cycle is past, compute the new zone count. */

	/*
	 * Figure out how many generations we have to decay the historical
	 * count, since multiple cycles may have elapsed since our last IO.
	 * We depend on int rounding here.
	 */
	gen_cnt = (int)(delta / ZONE_CYCLE_TIME);

	/* If more than 5 cycles since last the IO, reset count. */
	if (gen_cnt > 5) {
		cp->zone_avg_cnt = 0;
	} else {
		/* Update the count. */
		int	i;

		/*
		 * If the zone did more than 1 IO, just use its current count
		 * as the historical value, otherwise decay the historical
		 * count and factor that into the new historical count.  We
		 * pick a threshold > 1 so that we don't lose track of IO due
		 * to int rounding.
		 */
		if (cp->cycle_cnt > 1)
			cp->zone_avg_cnt = cp->cycle_cnt;
		else
			cp->zone_avg_cnt = cp->cycle_cnt +
			    (cp->zone_avg_cnt / 2);

		/*
		 * If more than one generation has elapsed since the last
		 * update, decay the values further.
		 */
		for (i = 1; i < gen_cnt; i++)
			cp->zone_avg_cnt = cp->zone_avg_cnt / 2;
	}

	/* A new cycle begins. */
	cp->cycle_start = now;
	cp->cycle_cnt = 0;

	return (0);
}

/*
 * Add IO op data to the zone.
 */
static void
add_zone_iop(zone_t *zonep, hrtime_t now, zfs_zone_iop_type_t op)
{
	switch (op) {
	case ZFS_ZONE_IOP_READ:
		(void) compute_historical_zone_cnt(now, &zonep->rd_ops);
		zonep->rd_ops.cycle_cnt++;
		break;
	case ZFS_ZONE_IOP_WRITE:
		(void) compute_historical_zone_cnt(now, &zonep->wr_ops);
		zonep->wr_ops.cycle_cnt++;
		break;
	case ZFS_ZONE_IOP_LOGICAL_WRITE:
		(void) compute_historical_zone_cnt(now, &zonep->lwr_ops);
		zonep->lwr_ops.cycle_cnt++;
		break;
	}
}

/*
 * Use a decaying average to keep track of the overall system latency.
 *
 * We want to have the recent activity heavily weighted, but if the
 * activity decreases or stops, then the average should quickly decay
 * down to the new value.
 *
 * Each cycle (CYCLE_TIME) we want to update the decayed average.  However,
 * since this calculation is driven by IO activity and since IO does not happen
 *
 * at fixed intervals, we use a timestamp to see when the last update was made.
 * If it was more than one cycle ago, then we need to decay the average by the
 * proper number of additional cycles in which no IO was performed.
 *
 * Return true if we actually computed a new system average.
 * If we're still within an active cycle there is nothing to do, return false.
 */
static int
compute_new_sys_avg(hrtime_t now, sys_lat_cycle_t *cp)
{
	hrtime_t delta;
	int	gen_cnt;

	/*
	 * Check if its time to recompute a new average.
	 * If we're still collecting data for the current cycle, return false.
	 */
	delta = now - cp->cycle_start;
	if (delta < CYCLE_TIME)
		return (0);

	/* A previous cycle is past, compute a new system average. */

	/*
	 * Figure out how many generations we have to decay, since multiple
	 * cycles may have elapsed since our last IO.
	 * We count on int rounding here.
	 */
	gen_cnt = (int)(delta / CYCLE_TIME);

	/* If more than 5 cycles since last the IO, reset average. */
	if (gen_cnt > 5) {
		cp->sys_avg_lat = 0;
	} else {
		/* Update the average. */
		int	i;

		cp->sys_avg_lat =
		    (cp->sys_avg_lat + cp->cycle_lat) / (1 + cp->cycle_cnt);

		/*
		 * If more than one generation has elapsed since the last
		 * update, decay the values further.
		 */
		for (i = 1; i < gen_cnt; i++)
			cp->sys_avg_lat = cp->sys_avg_lat / 2;
	}

	/* A new cycle begins. */
	cp->cycle_start = now;
	cp->cycle_cnt = 0;
	cp->cycle_lat = 0;

	return (1);
}

static void
add_sys_iop(hrtime_t now, int op, int lat)
{
	switch (op) {
	case ZFS_ZONE_IOP_READ:
		(void) compute_new_sys_avg(now, &rd_lat);
		rd_lat.cycle_cnt++;
		rd_lat.cycle_lat += lat;
		break;
	case ZFS_ZONE_IOP_WRITE:
		(void) compute_new_sys_avg(now, &wr_lat);
		wr_lat.cycle_cnt++;
		wr_lat.cycle_lat += lat;
		break;
	}
}

/*
 * Get the zone IO counts.
 */
static uint_t
calc_zone_cnt(hrtime_t now, sys_zio_cntr_t *cp)
{
	hrtime_t delta;
	uint_t cnt;

	if ((delta = compute_historical_zone_cnt(now, cp)) == 0) {
		/*
		 * No activity in the current cycle, we already have the
		 * historical data so we'll use that.
		 */
		cnt = cp->zone_avg_cnt;
	} else {
		/*
		 * If we're less than half way through the cycle then use
		 * the current count plus half the historical count, otherwise
		 * just use the current count.
		 */
		if (delta < (ZONE_CYCLE_TIME / 2))
			cnt = cp->cycle_cnt + (cp->zone_avg_cnt / 2);
		else
			cnt = cp->cycle_cnt;
	}

	return (cnt);
}

/*
 * Get the average read/write latency in usecs for the system.
 */
static uint_t
calc_avg_lat(hrtime_t now, sys_lat_cycle_t *cp)
{
	if (compute_new_sys_avg(now, cp)) {
		/*
		 * No activity in the current cycle, we already have the
		 * historical data so we'll use that.
		 */
		return (cp->sys_avg_lat);
	} else {
		/*
		 * We're within a cycle; weight the current activity higher
		 * compared to the historical data and use that.
		 */
		extern void __dtrace_probe_zfs__zone__calc__wt__avg(uintptr_t,
		    uintptr_t, uintptr_t);

		__dtrace_probe_zfs__zone__calc__wt__avg(
		    (uintptr_t)cp->sys_avg_lat,
		    (uintptr_t)cp->cycle_lat,
		    (uintptr_t)cp->cycle_cnt);

		return ((cp->sys_avg_lat + (cp->cycle_lat * 8)) /
		    (1 + (cp->cycle_cnt * 8)));
	}
}

/*
 * Account for the current IOP on the zone and for the system as a whole.
 * The latency parameter is in usecs.
 */
static void
add_iop(zone_t *zonep, hrtime_t now, zfs_zone_iop_type_t op, hrtime_t lat)
{
	/* Add op to zone */
	add_zone_iop(zonep, now, op);

	/* Track system latency */
	add_sys_iop(now, op, lat);
}

/*
 * Calculate and return the total number of read ops, write ops and logical
 * write ops for the given zone.  If the zone has issued operations of any type
 * return a non-zero value, otherwise return 0.
 */
static int
get_zone_io_cnt(hrtime_t now, zone_t *zonep, uint_t *rops, uint_t *wops,
    uint_t *lwops)
{
	*rops = calc_zone_cnt(now, &zonep->rd_ops);
	*wops = calc_zone_cnt(now, &zonep->wr_ops);
	*lwops = calc_zone_cnt(now, &zonep->lwr_ops);

	extern void __dtrace_probe_zfs__zone__io__cnt(uintptr_t,
	    uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__io__cnt((uintptr_t)zonep->zone_id,
	    (uintptr_t)(*rops), (uintptr_t)*wops, (uintptr_t)*lwops);

	return (*rops | *wops | *lwops);
}

/*
 * Get the average read/write latency in usecs for the system.
 */
static void
get_sys_avg_lat(hrtime_t now, uint_t *rlat, uint_t *wlat)
{
	*rlat = calc_avg_lat(now, &rd_lat);
	*wlat = calc_avg_lat(now, &wr_lat);

	/*
	 * In an attempt to improve the accuracy of the throttling algorithm,
	 * assume that IO operations can't have zero latency.  Instead, assume
	 * a reasonable lower bound for each operation type. If the actual
	 * observed latencies are non-zero, use those latency values instead.
	 */
	if (*rlat == 0)
		*rlat = 1000;
	if (*wlat == 0)
		*wlat = 10;

	extern void __dtrace_probe_zfs__zone__sys__avg__lat(uintptr_t,
	    uintptr_t);

	__dtrace_probe_zfs__zone__sys__avg__lat((uintptr_t)(*rlat),
	    (uintptr_t)*wlat);
}

/*
 * Find disk utilization for each zone and average utilization for all active
 * zones.
 */
static int
zfs_zone_wait_adjust_calculate_cb(zone_t *zonep, void *arg)
{
	zoneio_stats_t *sp = arg;
	uint_t rops, wops, lwops;

	if (zonep->zone_id == GLOBAL_ZONEID ||
	    get_zone_io_cnt(sp->zi_now, zonep, &rops, &wops, &lwops) == 0) {
		zonep->zone_io_util = 0;
		return (0);
	}

	/*
	 * This calculaton is (somewhat arbitrarily) scaled up by 1000 so this
	 * algorithm can use integers and not floating-point numbers.
	 */
	zonep->zone_io_util = ((rops * sp->zi_avgrlat) +
	    (wops * sp->zi_avgwlat) + (lwops * sp->zi_avgwlat)) * 1000;
	sp->zi_totutil += zonep->zone_io_util;

	if (zonep->zone_io_util > 0)
		sp->zi_active++;

	/*
	 * sdt:::zfs-zone-utilization
	 *
	 *	arg0: zone ID
	 *	arg1: read operations observed during time window
	 *	arg2: write operations observed during time window
	 *	arg3: logical write ops observed during time window
	 *	arg4: calculated utilization given read and write ops
	 */
	extern void __dtrace_probe_zfs__zone__utilization(
	    uint_t, uint_t, uint_t, uint_t, uint_t);

	__dtrace_probe_zfs__zone__utilization((uint_t)(zonep->zone_id),
	    (uint_t)rops, (uint_t)wops, (uint_t)lwops,
	    (uint_t)zonep->zone_io_util);

	return (0);
}

/*
 * For all zones "far enough" away from the average utilization, increase that
 * zones delay.  Otherwise, reduce its delay.
 */
static int
zfs_zone_wait_adjust_delay_cb(zone_t *zonep, void *arg)
{
	zoneio_stats_t *sp = arg;
	uint16_t delay = zonep->zone_io_delay;

	/*
	 * Adjust each IO's delay by a certain amount.  If the overall delay
	 * becomes too high, avoid increasing beyond the ceiling value.
	 */
	if (zonep->zone_io_util > sp->zi_avgutil &&
	    delay < zfs_zone_delay_ceiling &&
	    sp->zi_active > 1) {
		delay = delay + zfs_zone_delay_step < zfs_zone_delay_ceiling ?
		    delay + zfs_zone_delay_step : zfs_zone_delay_ceiling;
	} else if (zonep->zone_io_util < sp->zi_avgutil || sp->zi_active <= 1) {
		delay = delay - zfs_zone_delay_step > 0 ?
		    delay - zfs_zone_delay_step : 0;
	}

	/*
	 * sdt:::zfs-zone-throttle
	 *
	 *	arg0: zone ID
	 *	arg1: old delay for this zone
	 *	arg2: new delay for this zone
	 */
	extern void __dtrace_probe_zfs__zone__throttle(
	    uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__throttle((uintptr_t)(zonep->zone_id),
	    (uintptr_t)zonep->zone_io_delay, (uintptr_t)delay);

	zonep->zone_io_delay = delay;

	return (0);
}

/*
 * Examine the utilization between different zones, and adjust the delay for
 * each zone appropriately.
 */
static void
zfs_zone_wait_adjust(hrtime_t now)
{
	zoneio_stats_t stats;

	(void) bzero(&stats, sizeof (stats));

	stats.zi_now = now;
	get_sys_avg_lat(now, &stats.zi_avgrlat, &stats.zi_avgwlat);

	if (zone_walk(zfs_zone_wait_adjust_calculate_cb, &stats) != 0)
		return;

	if (stats.zi_active > 0)
		stats.zi_avgutil = stats.zi_totutil / stats.zi_active;

	/*
	 * sdt:::zfs-zone-stats
	 *
	 *	arg0: average system read latency
	 *	arg1: average system write latency
	 *	arg2: number of active zones
	 *	arg3: average IO 'utilization' per zone
	 */
	extern void __dtrace_probe_zfs__zone__stats(
	    uintptr_t, uintptr_t, uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__stats((uintptr_t)(stats.zi_avgrlat),
	    (uintptr_t)(stats.zi_avgwlat),
	    (uintptr_t)(stats.zi_active),
	    (uintptr_t)(stats.zi_avgutil));

	(void) zone_walk(zfs_zone_wait_adjust_delay_cb, &stats);
}

/*
 * Add our zone ID to the zio so we can keep track of which zones are doing
 * what, even when the current thread processing the zio is not associated
 * with the zone (e.g. the kernel taskq which pushes out RX groups).
 */
void
zfs_zone_zio_init(zio_t *zp)
{
	zone_t	*zonep = curzone;

	zp->io_zoneid = zonep->zone_id;
}

/*
 * Track IO operations per zone.  Called from dmu_tx_count_write for write ops
 * and dmu_read_uio for read ops.  For each operation, increment that zone's
 * counter based on the type of operation.
 *
 * There are three basic ways that we can see write ops:
 * 1) An application does write syscalls.  Those ops go into a TXG which
 *    we'll count here.  Sometime later a kernel taskq thread (we'll see the
 *    vdev IO as zone 0) will perform some number of physical writes to commit
 *    the TXG to disk.  Those writes are not associated with the zone which
 *    made the write syscalls and the number of operations is not correlated
 *    between the taskq and the zone.
 * 2) An application opens a file with O_SYNC.  Each write will result in
 *    an operation which we'll see here plus a low-level vdev write from
 *    that zone.
 * 3) An application does write syscalls followed by an fsync().  We'll
 *    count the writes going into a TXG here.  We'll also see some number
 *    (usually much smaller, maybe only 1) of low-level vdev writes from this
 *    zone when the fsync is performed, plus some other low-level vdev writes
 *    from the taskq in zone 0 (are these metadata writes?).
 *
 * 4) In addition to the above, there are misc. system-level writes, such as
 *    writing out dirty pages to swap, or sync(2) calls, which will be handled
 *    by the global zone and which we count but don't generally worry about.
 *
 * Because of the above, we can see writes twice because this is called
 * at a high level by a zone thread, but we also will count the phys. writes
 * that are performed at a low level via zfs_zone_zio_start.
 *
 * Without this, it can look like a non-global zone never writes (case 1).
 * Depending on when the TXG is flushed, the counts may be in the same sample
 * bucket or in a different one.
 *
 * Tracking read operations is simpler due to their synchronous semantics.  The
 * zfs_read function -- called as a result of a read(2) syscall -- will always
 * retrieve the data to be read through dmu_read_uio.
 */
void
zfs_zone_io_throttle(zfs_zone_iop_type_t type, uint64_t size)
{
	hrtime_t now;
	uint16_t wait;
	zone_t	*zonep = curzone;

	now = GET_USEC_TIME;

	/*
	 * Only bump the counters for logical operations here.  The counters for
	 * tracking physical IO operations are handled in zfs_zone_zio_done.
	 */
	if (type == ZFS_ZONE_IOP_LOGICAL_WRITE) {
		mutex_enter(&zonep->zone_stg_io_lock);
		add_iop(zonep, now, type, 0);
		mutex_exit(&zonep->zone_stg_io_lock);

		atomic_add_64(&zonep->zone_io_logwrite_ops, 1);
		atomic_add_64(&zonep->zone_io_logwrite_bytes, size);
	} else {
		atomic_add_64(&zonep->zone_io_logread_ops, 1);
		atomic_add_64(&zonep->zone_io_logread_bytes, size);
	}

	if (!zfs_zone_delay_enable)
		return;

	/*
	 * XXX There's a potential race here in that more than one thread may
	 * update the zone delays concurrently.  The worst outcome is corruption
	 * of our data to track each zone's IO, so the algorithm may make
	 * incorrect throttling decisions until the data is refreshed.
	 */
	if ((now - zfs_zone_last_checked) > ZONE_ZFS_100MS) {
		zfs_zone_last_checked = now;
		zfs_zone_wait_adjust(now);
	}

	if ((wait = zonep->zone_io_delay) > 0) {
		/*
		 * sdt:::zfs-zone-wait
		 *
		 *	arg0: zone ID
		 *	arg1: type of IO operation
		 *	arg2: time to delay (in us)
		 */
		extern void __dtrace_probe_zfs__zone__wait(
		    uintptr_t, uintptr_t, uintptr_t);

		__dtrace_probe_zfs__zone__wait((uintptr_t)(zonep->zone_id),
		    (uintptr_t)type, (uintptr_t)wait);

		drv_usecwait(wait);
	}
}

/*
 * Called from zio_vdev_io_start when an IO hits the end of the zio pipeline
 * and is issued.
 * Keep track of start time for latency calculation in zfs_zone_zio_done.
 */
void
zfs_zone_zio_start(zio_t *zp)
{
	if (!zfs_zone_delay_enable)
		return;

	zp->io_start = GET_USEC_TIME;
}

/*
 * Called from vdev_queue_io_done when an IO completes.
 * Increment our counter for zone ops.
 * Calculate the IO latency avg. for this zone.
 */
void
zfs_zone_zio_done(zio_t *zp)
{
	zone_t	*zonep;
	hrtime_t now, diff;

	if (!zfs_zone_delay_enable)
		return;

	if ((zonep = zone_find_by_id(zp->io_zoneid)) == NULL)
		return;

	now = GET_USEC_TIME;
	/* Calculate latency in usec */
	diff = now - zp->io_start;

	mutex_enter(&zonep->zone_stg_io_lock);
	add_iop(zonep, now, zp->io_type == ZIO_TYPE_READ ?
	    ZFS_ZONE_IOP_READ : ZFS_ZONE_IOP_WRITE, diff);
	mutex_exit(&zonep->zone_stg_io_lock);

	if (zp->io_type == ZIO_TYPE_READ) {
		atomic_add_64(&zonep->zone_io_phyread_ops, 1);
		atomic_add_64(&zonep->zone_io_phyread_bytes, zp->io_size);
	} else {
		atomic_add_64(&zonep->zone_io_phywrite_ops, 1);
		atomic_add_64(&zonep->zone_io_phywrite_bytes, zp->io_size);
	}

	zone_rele(zonep);

	/*
	 * Probe with 2 args; the zone that issued the IO and the IO
	 * latency in nsecs.
	 */
	extern void __dtrace_probe_zfs__zone__latency(uintptr_t, uintptr_t);

	__dtrace_probe_zfs__zone__latency((uintptr_t)(zp->io_zoneid),
	    (uintptr_t)(diff));
}

#endif
