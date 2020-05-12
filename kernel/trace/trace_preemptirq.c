// SPDX-License-Identifier: GPL-2.0
/*
 * preemptoff and irqoff tracepoints
 *
 * Copyright (C) Joel Fernandes (Google) <joel@joelfernandes.org>
 */

#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include "trace.h"

#define CREATE_TRACE_POINTS
#include <trace/events/preemptirq.h>

#ifdef CONFIG_TRACE_IRQFLAGS

#define IRQ_OFF_NORMAL		1
#define IRQ_OFF_IRQ_ENTRY 	2

#define is_irq_entry(x) (x == IRQ_OFF_IRQ_ENTRY)

/* Per-cpu variable to prevent redundant calls when IRQs already off */
static DEFINE_PER_CPU(int, tracing_irq_cpu);

void trace_hardirqs_on(void)
{
	int tracing_irq = this_cpu_read(tracing_irq_cpu);
	if (tracing_irq) {
		if (!in_nmi())
			trace_irq_enable_rcuidle(CALLER_ADDR0, CALLER_ADDR1,
						 is_irq_entry(tracing_irq));
		tracer_hardirqs_on(CALLER_ADDR0, CALLER_ADDR1);
		this_cpu_write(tracing_irq_cpu, 0);
	}

	lockdep_hardirqs_on(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_on);
NOKPROBE_SYMBOL(trace_hardirqs_on);

void trace_hardirqs_off(void)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, IRQ_OFF_NORMAL);
		tracer_hardirqs_off(CALLER_ADDR0, CALLER_ADDR1);
		if (!in_nmi())
			trace_irq_disable_rcuidle(CALLER_ADDR0, CALLER_ADDR1, 0);
	}

	lockdep_hardirqs_off(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_off);
NOKPROBE_SYMBOL(trace_hardirqs_off);

__visible void trace_hardirqs_on_caller(unsigned long caller_addr)
{
	int tracing_irq = this_cpu_read(tracing_irq_cpu);
	if (tracing_irq) {
		if (!in_nmi())
			trace_irq_enable_rcuidle(CALLER_ADDR0, caller_addr,
						 is_irq_entry(tracing_irq));
		tracer_hardirqs_on(CALLER_ADDR0, caller_addr);
		this_cpu_write(tracing_irq_cpu, 0);
	}

	lockdep_hardirqs_on(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_on_caller);
NOKPROBE_SYMBOL(trace_hardirqs_on_caller);

__visible void trace_hardirqs_off_caller(unsigned long caller_addr)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, IRQ_OFF_NORMAL);
		tracer_hardirqs_off(CALLER_ADDR0, caller_addr);
		if (!in_nmi())
			trace_irq_disable_rcuidle(CALLER_ADDR0, caller_addr, 0);
	}

	lockdep_hardirqs_off(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_off_caller);
NOKPROBE_SYMBOL(trace_hardirqs_off_caller);

__visible void trace_hardirqs_off_caller_irq_entry(unsigned long caller_addr)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, IRQ_OFF_IRQ_ENTRY);
		tracer_hardirqs_off(CALLER_ADDR0, caller_addr);
		if (!in_nmi())
			trace_irq_disable_rcuidle(CALLER_ADDR0, caller_addr, 1);
	}

	lockdep_hardirqs_off(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_off_caller_irq_entry);
NOKPROBE_SYMBOL(trace_hardirqs_off_caller_irq_entry);
#endif /* CONFIG_TRACE_IRQFLAGS */

#ifdef CONFIG_TRACE_PREEMPT_TOGGLE

void trace_preempt_on(unsigned long a0, unsigned long a1, int to_sched)
{
	if (!in_nmi())
		trace_preempt_enable_rcuidle(a0, a1, to_sched);
	tracer_preempt_on(a0, a1);
}

void trace_preempt_off(unsigned long a0, unsigned long a1, int to_sched)
{
	if (!in_nmi())
		trace_preempt_disable_rcuidle(a0, a1, to_sched);
	tracer_preempt_off(a0, a1);
}

void trace_preempt_switch_to_sched(unsigned long a0, unsigned long a1)
{
	/*
	 * We are not actually changing the preempt counter, just changing
	 * the context in which the preemption was disabled.
	 *
	 * here: from the preempt_disable() -> preempt_disable_sched().
	 */
	trace_preempt_on(a0, a1, 0);
	trace_preempt_off(a0, a1, 1);
}

void trace_preempt_switch_not_sched(unsigned long a0, unsigned long a1)
{
	/*
	 * We are not actually changing the preempt counter, just changing
	 * the context in which the preemption was disabled.
	 *
	 * here: from the preempt_disable_sched() -> preempt_disable().
	 */
	trace_preempt_on(a0, a1, 1);
	trace_preempt_off(a0, a1, 0);
}
#endif
