/*
 * Copyright (c) 2018 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <kernel.h>
#include <timeout_q.h>
#include <init.h>
#include <string.h>
#include <pm/pm.h>
#include <pm/state.h>
#include <pm/policy.h>
#include <tracing/tracing.h>

#include "pm_priv.h"

#define PM_STATES_LEN (1 + PM_STATE_SOFT_OFF - PM_STATE_ACTIVE)
#include <logging/log.h>
LOG_MODULE_REGISTER(pm, CONFIG_PM_LOG_LEVEL);

static int post_ops_done = 1;
static struct pm_state_info z_power_state;
static sys_slist_t pm_notifiers = SYS_SLIST_STATIC_INIT(&pm_notifiers);
static struct k_spinlock pm_notifier_lock;

#ifdef CONFIG_PM_STATS

#include <stats/stats.h>
#include <sys/util_macro.h>


struct pm_cpu_timing {
	uint32_t timer_start;
	uint32_t timer_end;
};

static struct pm_cpu_timing pm_cpu_timings[CONFIG_MP_NUM_CPUS];

static inline void pm_start_timer(void)
{
	pm_cpu_timings[_current_cpu->id].timer_start = k_cycle_get_32();
}

static inline void pm_stop_timer(void)
{
	pm_cpu_timings[_current_cpu->id].timer_end = k_cycle_get_32();
}

STATS_SECT_START(pm_cpu_stats)
STATS_SECT_ENTRY32(state_count)
STATS_SECT_ENTRY32(state_last_cycles)
STATS_SECT_ENTRY32(state_total_cycles)
STATS_SECT_END;

STATS_NAME_START(pm_cpu_stats)
STATS_NAME(pm_cpu_stats, state_count)
STATS_NAME(pm_cpu_stats, state_last_cycles)
STATS_NAME(pm_cpu_stats, state_total_cycles)
STATS_NAME_END(pm_cpu_stats);

#define PM_STAT_NAME_LEN sizeof("pm_cpu_XXX_state_X_stats")
static char pm_cpu_stat_names[CONFIG_MP_NUM_CPUS][PM_STATES_LEN][PM_STAT_NAME_LEN];
static struct stats_pm_cpu_stats pm_cpu_stats[CONFIG_MP_NUM_CPUS][PM_STATES_LEN];

static int pm_stats_init(const struct device *unused)
{
	for (int i = 0; i < CONFIG_MP_NUM_CPUS; i++) {
		for (int j = 0; j < PM_STATES_LEN; j++) {
			snprintk(pm_cpu_stat_names[i][j], PM_STAT_NAME_LEN,
				 "pm_cpu_%03d_state_%1d_stats", i, j);
			stats_init(&(pm_cpu_stats[i][j].s_hdr), STATS_SIZE_32, 3,
				   STATS_NAME_INIT_PARMS(pm_cpu_stats));
			stats_register(pm_cpu_stat_names[i][j], &(pm_cpu_stats[i][j].s_hdr));
		}
	}
	return 0;
}

SYS_INIT(pm_stats_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static void pm_stats_update(enum pm_state state)
{
	uint8_t cpu = _current_cpu->id;
	uint32_t time_total =
		pm_cpu_timings[cpu].timer_end -
		pm_cpu_timings[cpu].timer_start;

	STATS_INC(pm_cpu_stats[cpu][state], state_count);
	STATS_INCN(pm_cpu_stats[cpu][state], state_total_cycles, time_total);
	STATS_SET(pm_cpu_stats[cpu][state], state_last_cycles, time_total);
}
#else
static inline void pm_start_timer(void) {}
static inline void pm_stop_timer(void) {}


static void pm_stats_update(enum pm_state state) {}
#endif

static inline void exit_pos_ops(struct pm_state_info info)
{
	extern __weak void
		pm_power_state_exit_post_ops(struct pm_state_info info);

	if (pm_power_state_exit_post_ops != NULL) {
		pm_power_state_exit_post_ops(info);
	} else {
		/*
		 * This function is supposed to be overridden to do SoC or
		 * architecture specific post ops after sleep state exits.
		 *
		 * The kernel expects that irqs are unlocked after this.
		 */

		irq_unlock(0);
	}
}

static inline void pm_state_set(struct pm_state_info info)
{
	extern __weak void
		pm_power_state_set(struct pm_state_info info);

	if (pm_power_state_set != NULL) {
		pm_power_state_set(info);
	}
}

/*
 * Function called to notify when the system is entering / exiting a
 * power state
 */
static inline void pm_state_notify(bool entering_state)
{
	struct pm_notifier *notifier;
	k_spinlock_key_t pm_notifier_key;
	void (*callback)(enum pm_state state);

	pm_notifier_key = k_spin_lock(&pm_notifier_lock);
	SYS_SLIST_FOR_EACH_CONTAINER(&pm_notifiers, notifier, _node) {
		if (entering_state) {
			callback = notifier->state_entry;
		} else {
			callback = notifier->state_exit;
		}

		if (callback) {
			callback(z_power_state.state);
		}
	}
	k_spin_unlock(&pm_notifier_lock, pm_notifier_key);
}

