// SPDX-License-Identifier: GPL-2.0
/*
 * adaptive.c - the adaptive hybrid cpuidle governor
 *
 * Copyright (C) 2025, Ionut Nechita
 * Author: Ionut Nechita <ionut_n2001@yahoo.com>
 *
 * This governor combines features from multiple governors:
 * - Performance multiplier from Menu-TNG (2009) for workload awareness
 *   → Solves I/O performance regression on busy systems (Nehalem servers)
 *   → Original formula: exit_latency * multiplier > predicted_duration
 * - IRQ timing predictions from Mobile governor for accuracy
 *   → CONFIG_IRQ_TIMINGS for interrupt pattern learning
 * - Bucket-based correction from Menu governor for robustness
 *   → 12 buckets: 6 duration ranges × 2 I/O states
 * - EMA tracking from Mobile governor for pattern learning
 *   → Adaptive alpha parameter based on prediction accuracy
 * - Battery detection for dynamic AC/battery mode switching
 *   → Workqueue-based periodic detection (safe from idle path)
 * - Deep C-state boost from Mobile governor (20% prediction boost)
 *   → Favors power savings when system is actually idle
 * - Network activity detection (extension)
 *   → Fixes Mobile governor's 21% network latency regression
 *
 * Based on:
 * - menu.c by Adam Belay and Arjan van de Ven
 * - mobile.c by Ionut Nechita
 * - Menu-TNG patch by Arjan van de Ven (2009-09-11)
 *   https://lore.kernel.org/lkml/20090911174019.1ed02737@infradead.org/
 */

#include <linux/kernel.h>
#include <linux/cpuidle.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/stat.h>
#include <linux/sched/loadavg.h>
#include <linux/math64.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/kernel_stat.h>
#include <linux/workqueue.h>
#include <linux/percpu.h>

#include "gov.h"

/*
 * Access to scheduler runqueues for per-CPU load detection
 * This gives us true per-CPU load (Menu-TNG style) instead of global average.
 */
#include "../../kernel/sched/sched.h"

/*
 * Concepts behind the ADAPTIVE governor
 *
 * This governor solves the Mobile governor's 21% network latency regression
 * by adding workload awareness through a performance multiplier. The key
 * innovation is modulating C-state selection aggressiveness based on:
 *
 * 1) System workload (CPU load, IO wait, network activity)
 * 2) Power source (AC vs battery)
 * 3) Historical idle patterns (EMA, buckets, correction factors)
 *
 * Performance Multiplier Concept (from Menu-TNG):
 * ------------------------------------------------
 * multiplier = base + load_factor + io_factor + network_factor - battery_boost
 *
 * The multiplier is applied DIRECTLY to exit latency (Menu-TNG original):
 *   if (exit_latency * multiplier > predicted_idle_duration):
 *       skip this C-state (too deep)
 *
 * Higher multiplier → harder to enter deep states → better responsiveness
 * Lower multiplier → easier to enter deep states → better power savings
 *
 * Menu-TNG ranges (from 2009 patch):
 * - Idle system: multiplier ~1
 * - 1 CPU load (nr_running=1, load=10): mult = 1 + 2*10 = 21
 * - 5 IO wait tasks: mult = 1 + 10*5 = 51
 * - Busy server (load=50, io=3): mult = 1 + 100 + 30 = 131
 * - Battery mode: subtract 20 (promotes deep C-states)
 *
 * This solves the Nehalem server I/O performance problem (fio benchmark)
 * while maintaining power efficiency through adaptive selection.
 */

#define BUCKETS 12
#define INTERVAL_SHIFT 3
#define INTERVALS (1UL << INTERVAL_SHIFT)
#define RESOLUTION 1024
#define DECAY 4  /* Menu-TNG compatibility: faster adaptation (was 8) */
#define MAX_INTERESTING (50000 * NSEC_PER_USEC)

/* EMA constants (from Mobile) */
#define EMA_ALPHA_SHIFT_DEFAULT		7
#define EMA_ALPHA_SHIFT_MIN		6
#define EMA_ALPHA_SHIFT_MAX		8
#define MIN_IDLE_DURATION_US		100
#define MAX_IDLE_DURATION_US		(30000000UL)
#define FALLBACK_EMA_SHIFT		5

/* Performance multiplier constants (Menu-TNG compatible) */
#define BASE_MULTIPLIER			1	/* 1x - neutral (Menu-TNG style) */
#define MIN_MULTIPLIER			1	/* 1x - minimum barrier */
#define MAX_MULTIPLIER			200	/* 200x - maximum barrier for very busy systems */

/* Multiplier component values (Menu-TNG formulas, no normalization needed) */
/* Load and IO use Menu-TNG original formulas:
 * mult += 2 * get_loadavg()  where get_loadavg() = nr_running * 10
 * mult += 10 * nr_iowait_cpu()
 * These are applied directly without division, matching Menu-TNG behavior.
 */

/* Network activity factor (scaled for Menu-TNG range) */
#define NET_FACTOR_MODERATE		5	/* >5 softirqs/10ms */
#define NET_FACTOR_HEAVY		10	/* >20 softirqs/10ms */

/* Battery boost (scaled for Menu-TNG range) */
#define BATTERY_BOOST			20	/* Negative adjustment for battery mode */

/* Deep C-state boost from Mobile governor (optional) */
#define DEEP_CSTATE_BOOST_PERCENT	20	/* Add 20% to idle prediction for deep states */

/* Network activity detection */
#define NETWORK_CHECK_INTERVAL_NS	(10 * NSEC_PER_MSEC)
#define NETWORK_THRESHOLD_MODERATE	5
#define NETWORK_THRESHOLD_HEAVY		20

/* Prediction accuracy tracking */
#define MIN_SAMPLES_FOR_ADAPT		20
#define ACCURACY_THRESHOLD_LOW		60
#define ACCURACY_THRESHOLD_HIGH		85
#define STATS_UPDATE_INTERVAL		8

/* Governor rating - higher than mobile(25), menu(20), teo(19) */
#define ADAPTIVE_RATING			30

/* Battery detection interval */
#define BATTERY_CHECK_INTERVAL_MS	5000	/* Check every 5 seconds */

/*
 * Deep C-state promotion (from Mobile governor)
 * These thresholds are tuned for all Intel platforms (mobile, desktop, server).
 */
