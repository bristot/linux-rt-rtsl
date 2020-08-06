// SPDX-License-Identifier: GPL-2.0
/*
 * rtsl: A theoretically sound bound for the scheduling latency.
 *
 * This tool was presented as part of the following paper:
 *
 * de Oliveira, D. B., Casini, D., de Oliveira, R. S., Cucinotta, T.
 * "Demystifying the Real-Time Linux Scheduling Latency". 2020, In
 * 32nd Euromicro Conference on Real-time Systems (ECRTS 2020).
 *
 * This is the "latency parser," presented in the paper.
 *
 * The paper presents the theoretical explanation of the tracepoints added
 * by this module.
 *
 * Copyright (C) 2019-2020: Daniel Bristot de Oliveira <bristot@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/tracepoint.h>
#include <trace/events/preemptirq.h>

#define CREATE_TRACE_POINTS
#include <trace/events/rtsl.h>

#define MODULE_NAME "rtsl"

#ifdef RTSL_DEBUG
#define debug trace_printk
#define warn trace_printk
#define error trace_printk
#define stack() trace_dump_stack(1)
#else
#define debug(...) do {} while (0)
#define error(...) do {} while (0)
#define warn(...) do {} while (0)
#define stack()	do {} while (0)
#endif

/*
 * Preemption or IRQ disabled.
 */
struct poid {
	bool pd;
	bool id;
	u64 delta_start;
	u64 max;
};

/*
 * Preemption and IRQ enabled, ready to schedule.
 */
struct paie {
	u64 delta_start;
	u64 max;
};

/*
 * preemption disabled to schedule.
 */
struct psd {
	u64 delta_start;
	u64 max;
};

/*
 * the sched tail delay, that starts after the IRQs get disabled
 * before causing a context switch, and ends when the preemption
 * gets enabled in the return from the scheduler.
 */
struct dst {
        pid_t pid;
	u64 delta_start;
	u64 max;
};

/*
 * IRQ definitions.
 */
struct irq {
	u64 arrival_time;
	u64 delta_start;
	int was_psd;
	int vector;
};

/*
 * NMI definitions.
 */
struct nmi {
	u64 delta_start;
};

/*
 * The variables of a given CPU.
 */
struct rtsl_variables {
	local_t int_counter;
	struct poid poid;
	struct paie paie;
	struct psd psd;
	struct dst dst;
	struct irq irq;
	struct nmi nmi;
	bool running;

};

DEFINE_PER_CPU(struct rtsl_variables, per_cpu_rtsl_var);

#define get_clock()	trace_clock_local()
#define are_poid(poid)	poid->delta_start
#define is_pd(poid)	poid->pd
#define is_id(poid)	poid->id
#define is_paie(paie)	paie->delta_start
#define is_psd(psd)	psd->delta_start
#define is_irq(irq)	irq->delta_start
#define is_dst(dst)	dst->delta_start

static inline struct rtsl_variables *this_cpu_rtsl_var(void)
{
	return this_cpu_ptr(&per_cpu_rtsl_var);
}

static inline void rtsl_var_reset(struct rtsl_variables *rtsl_var)
{
	/*
	 * So far, all the values are initialized as 0, so
	 * zeroing the structure is perfect :-).
	 */
	memset(rtsl_var, 0, sizeof(struct rtsl_variables));
}

static inline void rtsl_var_init_all(void)
{
	struct rtsl_variables *rtsl_var;
	int cpu;
	for_each_cpu(cpu, cpu_online_mask) {
		rtsl_var = per_cpu_ptr(&per_cpu_rtsl_var, cpu);
		rtsl_var_reset(rtsl_var);
	}
}

static inline void rtsl_var_destroy_all(void)
{
	struct rtsl_variables *rtsl_var;
	int cpu;
	for_each_cpu(cpu, cpu_online_mask) {
		rtsl_var = per_cpu_ptr(&per_cpu_rtsl_var, cpu);
		rtsl_var_reset(rtsl_var);
	}
}

