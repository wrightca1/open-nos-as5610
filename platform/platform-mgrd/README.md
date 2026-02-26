# platform-mgrd

Minimal platform management daemon for AS5610-52X. Polls hwmon (thermal, fan) and can be extended for CPLD PSU/LED and fan PWM control.

## Build

```bash
# Native
make

# PPC32 (for rootfs)
make CROSS_COMPILE=powerpc-linux-gnu-
```

## Install

```bash
make DESTDIR=/mnt/rootfs install
```

## Full platform support

For complete thermal/fan/PSU/LED handling, integrate **ONLP** (Open Network Linux Platform) with the `accton_as5610_52x` platform from the ONL repository. This daemon is a minimal placeholder that only reads hwmon temps.

## RE references

- `../../docs/reverse-engineering/PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md`
- `../../docs/reverse-engineering/SFP_TURNUP_AND_ACCESS.md`
