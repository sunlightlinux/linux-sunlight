/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_PRIO_H
#define _LINUX_SCHED_PRIO_H

#define MAX_NICE	19
#define MIN_NICE	-20
#define NICE_WIDTH	(MAX_NICE - MIN_NICE + 1)

/*
 * Priority of a process goes from 0..MAX_PRIO-1, valid RT
 * priority is 0..MAX_RT_PRIO-1, and SCHED_NORMAL/SCHED_BATCH
 * tasks are in the range MAX_RT_PRIO..MAX_PRIO-1. Priority
 * values are inverted: lower p->prio value means higher priority.
 */

#define MAX_RT_PRIO		100

#define MAX_PRIO		(MAX_RT_PRIO + NICE_WIDTH)
#define DEFAULT_PRIO		(MAX_RT_PRIO + NICE_WIDTH / 2)

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static priority [ MAX_RT_PRIO..MAX_PRIO-1 ],
 * and back.
 */
#define NICE_TO_PRIO(nice)	((nice) + DEFAULT_PRIO)
#define PRIO_TO_NICE(prio)	((prio) - DEFAULT_PRIO)

/*
 * Convert nice value [19,-20] to rlimit style value [1,40].
 */
static inline long nice_to_rlimit(long nice)
{
	return (MAX_NICE - nice + 1);
}

/*
 * Convert rlimit style value [1,40] to nice value [-20, 19].
 */
static inline long rlimit_to_nice(long prio)
{
	return (MAX_NICE - prio + 1);
}

/*
 * Latency nice is meant to provide scheduler hints about the relative
 * latency requirements of a task with respect to other tasks.
 * Thus a task with latency_nice == 19 can be hinted as the task with no
 * latency requirements, in contrast to the task with latency_nice == -20
 * which should be given priority in terms of lower latency.
 */
#define MAX_LATENCY_NICE	19
#define MIN_LATENCY_NICE	-20

#define LATENCY_NICE_WIDTH	\
	(MAX_LATENCY_NICE - MIN_LATENCY_NICE + 1)

/*
 * Default tasks should be treated as a task with latency_nice = 0.
 */
#define DEFAULT_LATENCY_NICE	0
#define DEFAULT_LATENCY_PRIO	(DEFAULT_LATENCY_NICE + LATENCY_NICE_WIDTH/2)

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static latency [ 0..39 ],
 * and back.
 */
#define NICE_TO_LATENCY(nice)	((nice) + DEFAULT_LATENCY_PRIO)
#define LATENCY_TO_NICE(prio)	((prio) - DEFAULT_LATENCY_PRIO)

#endif /* _LINUX_SCHED_PRIO_H */