static inline void rtsl_stop_all(void)
{
	struct rtsl_variables *rtsl_var;
	int cpu;
	for_each_cpu(cpu, cpu_online_mask) {
		rtsl_var = per_cpu_ptr(&per_cpu_rtsl_var, cpu);
		rtsl_var->running = false;
	}
}

/*
 * rtsl_start: start the monitoring of a CPU.
 *
 * It is called after reaching the initial condition, after
 * enabling rtsl.
 */
static inline void rtsl_start(struct rtsl_variables *rtsl_var)
{
	rtsl_var->running = true;
}

/*
 * rtsl_stop: stop the monitoring of a CPU
 */
static inline void rtsl_stop(struct rtsl_variables *rtsl_var)
{
	rtsl_var->running = false;
}

static inline bool rtsl_running(struct rtsl_variables *rtsl_var)
{
	return rtsl_var->running;
}

/*
 * The enable/disable interface control.
 */
static atomic_t global_rtsl_enable;
static bool rtsl_enabled(void)
{
	return atomic_read(&global_rtsl_enable);
}

/*
 * After enabling the interface, each CPU needs to wait for the initial
 * condition to happen before start tracking its own states.
 */
static inline int rtsl_initialized(void)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();

	if (!rtsl_running(rtsl_var)) {
		if (!rtsl_enabled())
			return 0;
		/*
		 * We cannot start if IRQs are disabled. Why? preempt_schedule!
		 */
		if (irqs_disabled())
			return 0;

		rtsl_start(rtsl_var);

		return 1;
	}

	return 1;
}

/*
 * get_int_safe_duration: get the duration of a window.
 *
 * The thread variables (poid, paie and dst) need to have its duration
 * without the interference from interrupts. Instead of keeping a variable
 * to discount the interrupt interference from these variables, the
 * starting time of these variables are pushed forward with the
 * INT duration by interrupts. In this way, a single variable is used
 * to:
 *   - Know if a given window is being measured.
 *   - Account its duration.
 *   - Discount the interference.
 *
 * To avoid getting inconsistent values, e.g.,:
 *
 * 	now = get_clock()
 * 		--->	interrupt!
 * 			delta_start -= int duration;
 * 		<---
 * 	duration = now - delta_start;
 *
 * 	negative duration if the variable duration before the interrupt
 * 	was smaller than the interrupt execution.
 *
 * A counter of interrupts is used. If the counter increased, try
 * to capture the duration again.
 *
 */
static inline s64 get_int_safe_duration(struct rtsl_variables *rtsl_var,
					u64 *delta_start)
{
	u64 int_counter, now;
	s64 duration;

	do {
		int_counter = local_read(&rtsl_var->int_counter);
		/* synchronize with interrupts */
		barrier();

		now = get_clock();
		duration = (now - *delta_start);

		/* synchronize with interrupts */
		barrier();
	} while (int_counter != local_read(&rtsl_var->int_counter));

#ifdef RTSL_DEBUG
	/*
	 * This is an evidence of race conditions that cause
	 * a value to be "discounted" too much.
	 */
	if (duration < 0)
		stack();
#endif
	*delta_start = 0;

	return duration;
}

/*
 * The delta start should also be protected about interrupts touching it.
 *
 * See get_int_safe_duration().
 */
static void set_int_safe_delta_start(struct rtsl_variables *rtsl_var,
				     u64 *delta_start)
{
	u64 int_counter;

	do {
		int_counter = local_read(&rtsl_var->int_counter);
		/* synchronize with interrupts */
		barrier();

		*delta_start = get_clock();

		/* synchronize with interrupts */
		barrier();
	} while (int_counter != local_read(&rtsl_var->int_counter));
}

/*
 * poid_duration: preemption or IRQ disabled by a thread.
 *
 * Compute the preemption or IRQ disabled by a thread.
 */
