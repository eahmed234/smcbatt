// SPDX-License-Identifier: GPL-2.0-only
/*
 * smcbatt - correct battery and AC telemetry for the Intel-Mac macsmc port.
 *
 * Linux 7.3 replaced applesmc with macsmc, and on pre-T2 Macs that also means
 * the ACPI SBS battery is gone: macsmc-power registers macsmc-battery and
 * macsmc-ac instead. That driver assumes the SMC provides B0AP
 * (instantaneous battery power), which a MacBookPro11,2 does not. Two things
 * follow, and both are visible in every battery widget:
 *
 *   - POWER_NOW fails with -EOPNOTSUPP on every read, so the power_supply core
 *     logs "driver failed to report `power_now' property: -95" for each poll,
 *     forever;
 *   - without power_now, upower differentiates ENERGY_NOW instead. The driver
 *     reports ENERGY_NOW as charge_now * voltage_now, and pack voltage sags
 *     under load, so that derivative swings by tens of watts: a nearly full
 *     pack is shown with ~45 minutes of runtime and a ~100 W draw.
 *
 * Both are computable from values the driver already exposes: P = V * I, and
 * energy from charge at the pack's design voltage instead of the live one.
 * P is reported with the sign the power_supply class defines: positive while
 * the supply discharges. That means the battery's P is negated (current_now is
 * positive while charging), while macsmc-ac reports the power the adapter
 * delivers, which must stay positive.
 *
 * The driver also caches the adapter and battery state, and a resume does not
 * refresh it: unplug the charger during sleep and macsmc-ac still answers
 * online=1, macsmc-battery still answers "Not charging", while current_now
 * shows a discharge and charge_now falls. upower's OnBattery and TLP's profile
 * switching both read those cached values, so the desktop stays on its AC
 * profile (performance) on battery after every wake. Both properties are
 * therefore derived from current_now, which the SMC keeps current, and a
 * PM_POST_SUSPEND notifier emits power_supply_changed so consumers re-read.
 *
 * macsmc-power's property handlers are static, so rather than patch or replace
 * the driver this module swaps in a copy of its power_supply_desc whose
 * get_property() delegates to the original and only answers POWER_NOW,
 * ENERGY_NOW/ENERGY_FULL, ONLINE and STATUS itself. Nothing in the macsmc
 * stack is modified, and unloading the module puts the original handlers back.
 *
 * Properties the driver never registers (energy-full-design, time-to-empty)
 * cannot be added this way; they need a change in macsmc-power.
 */

#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

/* µV * µA = pW, /1e6 -> µW. µAh * µV = pWh, /1e6 -> µWh. */
#define SMCBATT_POWER_SCALE	1000000
#define SMCBATT_ENERGY_SCALE	1000000

/* µA: below this the pack is neither charging nor discharging. */
#define SMCBATT_CURRENT_DEADBAND	100000

struct smcbatt_supply {
	const char *name;
	/* Battery: power_now is positive while discharging. */
	bool discharge_positive;
	/* ONLINE/STATUS are derived from the pack current (stale on resume). */
	bool ac;
	bool battery;
	const struct power_supply_desc *orig;
	struct power_supply_desc desc;
};

static struct smcbatt_supply smcbatt_supplies[] = {
	{ .name = "macsmc-battery", .discharge_positive = true, .battery = true },
	{ .name = "macsmc-ac", .ac = true },
};

static struct smcbatt_supply *smcbatt_lookup(struct power_supply *psy)
{
	struct smcbatt_supply *s;
	int i;

	for (i = 0; i < ARRAY_SIZE(smcbatt_supplies); i++) {
		s = &smcbatt_supplies[i];
		if (s->orig != NULL && psy->desc == &s->desc)
			return s;
	}

	return NULL;
}

/*
 * P = V * I. current_now is positive while charging, so the battery flips the
 * sign to make power_now the draw on the pack, as upower expects. macsmc-ac
 * keeps it: a mains supply's power_now is the power it delivers, and negating
 * it showed a 57 W adapter as -57 W (also in macsmc_ac's power1_input). A
 * supply that is not present reports 0 V (macsmc-ac when unplugged): answer
 * 0 W rather than -EOPNOTSUPP, which is what makes the core log an error on
 * every read.
 */
static int smcbatt_power_now(struct smcbatt_supply *s, struct power_supply *psy,
			     int *out)
{
	union power_supply_propval volt, curr;
	s64 power;

	if (s->orig->get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &volt) < 0)
		return -EOPNOTSUPP;
	if (s->orig->get_property(psy, POWER_SUPPLY_PROP_CURRENT_NOW, &curr) < 0)
		return -EOPNOTSUPP;

	power = (s64)volt.intval * curr.intval / SMCBATT_POWER_SCALE;
	if (s->discharge_positive)
		power = -power;
	if (power > INT_MAX)
		power = INT_MAX;
	else if (power < INT_MIN)
		power = INT_MIN;

	*out = power;
	return 0;
}

