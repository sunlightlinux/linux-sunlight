// SPDX-License-Identifier: GPL-2.0
/*
 * Mobile CPU Governor - Battery-optimized governor for mobile systems
 *
 * Copyright (C) 2025, Ionut Nechita
 * Author: Ionut Nechita <ionut_n2001@yahoo.com>
 *
 * This governor combines the best features from multiple implementations:
 * - Simple and robust like the menu governor
 * - Enhanced error handling and validation
 * - Adaptive parameters for different workloads
 * - Better exit reason detection
 */

#include <linux/cpuidle.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/time.h>

/**
 * struct mobile_device - per-CPU data for mobile governor
 * @idle_ema_avg: exponential moving average for idle duration
 * @idle_total: accumulated idle time for current scheduling period
 * @last_jiffies: last time the idle task was rescheduled
 * @last_enter_time: timestamp when last entered idle state
 * @last_predicted_duration: last predicted idle duration for accuracy tracking
 * @exit_reason: reason for exiting idle (0=timer, 1=irq, 2=resched)
 * @ema_alpha_shift: adaptive EMA alpha parameter (higher = more stable)
 * @prediction_hits: number of accurate predictions
 * @prediction_total: total number of predictions made
 * @stats_update_counter: counter for periodic statistics updates
 * @fallback_ema_short: short-term EMA for quick pattern detection
 * @irq_available: cached IRQ timing availability flag
 */
struct mobile_device {
	/* Core EMA tracking */
	u64			idle_ema_avg;
	u64			idle_total;
	unsigned long		last_jiffies;

	/* Enhanced tracking for better decisions */
	u64			last_enter_time;
	u64			last_predicted_duration;
	u8			exit_reason;

	/* Adaptive EMA parameters */
	u8			ema_alpha_shift;

	/* Statistics for tuning - updated less frequently */
	u16			prediction_hits;
	u16			prediction_total;
	u8			stats_update_counter;

	/* Fallback mechanism when IRQ timings unavailable */
	u64			fallback_ema_short;
	u8			irq_available;
};

/* Core constants */
#define EMA_ALPHA_VAL_DEFAULT		64
#define EMA_ALPHA_SHIFT_DEFAULT		7
#define MAX_RESCHED_INTERVAL_MS		100

/* Enhanced constants for robustness */
#define EMA_ALPHA_SHIFT_MIN		6
#define EMA_ALPHA_SHIFT_MAX		8
#define MIN_IDLE_DURATION_US		100  /* Minimum idle duration to consider */
#define MAX_IDLE_DURATION_US		(30000000UL)  /* 30 seconds maximum */
#define MOBILE_RATING			25

/* Accuracy threshold for EMA adaptation */
#define ACCURACY_THRESHOLD_LOW		60
#define ACCURACY_THRESHOLD_HIGH		85
#define MIN_SAMPLES_FOR_ADAPT		20
#define STATS_UPDATE_INTERVAL		8

/* Fallback EMA parameters for when IRQ timings unavailable */
#define FALLBACK_EMA_SHIFT		5

/* Generic thresholds for deep C-state detection */
#define MIN_TICK_STOP_DURATION_US	1000  /* Don't stop tick for < 1ms idle */
#define DEEP_STATE_LATENCY_US		300   /* Latency threshold for deep C-states */

static DEFINE_PER_CPU(struct mobile_device, mobile_devices);

/**
 * mobile_ema_new - enhanced EMA calculation with adaptive alpha
 * @value: new value to incorporate
 * @ema_old: previous EMA value
 * @alpha_shift: alpha parameter shift value
 *
 * Return: new EMA value
 */
static u64 mobile_ema_new(u64 value, u64 ema_old, u8 alpha_shift)
{
	u8 alpha_val;

	/* Validate alpha_shift to prevent invalid shifts */
	if (alpha_shift > 7)
		alpha_shift = 7;

	alpha_val = 1 << (8 - alpha_shift);

	if (likely(ema_old))
		return ema_old + (((value - ema_old) * alpha_val) >> alpha_shift);

	return value;
}

/**
 * mobile_get_residency - safe way to get last residency
 * @dev: cpuidle device
 *
 * Return: last residency in microseconds, 0 if unavailable
 */