#define PROMOTION_IDLE_THRESHOLD_NS	(100 * NSEC_PER_USEC)	/* Min 100μs idle for promotion */
#define PROMOTION_LATENCY_THRESHOLD_NS	(18 * NSEC_PER_USEC)	/* Min 18μs latency_req (C2-like) */
#define PROMOTION_RESIDENCY_FACTOR	125	/* Accept 80% of target residency (125/100) */

/*
 * Enhanced tick stop logic (from Mobile governor)
 * Dynamic factors based on C-state depth for optimal tick management.
 *
 * Latency thresholds chosen for multi-platform compatibility:
 * - 150μs: Covers server C6 (150-200μs) and desktop/mobile C3 (200-350μs)
 * - 18μs: Standard C2-like intermediate state across all platforms
 */
#define MIN_TICK_STOP_DURATION_NS	(1000 * NSEC_PER_USEC)	/* Don't stop tick for < 1ms */
#define DEEP_STATE_LATENCY_NS		(150 * NSEC_PER_USEC)	/* 150μs - covers modern servers + desktop */
#define INTERMEDIATE_STATE_LATENCY_NS	(18 * NSEC_PER_USEC)	/* 18μs - C2-like states */
#define TICK_STOP_FACTOR_DEEP		105	/* Deep states: 1.05x - very aggressive */
#define TICK_STOP_FACTOR_INTERMEDIATE	110	/* Intermediate: 1.10x */
#define TICK_STOP_FACTOR_SHALLOW	115	/* Shallow: 1.15x - conservative */

/*
 * Load average macros are now in <linux/sched/loadavg.h>
 * We just need to declare external access to avenrun[]
 */

/*
 * ============================================================================
 * GLOBAL BATTERY DETECTION (Workqueue-based)
 * ============================================================================
 * Battery detection must run in a safe context (workqueue) because:
 * 1. power_supply API can sleep (mutex locks)
 * 2. May access I2C/SMBus drivers
 * 3. Cannot run from idle path (scheduler forbidden operations)
 *
 * Solution: Periodic workqueue updates a global atomic variable.
 * Idle path just reads the cached atomic value (lock-free, safe).
 *
 * Fallback logic (robust for desktops/servers without battery):
 * - If no MAINS power supply found → assume AC (desktop/server)
 * - If MAINS found but read fails → assume AC (safe fallback)
 * - If MAINS found and online=0 → battery mode
 * - If MAINS found and online=1 → AC mode
 */

/**
 * global_battery_mode - cached battery status (atomic, safe to read from idle)
 * 0 = AC power or unknown (desktop/server/fallback)
 * 1 = Battery power (laptop on battery)
 */
static atomic_t global_battery_mode = ATOMIC_INIT(0);

/**
 * battery_check_work - delayed work for periodic battery detection
 */
static struct delayed_work battery_check_work;

/**
 * battery_check_active - flag to control workqueue lifecycle
 * Set to 1 when first CPU enables governor (start workqueue)
 * Set to 0 when last CPU disables governor (stop workqueue)
 * This allows workqueue to run only when governor is actually in use.
 */
static atomic_t battery_check_active = ATOMIC_INIT(0);

/**
 * adaptive_users - count of CPUs using this governor
 * Used to start workqueue on first enable, stop on last disable.
 */
static atomic_t adaptive_users = ATOMIC_INIT(0);

/**
 * adaptive_battery_check_worker - workqueue callback for battery detection
 * @work: work_struct pointer
 *
 * Runs in process context where power_supply API is safe to use.
 * Updates global_battery_mode atomically.
 * Reschedules itself every BATTERY_CHECK_INTERVAL_MS.
 */
static void adaptive_battery_check_worker(struct work_struct *work)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret, on_battery = 0;
	bool found_mains = false;

	/*
	 * Try to find AC/MAINS power supply
	 * Common names: "AC", "AC0", "ACAD", "ADP1", etc.
	 * We iterate through class_for_each_device to find MAINS type.
	 */
	psy = power_supply_get_by_name("AC");
	if (!psy)
		psy = power_supply_get_by_name("AC0");
	if (!psy)
		psy = power_supply_get_by_name("ACAD");
	if (!psy)
		psy = power_supply_get_by_name("ADP1");

	if (psy) {
		found_mains = true;

		/* Read online property (1=connected, 0=disconnected) */
		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &val);
		if (ret == 0) {
			/* Success - use actual value */
			if (val.intval == 0)
				on_battery = 1;  /* AC offline → battery */
			else
				on_battery = 0;  /* AC online → AC power */
		} else {
			/* Read failed - assume AC (safe fallback) */
			on_battery = 0;
		}

		power_supply_put(psy);
	}

	/*
	 * Fallback for desktops/servers without battery:
	 * If no MAINS power supply found, assume AC mode.
	 * This prevents unnecessary deep C-states on systems
	 * where performance should be prioritized.
	 */
	if (!found_mains)
		on_battery = 0;

	/* Update global atomic (lock-free read from idle path) */
	atomic_set(&global_battery_mode, on_battery);

	/*
	 * Re-schedule for next check ONLY if governor is active.
	 * The workqueue starts when first CPU enables the governor,
	 * and stops when last CPU disables it. This prevents unnecessary
	 * battery checks when the governor isn't in use.
	 */
	if (atomic_read(&battery_check_active))
		schedule_delayed_work(&battery_check_work,
				      msecs_to_jiffies(BATTERY_CHECK_INTERVAL_MS));
}

/**
 * struct adaptive_device - per-CPU adaptive governor data
 * @needs_update: deferred update flag (from menu)
 * @tick_wakeup: tick caused wakeup (from menu)
 * @next_timer_ns: next timer event time (from menu)
 * @bucket: current bucket index for correction factors
 * @correction_factor: per-bucket correction factors (12 buckets)
 * @intervals: last 8 idle intervals for pattern detection
 * @interval_ptr: circular buffer pointer for intervals
 * @idle_ema_avg: exponential moving average for idle duration (from mobile)
 * @idle_total: accumulated idle time for current period (from mobile)
 * @last_jiffies: last reschedule timestamp (from mobile)
 * @ema_alpha_shift: adaptive EMA alpha parameter (from mobile)
 * @fallback_ema_short: short-term EMA for quick pattern detection (from mobile)
 * @irq_available: IRQ timing availability flag (from mobile)
 * @performance_multiplier: current performance multiplier (500-2000)
 * @network_activity_rx: NET_RX softirq counter cache
 * @network_activity_tx: NET_TX softirq counter cache
 * @last_workload_check: timestamp of last workload detection
 * @last_predicted_duration: last predicted idle duration (us)
 * @prediction_hits: accurate prediction count
 * @prediction_total: total prediction count
 * @stats_update_counter: periodic stats update counter
 *
 * Note: battery_mode removed - now uses global atomic (global_battery_mode)
 */
