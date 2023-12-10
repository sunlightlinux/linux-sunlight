// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019, Linaro Ltd
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 * Copyright (C) 2023
 * Author: Ionut Nechita <ionut_n2001@yahoo.com>
 */

#include <linux/cpuidle.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/sched/clock.h>

struct mobile_device {
	u64 idle_ema_avg;
	u64 idle_total;
	unsigned long last_jiffies;
};

#define EMA_ALPHA_VAL		64
#define EMA_ALPHA_SHIFT		7
#define MAX_RESCHED_INTERVAL_MS	118

static DEFINE_PER_CPU(struct mobile_device, mobile_devices);

static int mobile_ema_new(s64 value, s64 ema_old)
{
	if (likely(ema_old))
		return ema_old + (((value - ema_old) * EMA_ALPHA_VAL) >>
				  EMA_ALPHA_SHIFT);
	return value;
}

static void mobile_reflect(struct cpuidle_device *dev, int index)
{
	struct mobile_device *mobile_dev = this_cpu_ptr(&mobile_devices);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	struct cpuidle_state *s = &drv->states[index];
	int residency, last_residency;

	/*
	 * The idle task was not rescheduled since
	 * MAX_RESCHED_INTERVAL_MS, let's consider the duration is
	 * long enough to clear our stats.
	 */
	if (time_after(jiffies, mobile_dev->last_jiffies +
		       msecs_to_jiffies(MAX_RESCHED_INTERVAL_MS)))
		mobile_dev->idle_ema_avg = 0;

	/*
	 * Sum all the residencies in order to compute the total
	 * duration of the idle task.
	 */
	last_residency = (int)(dev->last_residency_ns / 1000000);
	residency = last_residency - s->exit_latency;
	if (residency > 0)
		mobile_dev->idle_total += residency;

	/*
	 * We exited the idle state with the need_resched() flag, the
	 * idle task will be rescheduled, so store the duration the
	 * idle task was scheduled in an exponential moving average and
	 * reset the total of the idle duration.
	 */
	if (need_resched()) {
		mobile_dev->idle_ema_avg = mobile_ema_new(mobile_dev->idle_total,
						      mobile_dev->idle_ema_avg);
		mobile_dev->idle_total = 0;
		mobile_dev->last_jiffies = jiffies;
	}
}

static int mobile_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		       bool *stop_tick)
{
	struct mobile_device *mobile_dev = this_cpu_ptr(&mobile_devices);
	int latency_req = cpuidle_governor_latency_req(dev->cpu);
	int i, index = 0;
	ktime_t delta_next;
	u64 now, irq_length, timer_length;
	u64 idle_duration_us;

	/*
	 * Get the present time as reference for the next steps
	 */
	now = local_clock();

	/*
	 * Get the next interrupt event giving the 'now' as a
	 * reference, if the next event appears to have already
	 * expired then we get the 'now' returned which ends up with a
	 * zero duration.
	 */
	irq_length = irq_timings_next_event(now) - now;

	/*
	 * Get the timer duration before expiration.
	 */
	timer_length = ktime_to_ns(tick_nohz_get_sleep_length(&delta_next));

	/*
	 * Get the smallest duration between the timer and the irq next event.
	 */
	idle_duration_us = min_t(u64, irq_length, timer_length) / NSEC_PER_USEC;

	/*
	 * Get the idle task duration average if the information is
	 * available.
	 */
	if (mobile_dev->idle_ema_avg)
		idle_duration_us = min_t(u64, idle_duration_us,
					 mobile_dev->idle_ema_avg);

	for (i = 0; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];

		if (dev->states_usage[i].disable)
			continue;

		if (s->power_usage <= -1 || s->power_usage > 0)
			continue;

		if (s->exit_latency > latency_req)
			break;

		if (s->exit_latency >= idle_duration_us)
			break;

		if (s->target_residency > (idle_duration_us - s->exit_latency))
			break;

		index = i;
	}

	if (!index)
		*stop_tick = false;

	return index;
}

static struct cpuidle_governor mobile_governor = {
	.name =		"mobile",
	.rating =	21,
	.select =	mobile_select,
	.reflect =	mobile_reflect,
};

static int __init init_governor(void)
{
	irq_timings_enable();
	return cpuidle_register_governor(&mobile_governor);
}

postcore_initcall(init_governor);