void pm_system_resume(void)
{
	/*
	 * This notification is called from the ISR of the event
	 * that caused exit from kernel idling after PM operations.
	 *
	 * Some CPU low power states require enabling of interrupts
	 * atomically when entering those states. The wake up from
	 * such a state first executes code in the ISR of the interrupt
	 * that caused the wake. This hook will be called from the ISR.
	 * For such CPU LPS states, do post operations and restores here.
	 * The kernel scheduler will get control after the ISR finishes
	 * and it may schedule another thread.
	 *
	 * Call pm_idle_exit_notification_disable() if this
	 * notification is not required.
	 */
	if (!post_ops_done) {
		post_ops_done = 1;
		exit_pos_ops(z_power_state);
		pm_state_notify(false);
	}
}

void pm_power_state_force(struct pm_state_info info)
{
	__ASSERT(info.state < PM_STATES_LEN,
		 "Invalid power state %d!", info.state);

	if (info.state == PM_STATE_ACTIVE) {
		return;
	}

	(void)arch_irq_lock();
	z_power_state = info;
	post_ops_done = 0;
	pm_state_notify(true);

	k_sched_lock();
	pm_start_timer();
	/* Enter power state */
	pm_state_set(z_power_state);
	pm_stop_timer();

	pm_system_resume();
	k_sched_unlock();
}

#if CONFIG_PM_DEVICE
static enum pm_state _handle_device_abort(struct pm_state_info info)
{
	LOG_DBG("Some devices didn't enter suspend state!");
	pm_resume_devices();

	z_power_state.state = PM_STATE_ACTIVE;
	return PM_STATE_ACTIVE;
}
#endif

enum pm_state pm_system_suspend(int32_t ticks)
{
	SYS_PORT_TRACING_FUNC_ENTER(pm, system_suspend, ticks);
	z_power_state = pm_policy_next_state(ticks);
	if (z_power_state.state == PM_STATE_ACTIVE) {
		LOG_DBG("No PM operations done.");
		SYS_PORT_TRACING_FUNC_EXIT(pm, system_suspend, ticks, z_power_state.state);
		return z_power_state.state;
	}
	post_ops_done = 0;

	if (ticks != K_TICKS_FOREVER) {
		/*
		 * Just a sanity check in case the policy manager does not
		 * handle this error condition properly.
		 */
		__ASSERT(z_power_state.min_residency_us >=
			z_power_state.exit_latency_us,
			"min_residency_us < exit_latency_us");

		/*
		 * We need to set the timer to interrupt a little bit early to
		 * accommodate the time required by the CPU to fully wake up.
		 */
		z_set_timeout_expiry(ticks -
		     k_us_to_ticks_ceil32(z_power_state.exit_latency_us), true);
	}

#if CONFIG_PM_DEVICE
	bool should_resume_devices = false;

	if (z_power_state.state != PM_STATE_RUNTIME_IDLE) {
		if (pm_suspend_devices()) {
			SYS_PORT_TRACING_FUNC_EXIT(pm, system_suspend,
				ticks, _handle_device_abort(z_power_state));
			return _handle_device_abort(z_power_state);
		}
		should_resume_devices = true;
	}
#endif
	/*
	 * This function runs with interruptions locked but it is
	 * expected the SoC to unlock them in
	 * pm_power_state_exit_post_ops() when returning to active
	 * state. We don't want to be scheduled out yet, first we need
	 * to send a notification about leaving the idle state. So,
	 * we lock the scheduler here and unlock just after we have
	 * sent the notification in pm_system_resume().
	 */
	k_sched_lock();
	pm_start_timer();
	/* Enter power state */
	pm_state_notify(true);
	pm_state_set(z_power_state);
	pm_stop_timer();

	/* Wake up sequence starts here */
#if CONFIG_PM_DEVICE
	if (should_resume_devices) {
		/* Turn on peripherals and restore device states as necessary */
		pm_resume_devices();
	}
#endif
	pm_stats_update(z_power_state.state);
	pm_system_resume();
	k_sched_unlock();
	SYS_PORT_TRACING_FUNC_EXIT(pm, system_suspend, ticks, z_power_state.state);
	return z_power_state.state;
}

void pm_notifier_register(struct pm_notifier *notifier)
{
	k_spinlock_key_t pm_notifier_key = k_spin_lock(&pm_notifier_lock);

	sys_slist_append(&pm_notifiers, &notifier->_node);
	k_spin_unlock(&pm_notifier_lock, pm_notifier_key);
}

int pm_notifier_unregister(struct pm_notifier *notifier)
{
	int ret = -EINVAL;
	k_spinlock_key_t pm_notifier_key;

	pm_notifier_key = k_spin_lock(&pm_notifier_lock);
	if (sys_slist_find_and_remove(&pm_notifiers, &(notifier->_node))) {
		ret = 0;
	}
	k_spin_unlock(&pm_notifier_lock, pm_notifier_key);

	return ret;
}

const struct pm_state_info pm_power_state_next_get(void)
{
	return z_power_state;
}