static u64 mobile_get_residency(struct cpuidle_device *dev)
{
	/* In newer kernels, residency is in nanoseconds */
	if (dev->last_residency_ns)
		return dev->last_residency_ns / 1000; /* Convert ns to us */
	else
		return 0; /* Fallback */
}

/**
 * mobile_get_irq_prediction - robust IRQ timing prediction with fallback
 * @now: current time in nanoseconds
 *
 * Return: predicted IRQ duration in nanoseconds, 0 if unavailable
 */
static u64 mobile_get_irq_prediction(u64 now)
{
	u64 irq_next;
	u64 irq_duration;

	/* Check if IRQ timings are available */
	if (!IS_ENABLED(CONFIG_IRQ_TIMINGS))
		return 0;

	/* Try to get IRQ prediction, with error handling */
	irq_next = irq_timings_next_event(now);

	/* Validate IRQ timing result */
	if (irq_next <= now)
		return 0; /* No valid prediction */

	irq_duration = irq_next - now;

	/* Sanity check - reject unreasonable predictions */
	if (irq_duration > MAX_IDLE_DURATION_US * NSEC_PER_USEC)
		return 0;

	return irq_duration;
}

/**
 * mobile_detect_exit_reason - improved exit reason detection
 * @mobile_dev: per-CPU mobile device data
 * @dev: cpuidle device
 */
static void mobile_detect_exit_reason(struct mobile_device *mobile_dev,
				      struct cpuidle_device *dev)
{
	u64 actual_residency = mobile_get_residency(dev);
	u64 predicted_duration = mobile_dev->last_predicted_duration;

	/* Default to timer/normal exit */
	mobile_dev->exit_reason = 0;

	/* If we have both actual and predicted data to compare */
	if (predicted_duration > 0 && actual_residency > 0) {
		/* If we woke up much earlier than predicted, likely IRQ */
		if (actual_residency < (predicted_duration / 2)) {
			mobile_dev->exit_reason = 1; /* IRQ */
		} else if (need_resched()) {
			mobile_dev->exit_reason = 2; /* Resched */
		}
		/* else: timer or close to prediction = normal */
	} else if (need_resched()) {
		mobile_dev->exit_reason = 2; /* Resched */
	}
}

/**
 * mobile_update_prediction_stats - track prediction accuracy (called less frequently)
 * @mobile_dev: per-CPU mobile device data
 * @dev: cpuidle device
 */
static void mobile_update_prediction_stats(struct mobile_device *mobile_dev,
					   struct cpuidle_device *dev)
{
	u64 actual = mobile_get_residency(dev);
	u64 predicted = mobile_dev->last_predicted_duration;
	u64 diff;

	/* Only update stats periodically to reduce overhead */
	mobile_dev->stats_update_counter++;
	if (mobile_dev->stats_update_counter < STATS_UPDATE_INTERVAL)
		return;

	mobile_dev->stats_update_counter = 0;

	if (predicted > 0 && actual > 0) {
		mobile_dev->prediction_total++;

		/* Calculate absolute difference safely */
		diff = (actual > predicted) ? (actual - predicted) : (predicted - actual);

		/* Consider it a hit if within 25% of prediction */
		if (diff <= (predicted >> 2))
			mobile_dev->prediction_hits++;

		/* Prevent overflow */
		if (mobile_dev->prediction_total > 1000) {
			mobile_dev->prediction_hits >>= 1;
			mobile_dev->prediction_total >>= 1;
		}
	}
}

/**
 * mobile_adapt_ema_parameters - adaptive EMA tuning
 * @mobile_dev: per-CPU mobile device data
 */
static void mobile_adapt_ema_parameters(struct mobile_device *mobile_dev)
{
	u32 accuracy;

	/* Need minimum samples before adapting */
	if (mobile_dev->prediction_total < MIN_SAMPLES_FOR_ADAPT)
		return;

	accuracy = (mobile_dev->prediction_hits * 100) / mobile_dev->prediction_total;

	/* Adapt EMA responsiveness based on accuracy */
	if (accuracy < ACCURACY_THRESHOLD_LOW) {
		/* Low accuracy - adapt faster */
		if (mobile_dev->ema_alpha_shift > EMA_ALPHA_SHIFT_MIN)
			mobile_dev->ema_alpha_shift--;
	} else if (accuracy > ACCURACY_THRESHOLD_HIGH) {
		/* High accuracy - can afford to be more stable */
		if (mobile_dev->ema_alpha_shift < EMA_ALPHA_SHIFT_MAX)
			mobile_dev->ema_alpha_shift++;
	}
}