struct adaptive_device {
	/* Menu-style bucket correction */
	int		needs_update;
	int		tick_wakeup;
	u64		next_timer_ns;
	unsigned int	bucket;
	unsigned int	correction_factor[BUCKETS];
	unsigned int	intervals[INTERVALS];
	int		interval_ptr;

	/* Mobile-style EMA tracking */
	u64		idle_ema_avg;
	u64		idle_total;
	unsigned long	last_jiffies;
	u8		ema_alpha_shift;
	u64		fallback_ema_short;
	u8		irq_available;

	/* Performance multiplier */
	u32		performance_multiplier;

	/* Workload detection */
	u32		network_activity_rx;
	u32		network_activity_tx;
	u64		last_workload_check;

	/* Statistics */
	u64		last_predicted_duration;
	u16		prediction_hits;
	u16		prediction_total;
	u8		stats_update_counter;
};

static DEFINE_PER_CPU(struct adaptive_device, adaptive_devices);

/**
 * which_bucket - determine bucket index based on idle duration and IO wait
 * @duration_ns: idle duration in nanoseconds
 *
 * Returns bucket index (0-11) based on duration magnitude and IO wait state.
 * Menu-TNG approach: 12 buckets = 6 duration ranges × 2 IO states
 * - Buckets 0-5: no IO wait
 * - Buckets 6-11: with IO wait
 *
 * This allows us to track correction factors separately for IO vs non-IO
 * workloads, as they have very different wakeup patterns.
 */
static inline int which_bucket(u64 duration_ns)
{
	int bucket = 0;
	unsigned int duration_us = duration_ns / NSEC_PER_USEC;

	/*
	 * We keep two groups of stats; one with no IO pending, one with.
	 * This allows us to calculate E(duration)|iowait
	 * (Menu-TNG concept from Arjan van de Ven, 2009)
	 */
	if (nr_iowait_cpu(smp_processor_id()) > 0)
		bucket = BUCKETS / 2;  /* Offset by 6 for IO wait */

	/* Now add duration-based offset (0-5) */
	if (duration_us < 10)
		return bucket;
	if (duration_us < 100)
		return bucket + 1;
	if (duration_us < 1000)
		return bucket + 2;
	if (duration_us < 10000)
		return bucket + 3;
	if (duration_us < 100000)
		return bucket + 4;
	return bucket + 5;
}

/**
 * adaptive_update_intervals - update repeating interval detector
 * @data: adaptive device data
 * @interval_us: new interval in microseconds
 *
 * Circular buffer of last 8 intervals for pattern detection (from menu).
 */
static void adaptive_update_intervals(struct adaptive_device *data,
				      unsigned int interval_us)
{
	data->intervals[data->interval_ptr++] = interval_us;
	if (data->interval_ptr >= INTERVALS)
		data->interval_ptr = 0;
}

/**
 * adaptive_ema_new - calculate new EMA value
 * @value: new value to incorporate
 * @ema_old: previous EMA value
 * @alpha_shift: alpha parameter shift value (6-8)
 *
 * Returns new EMA value. From mobile governor.
 */
static u64 adaptive_ema_new(u64 value, u64 ema_old, u8 alpha_shift)
{
	u8 alpha_val;

	if (alpha_shift > 7)
		alpha_shift = 7;

	alpha_val = 1 << (8 - alpha_shift);

	if (likely(ema_old))
		return ema_old + (((value - ema_old) * alpha_val) >> alpha_shift);

	return value;
}

/**
 * adaptive_get_irq_prediction - get IRQ timing prediction
 * @now: current time in nanoseconds
 *
 * Returns predicted time to next IRQ in nanoseconds, or 0 if unavailable.
 * From mobile governor.
 */
static u64 adaptive_get_irq_prediction(u64 now)
{
	u64 irq_next, irq_duration;

	if (!IS_ENABLED(CONFIG_IRQ_TIMINGS))
		return 0;

	irq_next = irq_timings_next_event(now);

	if (irq_next <= now)
		return 0;

	irq_duration = irq_next - now;

	if (irq_duration > MAX_IDLE_DURATION_US * NSEC_PER_USEC)
		return 0;

	return irq_duration;
}

/* Forward declarations */
static void adaptive_update(struct cpuidle_driver *drv,
			    struct cpuidle_device *dev);
static unsigned int get_typical_interval(struct adaptive_device *data);

/**
 * get_typical_interval - detect repeating idle patterns
 * @data: adaptive device data
 *
 * Returns typical interval in microseconds if pattern detected, UINT_MAX otherwise.
 * Directly from menu governor - detects fixed-interval workloads.
 */
static unsigned int get_typical_interval(struct adaptive_device *data)
{
	s64 value, min_thresh = -1, max_thresh = UINT_MAX;
	unsigned int max, min, divisor;
	u64 avg, variance, avg_sq;
	int i;

again:
	max = 0;
	min = UINT_MAX;
	avg = 0;
	variance = 0;
	divisor = 0;

	for (i = 0; i < INTERVALS; i++) {
		value = data->intervals[i];

		if (value <= min_thresh || value >= max_thresh)
			continue;

		divisor++;
		avg += value;
		variance += value * value;

		if (value > max)
			max = value;
		if (value < min)
			min = value;
	}

	if (!max || divisor < 2)
		return UINT_MAX;

	if (divisor == INTERVALS) {
		avg >>= INTERVAL_SHIFT;
		variance >>= INTERVAL_SHIFT;
	} else {
		do_div(avg, divisor);
		do_div(variance, divisor);
	}

	avg_sq = avg * avg;
	variance -= avg_sq;

	/* Pattern detected if variance is low */
	if (likely(variance <= U64_MAX/36)) {
		if ((avg_sq > variance * 36 && divisor * 4 >= INTERVALS * 3) ||
		    variance <= 400)
			return avg;
	}

	/* Outlier removal and retry */
	if (divisor * 4 <= INTERVALS * 3)
		return UINT_MAX;

	if (avg - min > max - avg)
		min_thresh = min;
	else
		max_thresh = max;

	goto again;
}

