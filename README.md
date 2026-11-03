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
the driver, `smcbatt` swaps in a copy of its `power_supply_desc` and answers two
properties itself, delegating everything else to the original:

| Property | Reported as |
| --- | --- |
| `POWER_NOW` | `V × I`, positive while discharging, 0 when the supply is absent |
| `ENERGY_NOW`, `ENERGY_FULL` | charge at `voltage_max_design` instead of the live pack voltage |

That is enough for upower (and anything else reading the power supply class) to
show a real draw and a real remaining time. Unloading the module puts the
driver's handlers back; nothing in `macsmc` is replaced, patched or blacklisted.

`macsmc-ac` is wrapped too: unplugged it reports 0 V, and answering 0 W instead
of `-EOPNOTSUPP` is what stops the journal spam.

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
journalctl -b -k | grep -c 'power_now'      # 0 after the module loads
```

Without the module: ~100 W, ~45 min at 83 %, and `power_now` errors in the log.
With it: ~15–20 W idle, hours of runtime, no errors.

## Charge limit

`macsmc-power` already exposes the limit macOS writes as
`/sys/class/power_supply/macsmc-battery/charge_control_end_threshold` (SMC key
`CHWA`), but it is root-only and nothing re-applies it after a boot. `battlimit`
adds the missing parts: the value lives in `/etc/battlimit.conf`, and
`battlimit.service` applies it at boot.

```sh
sudo battlimit set 80     # apply 80% now and remember it
battlimit                 # current: 80%  configured: 80%
sudo battlimit off        # stop limiting (100%)
```

`battlimit get` prints the limit in use and `battlimit apply` is what the unit
runs. Every write is read back, so a limit the SMC ignored is reported as an
error instead of being silently believed. `battlimit.service` carries
`ConditionPathExists` on the attribute, so a kernel that still boots `applesmc`
with ACPI SBS simply skips it.

A desktop shell may offer its own toggle for the same attribute (Noctalia has
one). That changes the limit at runtime only; `battlimit.service` re-applies
`/etc/battlimit.conf` at the next boot. For a change that should survive a
reboot, use `sudo battlimit set <percent>`, which updates the file and the
battery together.

## Limitations

`energy-full-design` and battery health stay unknown: `macsmc-power` never
registers `CHARGE_FULL_DESIGN`/`ENERGY_FULL_DESIGN`, and a property that does not
exist cannot be answered by wrapping a handler. That needs a change in the
driver itself. Percentage, draw, remaining time, cycles and temperature are all
correct with `smcbatt` loaded.

## License

GPL-2.0-only, see [`LICENSE`](LICENSE).
