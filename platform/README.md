# Platform Management

Handles hardware platform functions: CPLD, thermal sensors, fans, PSUs, LEDs, SFP/QSFP.

## Approach

Use **ONLP** (Open Network Linux Platform library) with the existing `accton_as5610_52x` platform
implementation from the ONL repository. This handles all platform I/O without us writing custom
code, and is fully open-source (Apache 2.0).

If ONLP is not suitable, implement a lightweight `platform-mgrd` daemon that reads/writes the
sysfs paths documented in the RE docs.

## Platform Hardware

| Hardware | Interface | Path |
|----------|-----------|------|
| CPLD | sysfs | `/sys/devices/ff705000.localbus/ea000000.cpld` |
| ASIC temp | sysfs/hwmon | `/sys/devices/pci0000:00/.../0000:01:00.0/temp1_input` |
| Board temps (×7) | I2C (9-004d) | `/sys/.../i2c-9/9-004d/temp1..7_input` |
| NE1617A temps (×2) | I2C (9-0018) | `/sys/.../i2c-9/9-0018/temp1_input`, `temp2_input` |
| Fan PWM | CPLD sysfs | `.../ea000000.cpld/pwm1` (0–248) |
| PSU 1/2 status | CPLD sysfs | `.../psu_pwr1_present`, `psu_pwr1_dc_ok` |
| SFP EEPROM | I2C | i2c-22..i2c-69, address 0x50 |
| QSFP EEPROM | I2C | i2c-70..i2c-73, address 0x50 |
| Status LEDs | CPLD sysfs | `led_psu1`, `led_psu2`, `led_diag`, `led_fan` |

## CPLD Driver

The `accton_as5610_52x_cpld` driver is required. It is available in:
- Cumulus kernel tree (GPL, but we cannot use that)
- ONL kernel tree — use this (Apache 2.0 / GPL-2.0 depending on component)

## RE References

- `../../docs/reverse-engineering/PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md`
- `../../docs/reverse-engineering/SFP_TURNUP_AND_ACCESS.md`