/*
 * ============================================================================
 * PERFORMANCE MULTIPLIER CALCULATION
 * ============================================================================
 * These functions implement the Menu-TNG concept of workload-aware C-state
 * selection. The multiplier modulates how "expensive" we consider each
 * C-state's exit latency based on current system workload.
 */

/**
 * get_loadavg - get per-CPU load average (Menu-TNG compatible)
 *
 * Returns per-CPU load scaled to match Menu-TNG original behavior.
 * This is the TRUE Menu-TNG approach - per-CPU load awareness.
 *
 * Menu-TNG original (2009) used this_cpu_load() which returned the scheduler's
 * exponentially weighted load average per-CPU. That function no longer exists
 * in modern kernels, so we approximate using nr_running with adjusted scaling
 * to match original Menu-TNG behavior:
 *
 * Menu-TNG (load average):     Adaptive (nr_running):
 * - load 0.0 → 0                - nr_run 0 → 0
 * - load 0.5 → 5                - nr_run 1 → 5  (single thread ≈ 0.5 load avg)
 * - load 1.5 → 15               - nr_run 2 → 15
 * - load 3.0+ → 30+             - nr_run 3+ → 30+
 *
 * This preserves Menu-TNG's gentle response to light workloads (YouTube, browser)
 * while maintaining strong protection for truly busy systems (servers, compilation).
 *
 * This is CRITICAL for correctness on multi-core systems with non-uniform
 * workloads (e.g., thread pinning, HPC, databases):
 * - Per-CPU approach: Each CPU sees its own load → optimal C-state selection
 * - Global average approach: All CPUs see same load → suboptimal decisions
 *
 * Example on 8-core system:
 * - CPU0-3: 2 tasks each → load=15 → multiplier high → avoid deep C-states ✓
 * - CPU4-7: 0 tasks each → load=0 → multiplier low → allow deep C-states ✓
 * vs. global average: all CPUs see load=7.5 → suboptimal for both groups ✗
 */
static int get_loadavg(void)
{
	struct rq *rq = this_cpu_ptr(&runqueues);
	unsigned int nr_run;

	/*
	 * Read current CPU's runqueue depth directly.
	 * No locking needed - single atomic read of hot cacheline.
	 */
	nr_run = READ_ONCE(rq->nr_running);

	/*
	 * Scale to match Menu-TNG load average behavior:
	 * - 0 tasks: return 0 (idle)
	 * - 1 task:  return 5 (≈ load average 0.5, gentle for light workloads)
	 * - 2+ tasks: return nr_run * 10 (standard Menu-TNG scaling)
	 *
	 * This special case for nr_run=1 is crucial for desktop responsiveness:
	 * Single-threaded workloads (YouTube, web browsing) get multiplier ~11
	 * instead of ~21, allowing deeper C-states (C3) for better power efficiency.
	 */
	if (nr_run == 0)
		return 0;
	if (nr_run == 1)
		return 5;	/* Single thread ≈ load average 0.5 */

	/* For 2+ tasks, scale normally like Menu-TNG */
	return nr_run * 10;
}

/**
 * adaptive_calc_load_factor - calculate CPU load component of multiplier
 *
 * Returns multiplier contribution based on THIS CPU's runqueue depth.
 * Higher load = higher multiplier = avoid deep C-states.
 * Menu-TNG original formula: mult += 2 * get_loadavg()
 *
 * This is per-CPU aware: A busy CPU0 won't prevent idle CPU7 from entering
 * deep C-states. This is ESSENTIAL for performance on non-uniform workloads.
 */
static u32 adaptive_calc_load_factor(void)
{
	int load;

	/* Get THIS CPU's runqueue depth (TRUE Menu-TNG per-CPU approach) */
	load = get_loadavg();

	/*
	 * Menu-TNG original formula: mult += 2 * get_loadavg()
	 * With our Menu-TNG compatible scaling, this becomes:
	 * contribution = 2 * get_loadavg()
	 *
	 * Example values for THIS CPU (Menu-TNG compatible):
	 * - nr_running=0 (idle) → load=0 → contribution=0
	 * - nr_running=1 (1 task) → load=5 → contribution=10
	 * - nr_running=2 (2 tasks) → load=20 → contribution=40
	 * - nr_running=5 (5 tasks) → load=50 → contribution=100
	 *
	 * Final multiplier example (base=1, no IO/network):
	 * - Idle: mult = 1 + 0 = 1 (allow deep C-states)
	 * - 1 task: mult = 1 + 10 = 11 (gentle barrier - Menu-TNG compatible!)
	 * - 2 tasks: mult = 1 + 40 = 41 (moderate barrier)
	 * - 5 tasks: mult = 1 + 100 = 101 (high barrier, avoid deep states)
	 *
	 * The nr_running=1 → mult=11 case is crucial for desktop workloads:
	 * - YouTube video (1 decode thread): mult=11 → C3 latency 30μs × 11 = 330μs
	 *   → Accepts C3 for predicted > 330μs (vs 630μs with old mult=21)
	 * - Result: Better power efficiency without sacrificing responsiveness
	 */
	return 2 * load;
}

/**
 * adaptive_calc_io_factor - calculate IO wait component of multiplier
 *
 * Returns multiplier contribution based on IO-bound tasks.
 * IO workloads benefit from quick wakeups (Menu-TNG concept).
 * Menu-TNG original formula: mult += 10 * nr_iowait_cpu()
 */
static u32 adaptive_calc_io_factor(void)
{
	unsigned int nr_iowait;

	/* Use per-CPU nr_iowait_cpu() */
	nr_iowait = nr_iowait_cpu(smp_processor_id());

	/*
	 * Menu-TNG original formula: mult += 10 * nr_iowait_cpu()
	 * This scales linearly with number of IO-waiting tasks.
	 *
	 * Examples:
	 * - 0 IO tasks: contribution = 0
	 * - 3 IO tasks: contribution = 30
	 * - 5 IO tasks: contribution = 50
	 *
	 * This solves the Nehalem server I/O benchmark regression!
	 */
	return 10 * nr_iowait;
}

