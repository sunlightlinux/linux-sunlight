/* drivers/cpufreq/cpufreq_times.c
 *
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/cpufreq.h>
#include <linux/cpufreq_times.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/threads.h>

static DEFINE_RAW_SPINLOCK(task_time_in_state_lock); /* task->time_in_state */

/**
 * struct cpu_freqs - per-cpu frequency information
 * @offset: start of these freqs' stats in task time_in_state array
 * @max_state: number of entries in freq_table
 * @last_index: index in freq_table of last frequency switched to
 * @freq_table: list of available frequencies
 */
struct cpu_freqs {
	unsigned int offset;
	unsigned int max_state;
	unsigned int last_index;
	unsigned int freq_table[];
};

static struct cpu_freqs *all_freqs[NR_CPUS];

static unsigned int next_offset;

void cpufreq_task_times_init(struct task_struct *p)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&task_time_in_state_lock, flags);
	p->time_in_state = NULL;
	raw_spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	p->max_state = 0;
}

void cpufreq_task_times_alloc(struct task_struct *p)
{
	void *temp;
	unsigned long flags;
	unsigned int max_state = READ_ONCE(next_offset);

	/* We use one array to avoid multiple allocs per task */
	temp = kcalloc(max_state, sizeof(p->time_in_state[0]), GFP_ATOMIC);
	if (!temp)
		return;

	raw_spin_lock_irqsave(&task_time_in_state_lock, flags);
	p->time_in_state = temp;
	p->max_state = max_state;
	raw_spin_unlock_irqrestore(&task_time_in_state_lock, flags);
}

/*
 * Grow the time_in_state arrays of tasks which were forked before the
 * latest policy was created. This must not happen from the tick, which
 * runs in hard interrupt context and cannot allocate on PREEMPT_RT.
 */
static void cpufreq_task_times_grow(unsigned int max_state)
{
	struct task_struct *g, *p;
	unsigned long flags;
	u64 *temp;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		if (!READ_ONCE(p->time_in_state) || READ_ONCE(p->max_state) >= max_state)
			continue;

		temp = kcalloc(max_state, sizeof(*temp), GFP_ATOMIC);
		if (!temp)
			break;

		raw_spin_lock_irqsave(&task_time_in_state_lock, flags);
		if (p->time_in_state && p->max_state < max_state) {
			memcpy(temp, p->time_in_state, p->max_state * sizeof(*temp));
			swap(temp, p->time_in_state);
			p->max_state = max_state;
		}
		raw_spin_unlock_irqrestore(&task_time_in_state_lock, flags);
		kfree(temp);
	}
	rcu_read_unlock();
}

void cpufreq_task_times_exit(struct task_struct *p)
{
	unsigned long flags;
	void *temp;

	if (!p->time_in_state)
		return;

	raw_spin_lock_irqsave(&task_time_in_state_lock, flags);
	temp = p->time_in_state;
	p->time_in_state = NULL;
	raw_spin_unlock_irqrestore(&task_time_in_state_lock, flags);
	kfree(temp);
}

int proc_time_in_state_show(struct seq_file *m, struct pid_namespace *ns,
	struct pid *pid, struct task_struct *p)
{
	unsigned int cpu, i, max_state = READ_ONCE(next_offset);
	u64 cputime, *times;
	unsigned long flags;
	struct cpu_freqs *freqs;
	struct cpu_freqs *last_freqs = NULL;

	times = kcalloc(max_state, sizeof(*times), GFP_KERNEL);
	if (max_state && !times)
		return -ENOMEM;

	/* Don't format output with the raw lock held and interrupts off */
	raw_spin_lock_irqsave(&task_time_in_state_lock, flags);
	if (p->time_in_state)
		memcpy(times, p->time_in_state,
		       min(max_state, p->max_state) * sizeof(*times));
	raw_spin_unlock_irqrestore(&task_time_in_state_lock, flags);

	for_each_possible_cpu(cpu) {
		freqs = all_freqs[cpu];
		if (!freqs || freqs == last_freqs)
			continue;
		last_freqs = freqs;

		seq_printf(m, "cpu%u\n", cpu);
		for (i = 0; i < freqs->max_state; i++) {
			cputime = 0;
			if (freqs->offset + i < max_state)
				cputime = times[freqs->offset + i];
			seq_printf(m, "%u %lu\n", freqs->freq_table[i],
				   (unsigned long)nsec_to_clock_t(cputime));
		}
	}
	kfree(times);
	return 0;
}

void cpufreq_acct_update_power(struct task_struct *p, u64 cputime)
{
	unsigned long flags;
	unsigned int state;
	struct cpu_freqs *freqs = all_freqs[task_cpu(p)];

	if (!freqs || is_idle_task(p) || p->flags & PF_EXITING)
		return;

	state = freqs->offset + READ_ONCE(freqs->last_index);

	raw_spin_lock_irqsave(&task_time_in_state_lock, flags);
	if (state < p->max_state && p->time_in_state)
		p->time_in_state[state] += cputime;
	raw_spin_unlock_irqrestore(&task_time_in_state_lock, flags);
}

static int cpufreq_times_get_index(struct cpu_freqs *freqs, unsigned int freq)
{
	int index;
        for (index = 0; index < freqs->max_state; ++index) {
		if (freqs->freq_table[index] == freq)
			return index;
        }
	return -1;
}

void cpufreq_times_create_policy(struct cpufreq_policy *policy)
{
	int cpu, index = 0;
	unsigned int count = 0;
	struct cpufreq_frequency_table *pos, *table;
	struct cpu_freqs *freqs;
	void *tmp;

	if (all_freqs[policy->cpu])
		return;

	table = policy->freq_table;
	if (!table)
		return;

	cpufreq_for_each_valid_entry(pos, table)
		count++;

	tmp = kzalloc(struct_size(freqs, freq_table, count), GFP_KERNEL);
	if (!tmp)
		return;

	freqs = tmp;
	freqs->max_state = count;

	cpufreq_for_each_valid_entry(pos, table)
		freqs->freq_table[index++] = pos->frequency;

	index = cpufreq_times_get_index(freqs, policy->cur);
	if (index >= 0)
		WRITE_ONCE(freqs->last_index, index);

	freqs->offset = next_offset;
	WRITE_ONCE(next_offset, freqs->offset + count);
	for_each_cpu(cpu, policy->related_cpus)
		all_freqs[cpu] = freqs;

	cpufreq_task_times_grow(freqs->offset + count);
}

void cpufreq_times_record_transition(struct cpufreq_policy *policy,
	unsigned int new_freq)
{
	int index;
	struct cpu_freqs *freqs = all_freqs[policy->cpu];
	if (!freqs)
		return;

	index = cpufreq_times_get_index(freqs, new_freq);
	if (index >= 0)
		WRITE_ONCE(freqs->last_index, index);
}