static void poid_duration(struct rtsl_variables *rtsl_var)
{
	struct poid *poid = &rtsl_var->poid;
	s64 duration;

	/*
	 * Should this be a WARN?
	 */
	if (!are_poid(poid))
		return;

	duration = get_int_safe_duration(rtsl_var, &poid->delta_start);

	/*
	 * Idle is a special case: it runs with preemption disabled
	 * but waiting for the wakeup to arrive. We should add annotations
	 * about preempt_enable and irq_enable before entering in the idle
	 * (e.g., intel_idle) and disable after its return, so the _real_ idle
	 * time does not count in the POID.
	 *
	 * For the paper, I added annotations to the idle=poll driver. But
	 * it will take some effort to do for all drivers. So, for now, let's
	 * ignore the idle POID.
	 *
	 * stop_critical_timings() and start_critical_timings() are our
	 * friends for the fix.
	 */
	if (current->pid == 0)
		return;

	trace_poid(duration);

	if (duration < poid->max)
		return;

	trace_max_poid(duration);
	poid->max = duration;

	return;
}

/*
 * irq_occurrence: account and trace the IRQ, and discount its interference.
 */
static void irq_occurence(struct rtsl_variables *rtsl_var)
{
	struct poid *poid = &rtsl_var->poid;
	struct paie *paie = &rtsl_var->paie;
	struct psd *psd = &rtsl_var->psd;
	struct irq *irq = &rtsl_var->irq;
	struct dst *dst = &rtsl_var->dst;
	s64 duration;

	duration = get_int_safe_duration(rtsl_var, &irq->delta_start);
	trace_irq_execution(irq->vector, irq->arrival_time, duration);

	/*
	 * If preemption was disabled, discount the interference from the
	 * poid value.
	 */
	if (are_poid(poid))
		poid->delta_start += duration;

	/*
	 * If we are in the dst, discount the interference.
	 */
	if (is_dst(dst))
		dst->delta_start += duration;

	/*
	 * Same for paie.
	 */
	if (is_paie(paie))
		paie->delta_start += duration;

	/*
	 * If, at the being of the IRQ, the preemption was disabled to
	 * schedule, discount the IRQ interference.
	 *
	 * - Why not using is_psd?
	 * - If this IRQ caused a need resched, the preemption to schedule
	 *   will be set before the IRQs get re-enabled, to avoid stacking
	 *   scheduler calls due to another IRQ that could arrive in the
	 *   "paie" that it would cause. See preempt_schedule_irq().
	 *
	 * That is why we need to know if the psd was already executing when
	 * the IRQs get masked to run this IRQ.
	 */
	if (irq->was_psd)
		psd->delta_start += duration;

	irq->vector = 0;
	irq->was_psd = 0;
}

/*
 * handle_irq_disable_normal: handle IRQ disabled by a thread.
 */
static void handle_irq_disable_normal(struct rtsl_variables *rtsl_var)
{
	struct poid *poid = &rtsl_var->poid;
	struct psd *psd = &rtsl_var->psd;
	struct dst *dst = &rtsl_var->dst;

	if (is_psd(psd)) {
		/*
		 * If PSD is set, we need to take note of the possible DST
		 * starting here.
		 *
		 * When the preempt_disable to schedule happens, it takes
		 * note of the current pid on dst->pid.
		 *
		 * While the dst->pid is still the current, the DST did not
		 * start, so keep renewing the delta_start until the context
		 * switch changes the current pid. After that, the DST is
		 * taking place, so do not touch the delta start anymore.
		 */
		if (dst->pid == current->pid)
		        set_int_safe_delta_start(rtsl_var, &dst->delta_start);

		/*
		 * We cannot just return here because the poid after the
		 * preemption from an IRQ start with the IRQ disabled
		 * after the schedule, but still before the psd end.
		 *
		 * See preempt_schedule_irq().
		 */
	}


	poid->id = true;

	/*
	 * If it is already on POID, it means that preemption is disabled and
	 * the it should return.
	 */
	if (are_poid(poid))
		return;

	/*
	 * OK, POID is starting....
	 */
	set_int_safe_delta_start(rtsl_var, &poid->delta_start);
}