/*
 * Energy at the pack's design voltage. MacBookPro11,2 reports 11.967 V for
 * voltage_max_design and ~11.95 V live, so the numbers stay within a tenth of
 * a percent of what the driver produced, but no longer move when the load
 * makes the pack sag.
 */
static int smcbatt_energy(struct smcbatt_supply *s, struct power_supply *psy,
			  enum power_supply_property charge_prop, int *out)
{
	union power_supply_propval charge, vmax;
	s64 energy;

	if (s->orig->get_property(psy, charge_prop, &charge) < 0 ||
	    charge.intval < 0)
		return -EOPNOTSUPP;
	if (s->orig->get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
				  &vmax) < 0 || vmax.intval <= 0)
		return -EOPNOTSUPP;

	energy = (s64)charge.intval * vmax.intval / SMCBATT_ENERGY_SCALE;
	if (energy > INT_MAX)
		energy = INT_MAX;

	*out = energy;
	return 0;
}

/*
 * The pack current is the only live indicator of the power source: the SMC
 * updates it continuously, whereas the adapter's ONLINE and the battery's
 * STATUS come from cached flags that survive a resume. A small deadband keeps
 * quiescent readings while plugged in from looking like a discharge.
 */
static int smcbatt_battery_current(int *out)
{
	struct power_supply *bat;
	union power_supply_propval curr;
	int ret;

	bat = power_supply_get_by_name("macsmc-battery");
	if (bat == NULL)
		return -ENODEV;

	ret = bat->desc->get_property(bat, POWER_SUPPLY_PROP_CURRENT_NOW, &curr);
	power_supply_put(bat);
	if (ret < 0)
		return ret;

	*out = curr.intval;
	return 0;
}

static int smcbatt_online(int *out)
{
	int draw;

	if (smcbatt_battery_current(&draw) < 0)
		return -EOPNOTSUPP;

	*out = draw < -SMCBATT_CURRENT_DEADBAND ? 0 : 1;
	return 0;
}

static int smcbatt_status(struct smcbatt_supply *s, struct power_supply *psy,
			  int *out)
{
	union power_supply_propval curr, cap;

	if (s->orig->get_property(psy, POWER_SUPPLY_PROP_CURRENT_NOW, &curr) < 0)
		return -EOPNOTSUPP;

	if (curr.intval < -SMCBATT_CURRENT_DEADBAND) {
		*out = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}
	if (curr.intval > SMCBATT_CURRENT_DEADBAND) {
		*out = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	}

	if (s->orig->get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &cap) == 0 &&
	    cap.intval >= 99)
		*out = POWER_SUPPLY_STATUS_FULL;
	else
		*out = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

static int smcbatt_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct smcbatt_supply *s = smcbatt_lookup(psy);
	int ret;

	if (s == NULL)
		return -EINVAL;

	/* Default to whatever the driver would have answered. */
	ret = s->orig->get_property(psy, psp, val);

	switch (psp) {
	case POWER_SUPPLY_PROP_POWER_NOW:
		if (smcbatt_power_now(s, psy, &val->intval) == 0)
			ret = 0;
		break;

	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (smcbatt_energy(s, psy, POWER_SUPPLY_PROP_CHARGE_NOW,
				   &val->intval) == 0)
			ret = 0;
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL:
		if (smcbatt_energy(s, psy, POWER_SUPPLY_PROP_CHARGE_FULL,
				   &val->intval) == 0)
			ret = 0;
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		if (s->ac && smcbatt_online(&val->intval) == 0)
			ret = 0;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (s->battery && smcbatt_status(s, psy, &val->intval) == 0)
			ret = 0;
		break;

	default:
		break;
	}

	return ret;
}

/*
 * The only live indicator of the power source is the pack current, which the
 * SMC keeps current. macsmc-ac's ONLINE flag is stale across plug/unplug, so
 * poll the current every 2 s and emit a change event when its sign flips.  The
 * PM_POST_SUSPEND notifier covers the wake path; the poll covers plug/unplug
 * while the machine is running.
 *
 * The poll is a delayed work item, not a timer: reading an SMC key goes through
 * apple_smc_acpi_mmio_wait(), which sleeps. A timer callback runs in softirq
 * context, where that sleep is a "scheduling while atomic" BUG and leaves the
 * preempt count corrupted. Work queued here runs in process context, so the
 * same read is allowed.
 */