/**
 * adaptive_calc_network_factor - calculate network activity component
 * @data: adaptive device data
 *
 * Returns multiplier contribution based on softirq activity.
 * Network workloads are latency-sensitive (extension beyond Menu-TNG).
 * This addresses network latency regression from Mobile governor.
 */
static u32 adaptive_calc_network_factor(struct adaptive_device *data)
{
	u32 rx_new, tx_new, rx_delta, tx_delta;
	u64 now = local_clock();

	/* Rate-limit: only check every 10ms */
	if (now - data->last_workload_check < NETWORK_CHECK_INTERVAL_NS)
		return 0;

	data->last_workload_check = now;

	/* Read softirq counters */
	rx_new = kstat_softirqs_cpu(NET_RX_SOFTIRQ, smp_processor_id());
	tx_new = kstat_softirqs_cpu(NET_TX_SOFTIRQ, smp_processor_id());

	/* Calculate deltas */
	rx_delta = rx_new - data->network_activity_rx;
	tx_delta = tx_new - data->network_activity_tx;

	/* Update cached values */
	data->network_activity_rx = rx_new;
	data->network_activity_tx = tx_new;

	/*
	 * Detect network activity level (scaled for Menu-TNG range)
	 * Examples:
	 * - Heavy network: +10 to multiplier
	 * - Moderate network: +5 to multiplier
	 * - No network: +0
	 */
	if (rx_delta > NETWORK_THRESHOLD_HEAVY ||
	    tx_delta > NETWORK_THRESHOLD_HEAVY)
		return NET_FACTOR_HEAVY;	/* +10 */

	if (rx_delta > NETWORK_THRESHOLD_MODERATE ||
	    tx_delta > NETWORK_THRESHOLD_MODERATE)
		return NET_FACTOR_MODERATE;	/* +5 */

	return 0;
}

/**
 * adaptive_calc_battery_boost - calculate battery mode adjustment
 *
 * Returns multiplier adjustment for battery mode (extension beyond Menu-TNG).
 * On battery: returns positive value to SUBTRACT from multiplier (promotes deep C-states)
 * On AC: returns 0 (prefer responsiveness)
 *
 * Reads global atomic (updated by workqueue) - lock-free, safe from idle path.
 * Fallback: If battery detection unavailable (desktop/server), returns 0 (AC mode).
 *
 * Example: On battery with idle system (mult = 1 - 20 → clamped to MIN=1)
 * This allows deeper C-states on battery for power savings.
 */
static u32 adaptive_calc_battery_boost(void)
{
	int on_battery = atomic_read(&global_battery_mode);

	if (on_battery == 1)
		return BATTERY_BOOST;  /* Battery: -20 to multiplier */
	else
		return 0;  /* AC power or unknown: no adjustment */
}

/**
 * adaptive_update_multiplier - calculate and update performance multiplier
 * @data: adaptive device data
 *
 * Combines all factors using Menu-TNG formula:
 * mult = 1 + 2*get_loadavg() + 10*nr_iowait + network_factor - battery_boost
 *
 * Battery mode is read from global atomic (updated by workqueue).
 */
static void adaptive_update_multiplier(struct adaptive_device *data)
{
	u32 load, io, net, battery_boost;
	s32 multiplier;

	load = adaptive_calc_load_factor();
	io = adaptive_calc_io_factor();
	net = adaptive_calc_network_factor(data);
	battery_boost = adaptive_calc_battery_boost();

	/* Menu-TNG formula: base + load + io + net - battery_boost */
	multiplier = (s32)BASE_MULTIPLIER + load + io + net - battery_boost;

	/* Clamp to safe range [1, 200] */
	if (multiplier < MIN_MULTIPLIER)
		multiplier = MIN_MULTIPLIER;
	if (multiplier > MAX_MULTIPLIER)
		multiplier = MAX_MULTIPLIER;

	data->performance_multiplier = (u32)multiplier;
}

/*
 * ============================================================================
 * STATE SELECTION LOGIC
 * ============================================================================
 */

/**
 * adaptive_select - select next idle state
 * @drv: cpuidle driver
 * @dev: cpuidle device
 * @stop_tick: output - whether to stop scheduler tick
 *
 * Returns selected idle state index.
 */