/*
 * handle_irq_disable_irq: handle IRQ disabled by the entry point of an IRQ.
 */
static void handle_irq_disable_irq(struct rtsl_variables *rtsl_var)
{
	struct psd *psd = &rtsl_var->psd;
	struct irq *irq = &rtsl_var->irq;

	/*
	 * See irq_occurence() for further explanation regarding
	 * irq->was_psd.
	 */
	if (is_psd(psd))
		irq->was_psd = 1;

	/*
	 * This value will be used in the report, but not to compute
	 * the execution time, so it is safe to get it unsafe.
	 */
	irq->arrival_time = get_clock();

	set_int_safe_delta_start(rtsl_var, &irq->delta_start);
}


/*
 * handle_irq_disable: IRQs disabled!
 *
 * IRQs can be disabled for two reasons: to postpone IRQs or to actually
 * protect an IRQ from being preempted by another one.
 *
 * This is the function that hooks to the tracepoint. It does not compute
 * any value, just forward the event to the specific functions.
 */
static void handle_irq_disable(void *nulla, unsigned long ip,
			       unsigned long parent_ip, int irq_entry)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();

	if (!rtsl_running(rtsl_var))
		return;

	if (irq_entry)
		handle_irq_disable_irq(rtsl_var);
	else
		handle_irq_disable_normal(rtsl_var);
}

/*
 * handle_irq_enable_irq: enable IRQs after the end of an IRQ handling.
 *
 * The last action from an interrupt occurrence is getting interrupts
 * enabled to return to the thread context. This is the best time to
 * get the irq duration.
 */
static void handle_irq_enable_irq(struct rtsl_variables *rtsl_var)
{
	if (!rtsl_running(rtsl_var))
		return;

	irq_occurence(rtsl_var);
}

/*
 * handle_irq_enable_normal: handle the IRQ enabled by a thread.
 */
static void handle_irq_enable_normal(struct rtsl_variables *rtsl_var)
{
	struct poid *poid = &rtsl_var->poid;
	struct paie *paie = &rtsl_var->paie;
	struct psd *psd = &rtsl_var->psd;

	if (!rtsl_running(rtsl_var))
		return;

	poid->id = false;

	/*
	 * if preemption is disabled, the POID continues.
	 *
	 * if in PSD, the IRQ enabled does not count.
	 */
	if (is_pd(poid) || is_psd(psd))
		return;

	poid_duration(rtsl_var);

	/*
	 * This is the paie start, if need_resched() is set.
	 */
	if (tif_need_resched_now())
		set_int_safe_delta_start(rtsl_var, &paie->delta_start);
}

/*
 * handle_irq_enable: IRQ enabled!
 *
 * This is the function that hooks to the tracepoint. It does not compute
 * any value, just forward the event to the specific functions.
 */
static void handle_irq_enable(void *nulla, unsigned long ip,
			      unsigned long parent_ip, int irq_exit)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();

	if (!rtsl_running(rtsl_var))
		return;

	if (irq_exit)
		handle_irq_enable_irq(rtsl_var);
	else
		handle_irq_enable_normal(rtsl_var);
}

/*
 * handle_preempt_disable_nosched: preemption disabled.
 *
 * The regular preempt disable, that contributes to the POID.
 */
static void handle_preempt_disable_nosched(void)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct poid *poid = &rtsl_var->poid;
	struct irq *irq = &rtsl_var->irq;

	if (!rtsl_running(rtsl_var))
		return;

	/*
	 * Preemption disabled on IRQ is interference, not poid.
	 */
	if (is_irq(irq))
		return;

	poid->pd = true;

	if (is_id(poid))
		return;

	set_int_safe_delta_start(rtsl_var, &poid->delta_start);

	return;
}