static atomic_t smcbatt_last_sign;
static struct delayed_work smcbatt_poll;

static void smcbatt_poll_func(struct work_struct *work)
{
	struct power_supply *psy;
	int i;
	int draw;
	int sign;

	sign = 0;
	if (smcbatt_battery_current(&draw) == 0) {
		/* -1 = discharging, +1 = charging/idle. */
		sign = draw < -SMCBATT_CURRENT_DEADBAND ? -1 : 1;
	}

	if (sign != atomic_read(&smcbatt_last_sign)) {
		printk(KERN_INFO "smcbatt: power source sign changed %d -> %d\n",
			atomic_read(&smcbatt_last_sign), sign);
		for (i = 0; i < ARRAY_SIZE(smcbatt_supplies); i++) {
			psy = power_supply_get_by_name(smcbatt_supplies[i].name);
			if (psy != NULL) {
				power_supply_changed(psy);
				power_supply_put(psy);
			}
		}
		atomic_set(&smcbatt_last_sign, sign);
	}

	schedule_delayed_work(&smcbatt_poll, msecs_to_jiffies(2000));
}

static void smcbatt_poll_setup(void)
{
	atomic_set(&smcbatt_last_sign, 0);
	INIT_DELAYED_WORK(&smcbatt_poll, smcbatt_poll_func);
	schedule_delayed_work(&smcbatt_poll, msecs_to_jiffies(2000));
}

static void smcbatt_poll_cleanup(void)
{
	cancel_delayed_work_sync(&smcbatt_poll);
}

/*
 * macsmc-power only refreshes the adapter state on an SMC notification, and
 * none arrives for a change that happened while the machine was asleep. The
 * values above are computed on demand, but upower and TLP's udev rule act on
 * the change event, so send one after resume, and also when the pack current
 * sign changes on plug/unplug.
 */
static int smcbatt_pm_notify(struct notifier_block *nb, unsigned long action,
			     void *data)
{
	struct power_supply *psy;
	int i;

	if (action != PM_POST_SUSPEND)
		return NOTIFY_DONE;

	for (i = 0; i < ARRAY_SIZE(smcbatt_supplies); i++) {
		psy = power_supply_get_by_name(smcbatt_supplies[i].name);
		if (psy == NULL)
			continue;
		power_supply_changed(psy);
		power_supply_put(psy);
	}

	return NOTIFY_DONE;
}

static struct notifier_block smcbatt_pm_nb = {
	.notifier_call = smcbatt_pm_notify,
};

static int __init smcbatt_init(void)
{
	struct smcbatt_supply *s;
	struct power_supply *psy;
	int i, fixed = 0;

	for (i = 0; i < ARRAY_SIZE(smcbatt_supplies); i++) {
		s = &smcbatt_supplies[i];

		psy = power_supply_get_by_name(s->name);
		if (psy == NULL)
			continue;

		s->orig = psy->desc;
		s->desc = *s->orig;
		s->desc.get_property = smcbatt_get_property;
		psy->desc = &s->desc;

		power_supply_changed(psy);
		power_supply_put(psy);
		fixed++;
	}

	if (fixed == 0) {
		pr_err("smcbatt: no macsmc power supplies found; is macsmc-power loaded?\n");
		return -ENODEV;
	}

	if (register_pm_notifier(&smcbatt_pm_nb) != 0)
		printk(KERN_WARNING "smcbatt: no resume notifier; AC state may lag after wake\n");

	smcbatt_poll_setup();

	return 0;
}

static void __exit smcbatt_exit(void)
{
	struct smcbatt_supply *s;
	struct power_supply *psy;
	int i;

	/* Finish any running poll before the descs it reads through go away. */
	smcbatt_poll_cleanup();
	unregister_pm_notifier(&smcbatt_pm_nb);

	for (i = 0; i < ARRAY_SIZE(smcbatt_supplies); i++) {
		s = &smcbatt_supplies[i];
		if (s->orig == NULL)
			continue;

		psy = power_supply_get_by_name(s->name);
		if (psy == NULL) {
			/* macsmc-power is gone; the supply took its desc with it. */
			s->orig = NULL;
			continue;
		}

		if (psy->desc == &s->desc) {
			psy->desc = s->orig;
			power_supply_changed(psy);
		}

		power_supply_put(psy);
		s->orig = NULL;
	}
}

module_init(smcbatt_init);
module_exit(smcbatt_exit);

MODULE_SOFTDEP("pre: macsmc_power");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Battery power/energy and stale AC-state fixups for Apple SMC (macsmc) machines, including plug-in/unplug change events");
MODULE_AUTHOR("Eisa Ahmed");