static int adaptive_select(struct cpuidle_driver *drv,
			   struct cpuidle_device *dev,
			   bool *stop_tick)
{
	struct adaptive_device *data = this_cpu_ptr(&adaptive_devices);
	s64 latency_req = cpuidle_governor_latency_req(dev->cpu);
	u64 predicted_ns, irq_prediction, timer_us;
	ktime_t delta, delta_tick;
	u32 multiplier;
	int i, idx;

	/* Handle deferred update from reflect */
	if (data->needs_update) {
		adaptive_update(drv, dev);
		data->needs_update = 0;
	} else if (!dev->last_residency_ns) {
		/* Driver rejected state - update intervals */
		adaptive_update_intervals(data, UINT_MAX);
	}

	/* Update performance multiplier based on current workload */
	adaptive_update_multiplier(data);
	multiplier = data->performance_multiplier;

	/* Find shortest expected idle interval */
	predicted_ns = get_typical_interval(data) * NSEC_PER_USEC;

	if (predicted_ns > RESIDENCY_THRESHOLD_NS || tick_nohz_tick_stopped()) {
		/* Get timer-based prediction */
		delta = tick_nohz_get_sleep_length(&delta_tick);
		if (unlikely(delta < 0)) {
			delta = 0;
			delta_tick = 0;
		}

		data->next_timer_ns = delta;
		data->bucket = which_bucket(data->next_timer_ns);

		/* Apply bucket correction factor (from menu) */
		timer_us = div_u64((RESOLUTION * DECAY * NSEC_PER_USEC) / 2 +
				   data->next_timer_ns *
				   data->correction_factor[data->bucket],
				   RESOLUTION * DECAY * NSEC_PER_USEC);

		predicted_ns = min((u64)timer_us * NSEC_PER_USEC, predicted_ns);

		/* Try IRQ timing prediction (from mobile) */
		if (data->irq_available) {
			irq_prediction = adaptive_get_irq_prediction(local_clock());
			if (irq_prediction > 0) {
				predicted_ns = min(predicted_ns, irq_prediction);
			} else {
				data->irq_available = 0;
			}
		}

		/* Apply EMA constraint if available (from mobile) */
		if (data->idle_ema_avg > MIN_IDLE_DURATION_US) {
			u64 ema_ns = data->idle_ema_avg * NSEC_PER_USEC;
			predicted_ns = min(predicted_ns, ema_ns);
		}

		/*
		 * If the tick is already stopped, the cost of possible short
		 * idle duration misprediction is much higher, because the CPU
		 * may be stuck in a shallow idle state for a long time as a
		 * result of it.
		 *
		 * Instead of using next_timer_ns directly (which could be very
		 * large, e.g., 10ms), use the minimum of the prediction and the
		 * timer. This prevents selecting excessively deep C-states when
		 * the prediction suggests a short idle period, while still
		 * clamping to next_timer_ns to avoid unnecessarily shallow states.
		 */
		if (tick_nohz_tick_stopped() && predicted_ns < TICK_NSEC)
			predicted_ns = min(predicted_ns, data->next_timer_ns);
	} else {
		/*
		 * Because the next timer event is not going to be determined
		 * here (it is too far in the future for that), set
		 * next_timer_ns to a semi-random "distant future" value to
		 * avoid selecting a shallow state when a deep one is actually
		 * appropriate.
		 */
		data->next_timer_ns = KTIME_MAX;
		delta_tick = TICK_NSEC / 2;
		data->bucket = BUCKETS - 1;
	}

	/* Store prediction for accuracy tracking */
	data->last_predicted_duration = predicted_ns / NSEC_PER_USEC;

	/* Fast path for trivial cases */
	if (unlikely(drv->state_count <= 1 || latency_req == 0) ||
	    ((data->next_timer_ns < drv->states[1].target_residency_ns ||
	      latency_req < drv->states[1].exit_latency_ns) &&
	     !dev->states_usage[0].disable)) {
		*stop_tick = !(drv->states[0].flags & CPUIDLE_FLAG_POLLING);
		return 0;
	}

	/*
	 * STATE SELECTION WITH PERFORMANCE MULTIPLIER (Menu-TNG algorithm)
	 * This is the key innovation - filter states by exit_latency * multiplier
	 *
	 * Menu-TNG check: if (exit_latency * multiplier > predicted_duration)
	 *   → State is too deep for current workload, reject it
	 *
	 * Mobile optimization: Add 20% boost to predicted_ns for deep C-states
	 * to favor power savings when system is actually idle.
	 */
	if (predicted_ns > 200 * NSEC_PER_USEC) {
		/* Boost prediction by 20% for deep C-states (from Mobile) */
		predicted_ns = (predicted_ns * (100 + DEEP_CSTATE_BOOST_PERCENT)) / 100;
	}

	idx = -1;
	for (i = 0; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];

		if (dev->states_usage[i].disable)
			continue;

		if (idx == -1)
			idx = i;  /* First enabled state */

		/* Latency constraint from PM QoS */
		if (s->exit_latency_ns > latency_req)
			break;

		/*
		 * MENU-TNG PERFORMANCE MULTIPLIER CHECK (original formula)
		 * Directly multiply exit latency by performance multiplier.
		 * No normalization - this preserves Menu-TNG's strong response
		 * to workload (e.g., mult=51 on busy I/O systems).
		 */
		if (s->exit_latency_ns * multiplier > predicted_ns)
			break;  /* Too deep for current workload */

		/* Target residency check */
		if (s->target_residency_ns <= predicted_ns) {
			idx = i;
			continue;
		}

		/* Special handling for polling vs physical states */
		if ((drv->states[idx].flags & CPUIDLE_FLAG_POLLING) &&
		    s->target_residency_ns < RESIDENCY_THRESHOLD_NS &&
		    s->target_residency_ns <= data->next_timer_ns &&
		    s->exit_latency_ns <= predicted_ns) {
			predicted_ns = s->target_residency_ns;
			idx = i;
			break;
		}

		if (predicted_ns < TICK_NSEC)
			break;

		if (!tick_nohz_tick_stopped()) {
			predicted_ns = drv->states[idx].target_residency_ns;
			break;
		}

		/* Avoid getting stuck in shallow states */
		if (drv->states[idx].target_residency_ns < TICK_NSEC &&
		    s->target_residency_ns <= delta_tick)
			idx = i;

		break;
	}

	if (idx == -1)
		idx = 0;

	/*
	 * AGGRESSIVE DEEP C-STATE PROMOTION (from Mobile governor)
	 *
	 * When a shallow state (POLL or C1) is selected, but we have sufficient
	 * idle duration and latency headroom, try to promote to deeper states
	 * (C2+) for better power savings.
	 *
	 * This is particularly effective on laptops and idle systems where:
	 * - Standard selection picks C1 conservatively
	 * - But we actually have time for C3/C6
	 * - Result: Better battery life without responsiveness loss
	 *
	 * We accept states with only 80% of target residency (125% factor),
	 * being more aggressive than standard selection (100%).
	 *
	 * This optimization is COMPLEMENTARY to Menu-TNG multiplier:
	 * - Menu-TNG prevents deep states on BUSY workloads (high multiplier)
	 * - This promotes deep states on IDLE scenarios (low multiplier passed)
	 */
	if (idx <= 1 && drv->state_count > 2) {
		/* Currently selected POLL or C1 - try to promote */
		if (predicted_ns > PROMOTION_IDLE_THRESHOLD_NS &&
		    latency_req >= PROMOTION_LATENCY_THRESHOLD_NS) {
			/* We have sufficient idle time and latency headroom */
			for (i = 2; i < drv->state_count; i++) {
				struct cpuidle_state *s = &drv->states[i];

				if (dev->states_usage[i].disable)
					continue;

				/* Respect latency constraint */
				if (s->exit_latency_ns > latency_req)
					break;

				/*
				 * Menu-TNG multiplier check still applies!
				 * This ensures we don't promote on busy workloads.
				 */
				if (s->exit_latency_ns * multiplier > predicted_ns)
					break;

				/*
				 * Aggressive residency check: accept 80% of target
				 * (standard would require 100%)
				 * Formula: target_residency * 125 / 100 > predicted_ns
				 */
				if (s->target_residency_ns >
				    (predicted_ns * PROMOTION_RESIDENCY_FACTOR / 100))
					break;

				/* This state is acceptable - promote to it */
				idx = i;
			}
		}
	}

	/*
	 * ENHANCED TICK STOP DECISION (from Mobile governor)
	 *
	 * Dynamic tick stop logic with factors based on C-state depth:
	 * - Deep states (>=150μs): factor 105% → almost always stop tick
	 * - Intermediate (18-150μs): factor 110% → aggressive
	 * - Shallow (<18μs): factor 115% → conservative
	 *
	 * Always stop tick for C2+ (index >= 2) to maintain deep state residency.
	 *
	 * This replaces the simple battery/AC boolean decision with a more
	 * nuanced approach that considers:
	 * 1. C-state depth (deeper = more aggressive tick stop)
	 * 2. Idle duration vs target residency (with dynamic factor)
	 * 3. Prediction availability (EMA or IRQ timing)
	 */
	if (idx > 0) {
		/* Physical C-state selected (not POLL) */
		struct cpuidle_state *selected_state = &drv->states[idx];
		u32 tick_stop_factor;
		bool has_prediction;

		/*
		 * Dynamic tick stop factor based on C-state depth:
		 * Deeper states get more aggressive tick stopping (lower factor).
		 *
		 * Rationale:
		 * - Deep states: Already paid high entry cost → maximize residency
		 * - Shallow states: Low entry cost → more conservative
		 *
		 * Thresholds work across all Intel platforms:
		 * - 150μs: Raptor Lake C3 (350μs), Sapphire Rapids C6 (150μs)
		 * - 18μs: C2-like states on all platforms
		 */
		if (selected_state->exit_latency_ns >= DEEP_STATE_LATENCY_NS) {
			/* Deep C-state (C3/C6+): very aggressive */
			tick_stop_factor = TICK_STOP_FACTOR_DEEP;  /* 105% */
		} else if (selected_state->exit_latency_ns >= INTERMEDIATE_STATE_LATENCY_NS) {
			/* Intermediate (C2): aggressive */
			tick_stop_factor = TICK_STOP_FACTOR_INTERMEDIATE;  /* 110% */
		} else {
			/* Shallow (C1): conservative */
			tick_stop_factor = TICK_STOP_FACTOR_SHALLOW;  /* 115% */
		}

		/*
		 * Check if we should stop tick:
		 * 1. Idle duration must exceed target residency * factor
		 * 2. Minimum 1ms idle duration (avoid thrashing)
		 * 3. Have prediction data (IRQ timing or EMA)
		 */
		has_prediction = (data->irq_available != 0) ||
				 (data->idle_ema_avg > MIN_IDLE_DURATION_US);

		*stop_tick = ((predicted_ns * 100) >
			      (selected_state->target_residency_ns * tick_stop_factor)) &&
			     (predicted_ns > MIN_TICK_STOP_DURATION_NS) &&
			     has_prediction;

		/*
		 * Always stop tick for C2 and deeper (index >= 2).
		 * This ensures we maintain deep state residency and don't
		 * get woken up by the tick prematurely.
		 */
		if (idx >= 2)
			*stop_tick = true;

		/*
		 * Fallback: If selected state requires more than half-tick
		 * and tick is not stopped, try to find shallower state.
		 */
		if (!(*stop_tick) && !tick_nohz_tick_stopped() &&
		    selected_state->target_residency_ns > delta_tick) {
			for (i = idx - 1; i >= 0; i--) {
				if (dev->states_usage[i].disable)
					continue;
				idx = i;
				if (drv->states[i].target_residency_ns <= delta_tick)
					break;
			}
		}
	} else {
		/* POLL state - never stop tick */
		*stop_tick = false;
	}

	return idx;
}

