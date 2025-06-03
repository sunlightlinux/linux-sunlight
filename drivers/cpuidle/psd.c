// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2025 Intel Corporation
 *  Author: Colin Ian King <colin.king@intel.com>
 *
 *  Kernel Prevent Sleep Demotion (PSD)
 */
#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/percpu.h>
#include <linux/jiffies.h>
#include <linux/cpuidle_psd.h>

/* jiffies at which the lease times out */
static DEFINE_PER_CPU(unsigned long, psd_timeout);
static int psd_cpu_lat_timeout_jiffies;

/*
 * A note about the use of the current cpu versus preemption.
 *
 * The use of have_prevent_sleep_demotion() is inside local
 * power management code, and are pinned to that cpu already.
 *
 * On the "set" side, interrupt level code is obviously also fully
 * migration-race free.
 *
 * All other cases are exposed to a migration-race.
 *
 * The goal of prevent sleep demotion is statistical rather than
 * deterministic, e.g. on average the CPU that hits event X will go
 * towards Y more often than not, and the impact of being wrong is a
 * bit of extra power potentially for some short durations.
 * Weighted against the costs in performance and complexity of dealing
 * with the race, the race condition is acceptable.
 *
 * The second known race is where interrupt context might set a
 * psd time in the middle of process context setting a different but
 * psd smaller time, with the result that process context will win
 * incorrectly, and the actual psd time will be less than expected,
 * but still non-zero. Here also the cost of dealing with the race
 * is outweight with the limited impact.
 *
 * The use of timings in jiffies is intentional, it is lightweight
 * read and very fast. While it mau seem that using finer resolution
 * timings is preferable, the expense is too high on I/O fast paths
 * when preventing sleep demotions via prevent_sleep_demotion.
 *
 */
int have_prevent_sleep_demotion(void)
{
	if (likely(psd_cpu_lat_timeout_jiffies)) {
		int cpu = raw_smp_processor_id();

		if (time_before(jiffies, per_cpu(psd_timeout, cpu)))
			return 1;

		/* keep the stored time value close to current */
		per_cpu(psd_timeout, cpu) = jiffies;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(have_prevent_sleep_demotion);

void prevent_sleep_demotion(void)
{
	if (likely(psd_cpu_lat_timeout_jiffies)) {
		const unsigned long next_jiffies = jiffies + psd_cpu_lat_timeout_jiffies;
		const int cpu = raw_smp_processor_id();

		/*  need to round up an extra jiffie */
		if (time_before(per_cpu(psd_timeout, cpu), next_jiffies))
			per_cpu(psd_timeout, cpu) = next_jiffies;
	}
}
EXPORT_SYMBOL_GPL(prevent_sleep_demotion);

static int psd_msecs_to_jiffies(const int msec)
{
	int ret = msecs_to_jiffies(msec);

	return msec > 0 && ret == 0 ? 1 : ret;
}

static __init int prevent_sleep_demotion_init(void)
{
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);
	unsigned int cpu;

	if (!dev_root)
		return -1;

	psd_cpu_lat_timeout_jiffies = psd_msecs_to_jiffies(PSD_NVME_DISK_MSEC);

	pr_info("cpuidle-psd: using %d msec (%d jiffies) for idle timing\n",
		PSD_NVME_DISK_MSEC, psd_cpu_lat_timeout_jiffies);

	for_each_possible_cpu(cpu)
		per_cpu(psd_timeout, cpu) = jiffies;

	return 0;
}

late_initcall(prevent_sleep_demotion_init);
