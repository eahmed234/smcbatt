# Battery reporting fix for Intel Macs on Linux 7.3+

Linux 7.3 removed `applesmc` and moved Intel Macs onto the `macsmc` stack. On
pre-T2 models the ACPI SBS battery goes with it: `macsmc-power` registers
`macsmc-battery` and `macsmc-ac` instead. On a MacBookPro11,2 that driver gets
the pack wrong in two ways:

* `power_now` is read from SMC key `B0AP`, which this model does not have, so
  every read fails with `-95`:
  `power_supply macsmc-battery: driver failed to report 'power_now' property: -95`
  once per poll, forever, and the attribute is always empty.
* `energy_now` / `energy_full` are computed as `charge × voltage_now`. Pack
  voltage sags under load, so energy moves even when charge does not. A client
  that has to derive the draw from energy — upower does exactly that when
  `power_now` is missing — then reports roughly **100 W** and **45 minutes** of
  runtime for a pack at 83 %.

The real numbers on the machine this was written for are `current_now ≈ -1.45 A`
and `voltage_now ≈ 11.96 V`, i.e. ~17 W and hours of runtime.

## What it does

`macsmc-power`'s property handlers are static, so rather than patch or replace
the driver, `smcbatt` swaps in a copy of its `power_supply_desc` and answers the
affected properties itself, delegating everything else to the original:

| Property | Reported as |
| --- | --- |
| `POWER_NOW` | `V × I`; the battery is positive while discharging, `macsmc-ac` is positive while it delivers power. 0 when the supply is absent |
| `ENERGY_NOW`, `ENERGY_FULL` | charge at `voltage_max_design` instead of the live pack voltage |
| `ONLINE` (`macsmc-ac`) | derived from the pack current: 0 while the pack discharges |
| `STATUS` (`macsmc-battery`) | `DISCHARGING`/`CHARGING` from the pack current, `FULL` at 99 %+ |

Both follow a debounced source: a new source has to hold for three consecutive
polls (~6 s) before `ONLINE`, `STATUS` and the change event move.

That is enough for upower (and anything else reading the power supply class) to
show a real draw and a real remaining time. Unloading the module puts the
driver's handlers back; nothing in `macsmc` is replaced, patched or blacklisted.

`macsmc-ac` is wrapped too: unplugged it reports 0 V, and answering 0 W instead
of `-EOPNOTSUPP` is what stops the journal spam. Its `power_now` is not
negated — before that, a 57 W adapter showed up as **-57 W** in both
`power_supply` and `macsmc_ac`'s `power1_input`.

### Stale adapter state after resume

`macsmc-power` keeps the pre-sleep adapter state. Unplug the charger during
suspend and after resume `macsmc-ac` still answers `online=1` and
`macsmc-battery` still answers `Not charging`, while `current_now` shows a
discharge and `charge_now` falls. upower's `OnBattery` and TLP's profile
switching both read those cached values, which is what made the desktop power
mode switch to Performance on battery after every wake.

`smcbatt` derives `ONLINE` and `STATUS` from `current_now` (100 mA deadband,
debounced over three polls) and on `PM_POST_SUSPEND` emits
`power_supply_changed` for both supplies, so upower and the TLP udev rule
re-read immediately even though the driver sends no notification of its own.
The debounce matters: while charging, the pack current reads a brief discharge
as the charger ramps or the cells balance, and each such sample used to send
TLP a change event and flip the desktop profile.

### Polling across system suspend

The 2 s poll is cancelled on `PM_SUSPEND_PREPARE` and restarted on
`PM_POST_SUSPEND`, which the kernel also sends when a suspend attempt fails, so
polling never stops for good. Nothing in the module then issues an SMC
transaction while the ACPI EC is being suspended: a reader caught in
`apple_smc_acpi_mmio_wait()` at that moment can hold up the suspend path, and
the machine stays awake (panel on, tens of watts) instead of entering S3.

## Plug-in / unplug auto-switch

`macsmc-power` also does not emit a `power_supply` change uevent when
`macsmc-ac/online` flips on plug-in or unplug while the machine is running, so
the `85-tlp.rules` udev rule (`ACTION=="change", SUBSYSTEM=="power_supply", ...`
`RUN+="/usr/bin/tlp auto"`) never fires and TLP auto-switch does not trigger.
That is the separate bug behind a plug-in after wake not switching back to the
AC profile.