/*
 * ============================================================================
 * FEEDBACK MECHANISM
 * ============================================================================
 */

/**
 * adaptive_reflect - record state for deferred update
 * @dev: cpuidle device
 * @index: idle state that was entered
 *
 * Fast path - just record data for later processing.
 */
static void adaptive_reflect(struct cpuidle_device *dev, int index)
{
	struct adaptive_device *data = this_cpu_ptr(&adaptive_devices);

	dev->last_state_idx = index;
	data->needs_update = 1;
	data->tick_wakeup = tick_nohz_idle_got_tick();
}

/**
 * adaptive_adapt_ema_parameters - tune EMA responsiveness
 * @data: adaptive device data
 *
 * Adapts alpha based on prediction accuracy (from mobile).
 */
static void adaptive_adapt_ema_parameters(struct adaptive_device *data)
{
	u32 accuracy;

	if (data->prediction_total < MIN_SAMPLES_FOR_ADAPT)
		return;

	accuracy = (data->prediction_hits * 100) / data->prediction_total;

	if (accuracy < ACCURACY_THRESHOLD_LOW) {
		/* Low accuracy - adapt faster */
		if (data->ema_alpha_shift > EMA_ALPHA_SHIFT_MIN)
			data->ema_alpha_shift--;
	} else if (accuracy > ACCURACY_THRESHOLD_HIGH) {
		/* High accuracy - more stable */
		if (data->ema_alpha_shift < EMA_ALPHA_SHIFT_MAX)
			data->ema_alpha_shift++;
	}
}

/**
 * adaptive_update - update correction factors and statistics
 * @drv: cpuidle driver
 * @dev: cpuidle device
 *
 * Heavy lifting happens here - update all tracking structures.
 */
static void adaptive_update(struct cpuidle_driver *drv,
			    struct cpuidle_device *dev)
{
	struct adaptive_device *data = this_cpu_ptr(&adaptive_devices);
	int last_idx = dev->last_state_idx;
	struct cpuidle_state *target = &drv->states[last_idx];
	u64 measured_ns, residency_us;
	unsigned int new_factor;

	/*
	 * Figure out actual idle duration (from menu logic)
	 */
	if (data->tick_wakeup && data->next_timer_ns > TICK_NSEC) {
		measured_ns = 9 * MAX_INTERESTING / 10;
	} else if ((drv->states[last_idx].flags & CPUIDLE_FLAG_POLLING) &&
		   dev->poll_time_limit) {
		measured_ns = data->next_timer_ns;
	} else {
		measured_ns = dev->last_residency_ns;

		/* Deduct exit latency */
		if (measured_ns > 2 * target->exit_latency_ns)
			measured_ns -= target->exit_latency_ns;
		else
			measured_ns /= 2;
	}