/*
 * handle_preempt_enable_nosched: preemption enabled.
 *
 * The regular preempt enable, that contributes to the POID.
 */
static void handle_preempt_enable_nosched(void)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct poid *poid = &rtsl_var->poid;
	struct paie *paie = &rtsl_var->paie;
	struct irq *irq = &rtsl_var->irq;

	if (!rtsl_running(rtsl_var))
		return;
	/*
	 * Preemption enabled on IRQ is interference, not poid.
	 */
	if (is_irq(irq))
		return;

	poid->pd = false;

	if (is_id(poid))
		return;

	poid_duration(rtsl_var);

	if (tif_need_resched_now())
		set_int_safe_delta_start(rtsl_var, &paie->delta_start);

	return;
}
/*
 * paie_duration: compute the paie duration.
 */
static void paie_duration(struct rtsl_variables *rtsl_var)
{
	struct paie *paie = &rtsl_var->paie;
	s64 duration;

	/*
	 * The need resched took place during the paie.
	 */
	if (!is_paie(paie))
		return;

	duration = get_int_safe_duration(rtsl_var, &paie->delta_start);

	/*
	 * Idle is an special case, do not print.
	 */
	if (current->pid == 0)
		return;

	trace_paie(duration);

	if (paie->max > duration)
		return;

	trace_max_paie(duration);
	paie->max = duration;
	return;

}

/*
 * handle_preempt_disable_sched: first action for scheduling.
 *
 * This is the start of a PSD, and might be the end of the PAIE,
 * if need resched is set.
 *
 * Note: PAIE is only valid for regular need resched, not to the
 * lazy version (which is lazy...).
 */
static void handle_preempt_disable_sched(void)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct poid *poid = &rtsl_var->poid;
	struct paie *paie = &rtsl_var->paie;
	struct psd *psd = &rtsl_var->psd;
	struct dst *dst = &rtsl_var->dst;
	struct irq *irq = &rtsl_var->irq;

	/*
	 * check initial condition
	 */
	if (!rtsl_initialized())
		return;

	/*
	 * Paie is only valid if the scheduler was called with interrupts
	 * also enabled.
	 *
	 * It is not a problem to disable preemption to call the scheduler
	 * with interrupts disabled, see preempt_schedule_irq().
	 *
	 * IRQs will be enabled before calling the __schedule().
	 */
	if (tif_need_resched_now() && !is_irq(irq) && !is_id(poid))
		paie_duration(rtsl_var);

	/*
	 * We are not in paie anymore.
	 */
	paie->delta_start = 0;

	/*
	 * Get the current pid to indentify that the context
	 * switch took place because it changed, and so DST started.
	 */
	dst->pid = current->pid;

	set_int_safe_delta_start(rtsl_var, &psd->delta_start);
}


/*
 * handle_preempt_enable_sched: last action of the scheduler.
 *
 * At this point, the scheduler already run and might return
 * to the thread execution.
 *
 * It is always the end of the PSD and DST. It might be the begin
 * of the PAIE if the need resched was set after the context switch.
 */
static void handle_preempt_enable_sched(void)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct paie *paie = &rtsl_var->paie;
	struct psd *psd = &rtsl_var->psd;
	struct dst *dst = &rtsl_var->dst;
	u64 psd_duration;
	u64 dst_duration;

	if (!rtsl_running(rtsl_var))
		return;

	if (is_dst(dst)) {
		dst_duration = get_int_safe_duration(rtsl_var, &dst->delta_start);
		trace_dst(dst_duration);

		if (dst_duration > dst->max) {
			trace_max_dst(dst_duration);
			dst->max = dst_duration;
		}
	}

	psd_duration = get_int_safe_duration(rtsl_var, &psd->delta_start);

	trace_psd(psd_duration);

	if (psd_duration > psd->max) {
		trace_max_psd(psd_duration);
		psd->max = psd_duration;
	}

	/*
	 * If need resched is set, PAIE starts again!
	 */
	if (tif_need_resched_now())
		set_int_safe_delta_start(rtsl_var, &paie->delta_start);
}