`smcbatt` polls `current_now` every 2 s and emits `power_supply_changed` for
both supplies when the power source changes — discharging while negative,
charging or idle while positive — which is the only live indicator of the power
source the SMC keeps current. A new source must hold for three consecutive
polls (~6 s) before the debounced `ONLINE`/`STATUS` and the event follow it, so
a single noisy sample cannot make TLP switch profiles. On a real plug-in the
event fires the udev rule and `tlp auto` picks `TLP_PROFILE_AC`; on unplug it
picks `TLP_PROFILE_BAT`. Idle polling emits nothing.

## Requirements

* An Intel Mac whose SMC is bound by `macsmc` (kernel 7.3+). On CachyOS that is
  the Intel-Mac port of 7.3; 6.18 LTS still ships `applesmc` + SBS and needs
  none of this, so DKMS skips those kernels on purpose.
* `dkms` and the headers of the kernel you run.

## Install (Arch / CachyOS)

```sh
cd packaging/aur/smcbatt-dkms-git && makepkg -si
sudo systemctl start smcbatt.service     # the package enables it, DKMS may still be building at install time
```

Or from source:

```sh
make
sudo insmod smcbatt.ko
systemctl restart upower
```

## Autoloading

`smcbatt.service` is a oneshot unit that runs `modprobe smcbatt` at boot. Its
`ConditionPathExists=/sys/class/power_supply/macsmc-battery` means it is skipped
on kernels and machines without the macsmc battery, so an LTS boot does not fail
`systemd-modules-load.service` or the unit itself. It is ordered before
`upower.service`, so upower's first refresh already sees the corrected values.

The package enables the unit but does not start it at install time: the DKMS
hook may not have built the module yet. `systemctl start smcbatt` once, or just
reboot.

## Verify

```sh
lsmod | grep smcbatt
upower -i /org/freedesktop/UPower/devices/battery_macsmc_battery | \
  grep -E 'energy|rate|time|percentage'
cat /sys/class/power_supply/macsmc-ac/online        # 0 while unplugged
cat /sys/class/power_supply/macsmc-battery/status   # Discharging while unplugged
journalctl -b -k | grep -c 'power_now'      # 0 after the module loads
```

Without the module: ~100 W, ~45 min at 83 %, and `power_now` errors in the log.
With it: ~15–20 W idle, hours of runtime, no errors.

## Charge limit

`macsmc-power` already exposes the limit macOS writes as
`/sys/class/power_supply/macsmc-battery/charge_control_end_threshold` (SMC key
`CHWA`), and UPower drives it through `EnableChargeThreshold`. A shell with a
charge-limit toggle therefore works without this project: Noctalia's toggle
writes 80% when on and 100% when off, straight through UPower.

`battlimit` covers what that path cannot: a value other than 80/100, and
re-applying a remembered value at boot. It ships with `/etc/battlimit.conf` and
`battlimit.service`, and the unit is installed **disabled** so it never rewrites
the limit behind a toggle you already use.

```sh
battlimit                 # current: 80%  configured: 80%
sudo battlimit set 75     # apply 75% and remember it (root)
sudo battlimit off        # back to 100%
sudo systemctl enable --now battlimit.service   # make the file authoritative
```

`battlimit get` prints the limit in use and `battlimit apply` is what the unit
runs. Every write is read back, so a limit the SMC ignored is reported as an
error instead of being silently believed. `battlimit.service` carries
`ConditionPathExists` on the attribute, so a kernel that still boots `applesmc`
with ACPI SBS simply skips it.

Two owners, so pick one:

* **Shell toggle** (default): leave `battlimit.service` disabled. Toggling on
  writes 80, off writes 100; the file is only a note.
* **`battlimit`**: `sudo systemctl enable --now battlimit.service`. The limit is
  then whatever `/etc/battlimit.conf` says at every boot, and a shell toggle only
  holds until the next one. Note that a config of 100 is active too: it resets a
  toggle-on back to 100 at boot.

## Limitations

`energy-full-design` and battery health stay unknown: `macsmc-power` never
registers `CHARGE_FULL_DESIGN`/`ENERGY_FULL_DESIGN`, and a property that does not
exist cannot be answered by wrapping a handler. That needs a change in the
driver itself. Percentage, draw, remaining time, cycles and temperature are all
correct with `smcbatt` loaded.

## License

GPL-2.0-only, see [`LICENSE`](LICENSE).