	/* Clamp to timer boundary */
	if (measured_ns > data->next_timer_ns)
		measured_ns = data->next_timer_ns;

	residency_us = measured_ns / NSEC_PER_USEC;

	/*
	 * UPDATE BUCKET CORRECTION FACTOR (from menu)
	 */
	new_factor = data->correction_factor[data->bucket];
	new_factor -= new_factor / DECAY;

	if (data->next_timer_ns > 0 && measured_ns < MAX_INTERESTING)
		new_factor += div64_u64(RESOLUTION * measured_ns,
					data->next_timer_ns);
	else
		new_factor += RESOLUTION;

	if (unlikely(new_factor == 0))
		new_factor = 1;

	data->correction_factor[data->bucket] = new_factor;

	/*
	 * UPDATE REPEATING INTERVAL DETECTOR (from menu)
	 */
	adaptive_update_intervals(data, residency_us);

	/*
	 * UPDATE EMA (from mobile)
	 */
	data->idle_total += residency_us;
	data->fallback_ema_short = adaptive_ema_new(residency_us,
						    data->fallback_ema_short,
						    FALLBACK_EMA_SHIFT);

	/* Check if reschedule happened */
	if (need_resched() ||
	    !time_after(jiffies, data->last_jiffies +
			msecs_to_jiffies(100))) {
		data->idle_ema_avg = adaptive_ema_new(data->idle_total,
						      data->idle_ema_avg,
						      data->ema_alpha_shift);
		data->idle_total = 0;
		data->last_jiffies = jiffies;

		/* Adapt EMA parameters */
		adaptive_adapt_ema_parameters(data);
	}

	/*
	 * UPDATE PREDICTION STATISTICS
	 */
	data->stats_update_counter++;
	if (data->stats_update_counter >= STATS_UPDATE_INTERVAL) {
		data->stats_update_counter = 0;

		if (data->last_predicted_duration > 0 && residency_us > 0) {
			u64 predicted = data->last_predicted_duration;
			u64 diff = (residency_us > predicted) ?
				   (residency_us - predicted) :
				   (predicted - residency_us);

			data->prediction_total++;

			/* Hit if within 25% */
			if (diff <= (predicted >> 2))
				data->prediction_hits++;

			/* Prevent overflow */
			if (data->prediction_total > 1000) {
				data->prediction_hits >>= 1;
				data->prediction_total >>= 1;
			}
		}
	}

	/* Periodically re-check IRQ timing availability */
	if (!data->irq_available && (jiffies % 1000) == 0)
		data->irq_available = 1;
}

/*
 * ============================================================================
 * INITIALIZATION AND REGISTRATION
 * ============================================================================
 */

/**
 * adaptive_enable_device - initialize per-CPU data and start workqueue
 * @drv: cpuidle driver
 * @dev: cpuidle device
 *
 * Called when a CPU selects this governor. On first enable (user count 0→1),
 * starts the battery check workqueue.
 *
 * Returns 0 on success.
 */
static int adaptive_enable_device(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev)
{
	struct adaptive_device *data = &per_cpu(adaptive_devices, dev->cpu);
	int i, prev_users;

	memset(data, 0, sizeof(*data));

	/* Initialize correction factors to unity (from menu) */
	for (i = 0; i < BUCKETS; i++)
		data->correction_factor[i] = RESOLUTION * DECAY;

	/* Initialize EMA parameters (from mobile) */
	data->ema_alpha_shift = EMA_ALPHA_SHIFT_DEFAULT;
	data->irq_available = 1;

	/* Initialize performance multiplier */
	data->performance_multiplier = BASE_MULTIPLIER;

	/*
	 * Increment user count. If this is the first user (0→1),
	 * start the battery check workqueue.
	 */
	prev_users = atomic_inc_return(&adaptive_users);
	if (prev_users == 1) {
		/* First CPU enabling governor - start workqueue */
		atomic_set(&battery_check_active, 1);
		schedule_delayed_work(&battery_check_work, 0);
	}

	return 0;
}

/**
 * adaptive_disable_device - cleanup per-CPU data and stop workqueue if needed
 * @drv: cpuidle driver
 * @dev: cpuidle device
 *
 * Called when a CPU deselects this governor. On last disable (user count 1→0),
 * stops the battery check workqueue.
 */
static void adaptive_disable_device(struct cpuidle_driver *drv,
				    struct cpuidle_device *dev)
{
	int remaining_users;

	/*
	 * Decrement user count. If this was the last user (1→0),
	 * stop the battery check workqueue.
	 */
	remaining_users = atomic_dec_return(&adaptive_users);
	if (remaining_users == 0) {
		/*
		 * Last CPU disabling governor - stop workqueue.
		 *
		 * Sequence:
		 * 1. Clear active flag → prevents re-scheduling
		 * 2. Cancel pending work and wait for completion
		 */
		atomic_set(&battery_check_active, 0);
		cancel_delayed_work_sync(&battery_check_work);
	}

	/* Note: per-CPU data cleanup not needed - will be reinitialized on next enable */
}

static struct cpuidle_governor adaptive_governor = {
	.name		= "adaptive",
	.rating		= ADAPTIVE_RATING,
	.enable		= adaptive_enable_device,
	.disable	= adaptive_disable_device,
	.select		= adaptive_select,
	.reflect	= adaptive_reflect,
};

/**
 * adaptive_governor_init - register the adaptive governor
 *
 * Initializes the governor and registers it with cpuidle subsystem.
 * The battery check workqueue is initialized but not started here -
 * it will be started by adaptive_enable_device() when first CPU
 * selects this governor.
 */
static int __init adaptive_governor_init(void)
{
	/* Enable IRQ timings if available */
#ifdef CONFIG_IRQ_TIMINGS
	irq_timings_enable();
#endif

	/*
	 * Initialize battery detection workqueue (but don't start it).
	 * The workqueue will be started when first CPU enables the governor,
	 * and stopped when last CPU disables it.
	 */
	INIT_DELAYED_WORK(&battery_check_work, adaptive_battery_check_worker);

	/* Register governor with cpuidle subsystem */
	return cpuidle_register_governor(&adaptive_governor);
}

postcore_initcall(adaptive_governor_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ionut Nechita <ionut_n2001@yahoo.com>");
MODULE_DESCRIPTION("Adaptive hybrid cpuidle governor");