/*
 * handle_preempt_disable: hook to the preempt_disable tracepoint.
 *
 * and decides which kind of preempt disable it is:
 * 	- to avoid the scheduler;
 * 	- to call the scheduler.
 */
static void handle_preempt_disable(void *nulla, unsigned long ip,
				   unsigned long parent_ip,
				   int to_schedule)
{
	if (to_schedule)
		handle_preempt_disable_sched();
	else
		handle_preempt_disable_nosched();
}

/*
 * handle_preempt_disable: hook to the preempt_enable tracepoint.
 *
 * and decides which kind of preempt enable it is:
 * 	- return from the scheduler;
 * 	- return not to schedule.
 */
static void handle_preempt_enable(void *nulla, unsigned long ip,
				  unsigned long parent_ip,
				  int to_schedule)
{
	if (to_schedule)
		handle_preempt_enable_sched();
	else
		handle_preempt_enable_nosched();
}

/*
 * handle_nmi_entry: hook to the nmi entry tracepoint.
 *
 * Get the current time and, that is it.
 */
static void handle_nmi_entry(void *nulla, unsigned long ip,
			     unsigned long parent_ip)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct nmi *nmi = &rtsl_var->nmi;

	if (!rtsl_running(rtsl_var))
		return;

	nmi->delta_start = get_clock();
}

/*
 * handle_nmi_exit: hook to the nmi exit tracepoint.
 *
 * Get the current time, compute the NMI duration and discount it
 * from other time windows. No synchronization is need from NMI
 * viewpoint. It just needs to increment the int counter.
 */
static void handle_nmi_exit(void *nulla, unsigned long ip,
			    unsigned long parent_ip)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct paie *paie = &rtsl_var->paie;
	struct poid *poid = &rtsl_var->poid;
	struct psd *psd = &rtsl_var->psd;
	struct irq *irq = &rtsl_var->irq;
	struct dst *dst = &rtsl_var->dst;
	struct nmi *nmi = &rtsl_var->nmi;
	u64 duration;

	if (!rtsl_running(rtsl_var))
		return;

	duration = get_clock() - nmi->delta_start;

	trace_nmi_execution(nmi->delta_start, duration);

	/*
	 * We forward the "relative"  disable time to discount
	 * the nmi execution time from the IRQ.
	 */
	local_inc(&rtsl_var->int_counter);

	if (is_irq(irq))
		irq->delta_start += duration;

	if (are_poid(poid))
		poid->delta_start += duration;

	if (is_psd(psd))
		psd->delta_start += duration;

	if (is_dst(dst))
		dst->delta_start += duration;

	if (is_paie(paie))
		paie->delta_start += duration;
}

/*
 * handle_irq_entry: identify the IRQ vector.
 *
 * The begin of the interrupt vector is captured by the first action that
 * identifies it: the annotation that IRQs where disabled on the very
 * early interrupt handling path, even before the definition of the
 * IRQ descriptor or identifier.
 *
 * This tracepoint serves only to identify which interrupt vector will
 * handle it.
 *
 * Is the interrupt vector the best identifier? Probably not, I need to
 * think more about which ID to use (and if it it should be a number).
 */
static void handle_irq_vector_entry(void *nulla, int vector)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct irq *irq = &rtsl_var->irq;

	if (!rtsl_running(rtsl_var))
		return;

	irq->vector = vector;

	local_inc(&rtsl_var->int_counter);
}

static void handle_irq_entry(void *nulla, int irq_nr, struct irqaction *action)
{
	struct rtsl_variables *rtsl_var = this_cpu_rtsl_var();
	struct irq *irq = &rtsl_var->irq;

	if (!rtsl_running(rtsl_var))
		return;

	irq->vector = irq_nr;

	local_inc(&rtsl_var->int_counter);
}