/**
 * mobile_reflect - enhanced reflection with better tracking
 * @dev: cpuidle device
 * @index: idle state that was entered
 */
static void mobile_reflect(struct cpuidle_device *dev, int index)
{
	struct mobile_device *mobile_dev = this_cpu_ptr(&mobile_devices);
	u64 residency_us = mobile_get_residency(dev);

	/* Validate residency (basic sanity check) */
	if (residency_us == 0 || residency_us > MAX_IDLE_DURATION_US)
		return;

	/* Detect why we exited idle */
	mobile_detect_exit_reason(mobile_dev, dev);

	/* Update prediction accuracy stats */
	mobile_update_prediction_stats(mobile_dev, dev);

	/* Clear stats if idle task wasn't rescheduled recently */
	if (time_after(jiffies, mobile_dev->last_jiffies +
		       msecs_to_jiffies(MAX_RESCHED_INTERVAL_MS))) {
		mobile_dev->idle_ema_avg = 0;
		/* Reset stats but keep EMA parameters */
		mobile_dev->prediction_hits = 0;
		mobile_dev->prediction_total = 0;
	}

	/* Always accumulate total idle time */
	mobile_dev->idle_total += residency_us;

	/* Update short-term fallback EMA for all exits */
	mobile_dev->fallback_ema_short = mobile_ema_new(residency_us,
							mobile_dev->fallback_ema_short,
							FALLBACK_EMA_SHIFT);

	/* Update EMA only for meaningful reschedules */
	if (mobile_dev->exit_reason == 2) { /* Resched */
		mobile_dev->idle_ema_avg = mobile_ema_new(mobile_dev->idle_total,
							  mobile_dev->idle_ema_avg,
							  mobile_dev->ema_alpha_shift);
		mobile_dev->idle_total = 0;
		mobile_dev->last_jiffies = jiffies;

		/* Adapt EMA parameters based on recent performance */
		mobile_adapt_ema_parameters(mobile_dev);
	}

	/* Periodically re-enable IRQ timings if disabled */
	if (!mobile_dev->irq_available && (jiffies % 1000) == 0)
		mobile_dev->irq_available = 1; /* Re-test every ~1000 jiffies */
}

/**
 * mobile_select - enhanced state selection with robust prediction and fallback
 * @drv: cpuidle driver
 * @dev: cpuidle device
 * @stop_tick: whether to stop the scheduler tick
 *
 * Return: selected idle state index
 */
