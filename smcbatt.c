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
 * macsmc-power's property handlers are static, so rather than patch or replace
 * the driver this module swaps in a copy of its power_supply_desc whose
 * get_property() delegates to the original and only answers POWER_NOW and
 * ENERGY_NOW/ENERGY_FULL itself. Nothing in the macsmc stack is modified, and
 * unloading the module puts the original handlers back.
 *
 * Properties the driver never registers (energy-full-design, time-to-empty)
 * cannot be added this way; they need a change in macsmc-power.
 */

#include <linux/module.h>
#include <linux/power_supply.h>

/* µV * µA = pW, /1e6 -> µW. µAh * µV = pWh, /1e6 -> µWh. */
#define SMCBATT_POWER_SCALE	1000000
#define SMCBATT_ENERGY_SCALE	1000000

struct smcbatt_supply {
	const char *name;
	const struct power_supply_desc *orig;
	struct power_supply_desc desc;
};

static struct smcbatt_supply smcbatt_supplies[] = {
	{ .name = "macsmc-battery" },
	{ .name = "macsmc-ac" },
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
 * P = V * I. current_now is positive while charging, so flip the sign to make
 * power_now the draw on the pack, as upower expects. A supply that is not
 * present reports 0 V (macsmc-ac when unplugged): answer 0 W rather than
 * -EOPNOTSUPP, which is what makes the core log an error on every read.
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

	power = -(s64)volt.intval * curr.intval / SMCBATT_POWER_SCALE;
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

	default:
		break;
	}

	return ret;
}

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

	return 0;
}

static void __exit smcbatt_exit(void)
{
	struct smcbatt_supply *s;
	struct power_supply *psy;
	int i;

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
MODULE_DESCRIPTION("Battery power/energy fixups for Apple SMC (macsmc) machines");
MODULE_AUTHOR("Eisa Ahmed");