/*
 * These are helper functions to hook to tracepoints without
 * refering to their internal structure.
 *
 * They can be removed if the tracer becomes part of the kernel.
 * In that case, the tracefs could be used instead of debugfs.
 */
struct tp_and_name {
	struct tracepoint *tp;
	void *probe;
	char *name;
	int registered;
};

/*
 * This is the callback that compares tracepoint by their names,
 * and get the tracepoint structure.
 *
 * See get_struct_tracepoint().
 */
static void fill_tp_by_name(struct tracepoint *ktp, void *priv)
{
	struct tp_and_name *tp  = priv;

	if (!strcmp(ktp->name, tp->name))
		tp->tp = ktp;
}

/*
 * get_struct_tracepoint: search a tracepoint by its name.
 *
 * Returns the tracepoint structure of given tracepoint name,
 * or NULL.
 */
static struct tracepoint *get_struct_tracepoint(char *name)
{
	struct tp_and_name tp = {
		.name = name,
		.tp = NULL
	};

	for_each_kernel_tracepoint(fill_tp_by_name, &tp);

	return tp.tp;
}

/*
 * register_tracepoints: register a vector of tracepoints.
 *
 * Receives a vector of tp_and_name, search for their given tracepoint
 * structure by the tp name, and register the probe (when possible).
 *
 * It also keeps note of the registered tracepoints, so it can
 * known which ones to disable later.
 *
 */
static int register_tracepoints(struct tp_and_name *tracepoints, int count)
{
	int retval;
	int i;

	for (i = 0; i < count; i++) {
		tracepoints[i].tp = get_struct_tracepoint(tracepoints[i].name);

		if (!tracepoints[i].tp)
			goto out_err;

		tracepoints[i].registered = 1;

		retval = tracepoint_probe_register(tracepoints[i].tp,
						   tracepoints[i].probe, NULL);
		if (retval)
			goto out_err;
	}

	return 0;

out_err:
	for (i = 0; i < count; i++) {
		if (!tracepoints[i].registered)
			continue;

		tracepoint_probe_unregister(tracepoints[i].tp,
					    tracepoints[i].probe, NULL);
	}
	return -EINVAL;
}

/*
 * unregister_tracepoints: unregister tracepoints
 *
 * See register_tracepoints().
 */
static void unregister_tracepoints(struct tp_and_name *tracepoints, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		if (!tracepoints[i].registered)
			continue;

		tracepoint_probe_unregister(tracepoints[i].tp,
					    tracepoints[i].probe, NULL);

		tracepoints[i].registered = 0;
	}

	return;
}

/*
 * The tracepoints to hook at.
 */
#define NR_TP	18
static struct tp_and_name tps[NR_TP] = {
	{
		.probe = handle_nmi_entry,
		.name = "nmi_entry",
		.registered = 0
	},
	{
		.probe = handle_nmi_exit,
		.name = "nmi_exit",
		.registered = 0
	},
	{
		.probe = handle_irq_disable,
		.name = "irq_disable",
		.registered = 0
	},
	{
		.probe = handle_irq_enable,
		.name = "irq_enable",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "local_timer_entry",
		.registered = 0
	},
#ifdef IRQ_VECTOR
	{
		.probe = handle_irq_vector_entry,
		.name = "external_interrupt_entry",
		.registered = 0
	},
#else
	{
		.probe = handle_irq_entry,
		.name = "irq_handler_entry",
		.registered = 0
	},
#endif
	{
		.probe = handle_irq_vector_entry,
		.name = "thermal_apic_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "deferred_error_apic_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "threshold_apic_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "call_function_single_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "call_function_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "reschedule_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "irq_work_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "x86_platform_ipi_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "error_apic_entry",
		.registered = 0
	},
	{
		.probe = handle_irq_vector_entry,
		.name = "spurious_apic_entry",
		.registered = 0
	},
	{
		.probe = handle_preempt_disable,
		.name = "preempt_disable",
		.registered = 0
	},
	{
		.probe = handle_preempt_enable,
		.name = "preempt_enable",
		.registered = 0
	},
};