static int mobile_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
			 bool *stop_tick)
{
	struct mobile_device *mobile_dev = this_cpu_ptr(&mobile_devices);
	int latency_req = cpuidle_governor_latency_req(dev->cpu);
	int i, index = 0;
	ktime_t delta_next;
	u64 now, irq_length, timer_length;
	u64 idle_duration_us;
	bool has_irq_prediction = false;

	now = local_clock();
	mobile_dev->last_enter_time = now;

	/* Get IRQ prediction with validation - but cache availability check */
	if (mobile_dev->irq_available) {
		irq_length = mobile_get_irq_prediction(now);
		has_irq_prediction = (irq_length > 0);

		/* If IRQ prediction failed multiple times, disable it temporarily */
		if (!has_irq_prediction)
			mobile_dev->irq_available = 0; /* Will re-enable periodically */
	}

	/* Get timer duration */
	timer_length = ktime_to_ns(tick_nohz_get_sleep_length(&delta_next));

	/* Combine predictions intelligently with enhanced fallback */
	if (has_irq_prediction) {
		idle_duration_us = min_t(u64, irq_length, timer_length) / NSEC_PER_USEC;
	} else {
		/* Fallback when no IRQ prediction available */
		idle_duration_us = timer_length / NSEC_PER_USEC;

		/* Use short-term EMA for quick pattern detection */
		if (mobile_dev->fallback_ema_short > MIN_IDLE_DURATION_US) {
			idle_duration_us = min_t(u64, idle_duration_us,
						 mobile_dev->fallback_ema_short);
		} else if (mobile_dev->idle_ema_avg > 0) {
			/* Conservative fallback */
			idle_duration_us = min_t(u64, idle_duration_us,
						 mobile_dev->idle_ema_avg * 3 / 4);
		}
	}

	/* Apply main EMA constraint if available */
	if (mobile_dev->idle_ema_avg > MIN_IDLE_DURATION_US) {
		idle_duration_us = min_t(u64, idle_duration_us,
					 mobile_dev->idle_ema_avg);
	}

	/* Optimization: Boost idle prediction to favor deeper C-states
	 * This helps avoid getting stuck in shallow states */
	if (idle_duration_us > 200) {
		/* Add 20% boost to idle prediction for deeper state consideration */
		idle_duration_us = (idle_duration_us * 120) / 100;
	}

	/* Final validation of idle duration */
	if (idle_duration_us < MIN_IDLE_DURATION_US)
		idle_duration_us = MIN_IDLE_DURATION_US;
	else if (idle_duration_us > MAX_IDLE_DURATION_US)
		idle_duration_us = MAX_IDLE_DURATION_US;

	/* Store prediction for later accuracy checking */
	mobile_dev->last_predicted_duration = idle_duration_us;

	/* Select appropriate idle state - prefer deeper C-states for Ryzen */
	for (i = 0; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];

		if (su->disable)
			continue;

		if (s->exit_latency > latency_req)
			break;

		if (s->target_residency > idle_duration_us)
			break;

		index = i;
	}

	/* Optimization: Aggressively promote to deeper C-states
	 * Skip shallow states when possible and prefer deeper states for better power savings */
	if (index <= 1 && drv->state_count > 2) {  /* Currently selected POLL or first C-state */
		/* Check if we can use deeper C-states instead */
		if (idle_duration_us > 100 && latency_req >= 18) {
			/* Try to use at least intermediate C-states */
			for (i = 2; i < drv->state_count; i++) {
				struct cpuidle_state *s = &drv->states[i];
				struct cpuidle_state_usage *su = &dev->states_usage[i];

				if (su->disable)
					continue;

				if (s->exit_latency > latency_req)
					break;

				/* Be more aggressive - accept if we have 80% of target residency */
				if (s->target_residency > (idle_duration_us * 125 / 100))
					break;

				index = i;
			}
		}
	}

	/* Enhanced stop_tick logic with dynamic factors based on C-state depth */
	if (index > 0) {
		struct cpuidle_state *selected_state = &drv->states[index];

		/* Dynamic factor based on selected C-state depth */
		u32 tick_stop_factor;

		/* More aggressive tick stopping for deeper C-states */
		if (selected_state->exit_latency >= DEEP_STATE_LATENCY_US) {
			/* Deep C-state - very aggressive */
			tick_stop_factor = 105; /* 1.05x - almost always stop tick */
		} else if (selected_state->exit_latency >= 18) {
			/* Intermediate C-state - aggressive */
			tick_stop_factor = 110; /* 1.10x */
		} else {
			/* Shallow C-state - still stop tick to encourage promotion */
			tick_stop_factor = 115; /* 1.15x */
		}

		*stop_tick = (idle_duration_us * 100) > (selected_state->target_residency * tick_stop_factor) &&
			     (idle_duration_us > MIN_TICK_STOP_DURATION_US) &&
			     (has_irq_prediction || mobile_dev->idle_ema_avg > 0);

		/* Always stop tick for C2 and deeper to maintain state */
		if (index >= 2)
			*stop_tick = true;
	} else {
		*stop_tick = false;
	}

	return index;
}

static struct cpuidle_governor mobile_governor = {
	.name		= "mobile",
	.rating		= MOBILE_RATING,
	.select		= mobile_select,
	.reflect	= mobile_reflect,
};

static int __init mobile_governor_init(void)
{
	int cpu;

	/* Initialize per-CPU data with sane defaults */
	for_each_possible_cpu(cpu) {
		struct mobile_device *mobile_dev = &per_cpu(mobile_devices, cpu);

		memset(mobile_dev, 0, sizeof(*mobile_dev));
		mobile_dev->ema_alpha_shift = EMA_ALPHA_SHIFT_DEFAULT;
		mobile_dev->exit_reason = 0;
		mobile_dev->irq_available = 1; /* Start with IRQ timings enabled */
		mobile_dev->stats_update_counter = 0;
	}

	/* Enable IRQ timings if available */
	irq_timings_enable();

	return cpuidle_register_governor(&mobile_governor);
}

postcore_initcall(mobile_governor_init);
