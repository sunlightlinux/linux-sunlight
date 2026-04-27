/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cpuidle_adaptive

#if !defined(_TRACE_CPUIDLE_ADAPTIVE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CPUIDLE_ADAPTIVE_H

#include <linux/tracepoint.h>

/*
 * cpuidle_adaptive_select - emitted at the end of adaptive_select() with the
 * full decision context: which state was picked, the predicted idle window
 * (after deep-state / battery boosts), the workload multiplier, and the
 * factor breakdown that produced it. Use this to debug "why did we land in
 * C1 instead of C6" in production.
 */
TRACE_EVENT(cpuidle_adaptive_select,

	TP_PROTO(unsigned int cpu, int idx, u64 predicted_ns, s64 latency_req,
		 u32 multiplier, u16 load_factor, u16 io_factor,
		 u8 net_factor, u8 battery_boost, bool sibling_busy),

	TP_ARGS(cpu, idx, predicted_ns, latency_req, multiplier,
		load_factor, io_factor, net_factor, battery_boost, sibling_busy),

	TP_STRUCT__entry(
		__field(unsigned int,	cpu)
		__field(int,		idx)
		__field(u64,		predicted_ns)
		__field(s64,		latency_req)
		__field(u32,		multiplier)
		__field(u16,		load_factor)
		__field(u16,		io_factor)
		__field(u8,		net_factor)
		__field(u8,		battery_boost)
		__field(bool,		sibling_busy)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->idx = idx;
		__entry->predicted_ns = predicted_ns;
		__entry->latency_req = latency_req;
		__entry->multiplier = multiplier;
		__entry->load_factor = load_factor;
		__entry->io_factor = io_factor;
		__entry->net_factor = net_factor;
		__entry->battery_boost = battery_boost;
		__entry->sibling_busy = sibling_busy;
	),

	TP_printk("cpu=%u idx=%d predicted=%llu lat_req=%lld mult=%u "
		  "load=%u io=%u net=%u batt_boost=%u sib_busy=%d",
		  __entry->cpu, __entry->idx,
		  (unsigned long long)__entry->predicted_ns,
		  (long long)__entry->latency_req,
		  __entry->multiplier,
		  (unsigned int)__entry->load_factor,
		  (unsigned int)__entry->io_factor,
		  (unsigned int)__entry->net_factor,
		  (unsigned int)__entry->battery_boost,
		  __entry->sibling_busy)
);

/*
 * cpuidle_adaptive_update - emitted from adaptive_update() once the actual
 * residency is known. Lets you measure prediction accuracy without scraping
 * sysfs counters: predicted_us is what the governor told the cpuidle core
 * to expect, measured_us is what we actually slept, and accuracy_pct is the
 * running hit rate against the ±25% window used to retune EMA alpha.
 */
TRACE_EVENT(cpuidle_adaptive_update,

	TP_PROTO(unsigned int cpu, int last_idx, u64 measured_us,
		 u64 predicted_us, u32 accuracy_pct, u32 multiplier),

	TP_ARGS(cpu, last_idx, measured_us, predicted_us, accuracy_pct,
		multiplier),

	TP_STRUCT__entry(
		__field(unsigned int,	cpu)
		__field(int,		last_idx)
		__field(u64,		measured_us)
		__field(u64,		predicted_us)
		__field(u32,		accuracy_pct)
		__field(u32,		multiplier)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->last_idx = last_idx;
		__entry->measured_us = measured_us;
		__entry->predicted_us = predicted_us;
		__entry->accuracy_pct = accuracy_pct;
		__entry->multiplier = multiplier;
	),

	TP_printk("cpu=%u last_idx=%d measured_us=%llu predicted_us=%llu "
		  "accuracy=%u%% mult=%u",
		  __entry->cpu, __entry->last_idx,
		  (unsigned long long)__entry->measured_us,
		  (unsigned long long)__entry->predicted_us,
		  __entry->accuracy_pct, __entry->multiplier)
);

#endif /* _TRACE_CPUIDLE_ADAPTIVE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