/*
 * rtsl_enable: the enable interface.
 *
 * It should initiate the variables, hoot the tracepoints and then
 * infor the CPUs they can start to wait for the initial condition.
 */
static int rtsl_enable(void)
{

	rtsl_var_init_all();

	if (register_tracepoints(tps, NR_TP))
		goto out_err;

	atomic_set(&global_rtsl_enable, 1);

	return 0;

out_err:
	return -EINVAL;
}


/*
 * rtsl_disable: the enable interface (to disable).
 *
 * disable the global trace, disable all CPUs, and unhook
 * the tracepoints.
 */
static void rtsl_disable(void)
{
	atomic_set(&global_rtsl_enable, 0);

	rtsl_var_destroy_all();
	unregister_tracepoints(tps, NR_TP);

	return;
}

DEFINE_MUTEX(interface_lock);

static ssize_t rtsl_enable_read_data(struct file *filp, char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	bool enabled;
	char buf[4];

	memset(buf, 0, sizeof(buf));

	mutex_lock(&interface_lock);
	enabled = rtsl_enabled();
	mutex_unlock(&interface_lock);

	sprintf(buf, "%x\n", enabled);

	return simple_read_from_buffer(user_buf, count, ppos,
                                       buf, strlen(buf)+1);
}

static ssize_t rtsl_enable_write_data(struct file *filp,
				     const char __user *user_buf,
				     size_t count, loff_t *ppos)
{
	int retval = count;
	char buf[3];

	if (count < 1 || count > 3)
		return -EINVAL;

	memset(buf, 0, sizeof(buf));

	retval = simple_write_to_buffer(buf, sizeof(buf)-1, ppos, user_buf,
					count);
        if (!retval)
                return -EFAULT;

	mutex_lock(&interface_lock);

	switch (buf[0]) {
	case '1':
		/*
		 * If it is already enabled, reset.
		 */
		if (rtsl_enabled())
			rtsl_disable();
		rtsl_enable();
		break;
	case '0':
		if (rtsl_enabled())
			rtsl_disable();
		break;
	default:
		retval = -EINVAL;
	}

	mutex_unlock(&interface_lock);
	return retval;
}

struct dentry *interface_root_dir;
struct dentry *interface_enable;

static const struct file_operations interface_enable_fops = {
        .open   = simple_open,
        .llseek = no_llseek,
        .write  = rtsl_enable_write_data,
        .read   = rtsl_enable_read_data,
};

/*
 * rtsl_init_interface: Init the interface.
 */
int __init rtsl_init_interface(void)
{
	int retval = 0;

	mutex_lock(&interface_lock);

	interface_root_dir = debugfs_create_dir("rtsl", NULL);
	if (!interface_root_dir)
		return -ENOMEM;

	interface_enable = debugfs_create_file("enable", 0600,
					       interface_root_dir, NULL,
					       &interface_enable_fops);
	if (!interface_enable) {
		debugfs_remove(interface_root_dir);
		retval = -ENOMEM;
	}

	mutex_unlock(&interface_lock);

	return retval;
}

/*
 * rtsl_init_interface: Init the interface.
 */
void rtsl_destroy_interface(void)
{
	mutex_lock(&interface_lock);
	debugfs_remove(interface_enable);
	debugfs_remove(interface_root_dir);
	mutex_unlock(&interface_lock);

	rtsl_disable();
}

module_init(rtsl_init_interface);
module_exit(rtsl_destroy_interface);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Bristot de Oliveira");
MODULE_DESCRIPTION("rtsl: A theoretically sound scheduling latency analysis");
