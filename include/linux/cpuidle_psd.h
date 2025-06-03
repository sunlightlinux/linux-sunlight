/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (C) 2025 Intel Corporation
 *  Author: Colin Ian King <colin.king@intel.com>
 *
 *  Kernel prevent sleep demotion infrastructructure
 */
#ifndef _LINUX_CPUIDLE_PSD_H
#define _LINUX_CPUIDLE_PSD_H

/* duration of sleep demotion for PCIe NVME disks in msec */
#define PSD_NVME_DISK_MSEC		(1)

/* API prototypes */
#ifdef CONFIG_CPU_IDLE_PSD

extern void prevent_sleep_demotion(void);
extern int have_prevent_sleep_demotion(void);

#else

static inline void prevent_sleep_demotion(void)
{
}

static inline int have_prevent_sleep_demotion(void)
{
	return 0;
}
#endif

#endif
