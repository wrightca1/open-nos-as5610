# Platform Management

Handles hardware platform functions: CPLD, thermal sensors, fans, PSUs, LEDs, SFP/QSFP.

## Components

| Component | Description |
|-----------|-------------|
| `drivers/accton_as5610_cpld.c` | Out-of-tree kernel module for CPLD sysfs interface |
| `platform-mgrd/` | Userspace daemon: thermal fan control, PSU/fan monitoring, LED management, SFP EEPROM |
| `../tools/cpld-ctl.c` | Standalone CPLD register read/write utility via `/dev/mem` mmap |

## Required Kernel Modules

Loaded at boot by `nos-bde-modules.service` (in order):

1. **`i2c-mux-pca954x.ko`** — Expands I2C mux topology (2 base buses to 70 buses). Required BEFORE CPLD module and platform-mgrd. Without this, hwmon sensors and SFP buses are not accessible.
2. **`nos_kernel_bde.ko`** — BCM56846 PCI BAR0 + DMA
3. **`nos_user_bde.ko`** — Userspace ioctl interface to BDE
4. **`accton_as5610_cpld.ko`** — CPLD sysfs attributes (fan PWM, PSU status, LEDs, watchdog)

## Platform Hardware

### CPLD (memory-mapped at 0xEA000000, eLBC chip select 1)

| Register | Offset | Description |
|----------|--------|-------------|
| PSU2 status | 0x01 | PSU 2 present/OK bits |
| PSU1 status | 0x02 | PSU 1 present/OK bits |
| System status | 0x03 | Fan present (bit2), fan OK (bit3), airflow (bit4: 0=F2B, 1=B2F) |
| Fan PWM | 0x0D | 5-bit speed (0x00-0x1F), duty = raw/31 |
| LED control | 0x13 | System LED register |
| LED locator | 0x15 | Locator LED register |

**Sysfs interface** (via `accton_as5610_cpld.ko`):

| Attribute | Path | Scale |
|-----------|------|-------|
| `pwm1` | `.../ea000000.cpld/pwm1` | 0-248 (raw = sysfs/8, 5-bit 0-31) |
| `system_fan_ok` | `.../ea000000.cpld/system_fan_ok` | 1=OK, 0=fault |
| `system_fan_present` | `.../ea000000.cpld/system_fan_present` | 1=present |
| `system_fan_air_flow` | `.../ea000000.cpld/system_fan_air_flow` | "front-to-back" or "back-to-front" |
| `psu_pwr1_present` | `.../ea000000.cpld/psu_pwr1_present` | 1=present |
| `psu_pwr1_all_ok` | `.../ea000000.cpld/psu_pwr1_all_ok` | 1=OK |
| `led_diag` | `.../ea000000.cpld/led_diag` | "green", "amber", "off" |
| `led_fan` | `.../ea000000.cpld/led_fan` | "green", "amber", "off" |
| `led_psu1` / `led_psu2` | `.../ea000000.cpld/led_psu{1,2}` | "green", "amber", "off" |
| `watch_dog_keep_alive` | `.../ea000000.cpld/watch_dog_keep_alive` | Write "1" to pet |

### Temperature Sensors

| Sensor | Bus | Address | Driver | Channels | hwmon |
|--------|-----|---------|--------|----------|-------|
| MAX6697 | i2c-9 | 0x4d | max6697 | 8 (temp1-8) | hwmon0 |
| MAX1617 (NE1617A) | i2c-9 | 0x18 | adm1021 | 2 (temp1=local, temp2=remote) | hwmon1 |

Both sensors are on i2c-9 (via PCA9548 mux@0x70 on i2c-0, channel 7).
The MAX1617 temp2 (remote sensor) typically reads the highest temperature (~40C).

**Requires**: `i2c-mux-pca954x.ko` loaded to expand mux topology and bind sensor drivers.

### Fan Control

Thermal-driven PWM control by platform-mgrd:

| Max Temperature | PWM sysfs | CPLD raw | Duty |
|----------------|-----------|----------|------|
| < 35C | 96 | 12 (0x0C) | ~39% |
| 35-45C | 168 | 21 (0x15) | ~68% |
| 45-55C | 248 | 31 (0x1F) | 100% |
| > 55C or no sensors | 248 | 31 (0x1F) | 100% (fail-safe) |

### SFP/QSFP EEPROM

| Port Range | Type | I2C Bus | EEPROM sysfs index |
|------------|------|---------|-------------------|
| 1-48 | SFP+ | bus 22-69 (21+port) | eeprom(port+6) |
| 49-52 | QSFP+ | bus 18-21 | eeprom(port+6) |

Address: 0x50 (A0h, SFF-8472/SFF-8636).

### SFP GPIO Control (bus 17, PCA9506 expanders)

| Signal | Address | Active | Direction |
|--------|---------|--------|-----------|
| PRESENT | 0x20 | LOW = present | Input |
| TX_FAULT | 0x21 | HIGH = fault | Input |
| RX_LOS | 0x22 | HIGH = loss | Input |
| TX_DISABLE | 0x24 | HIGH = TX enabled | Output (IOC=0, OP=1) |

## cpld-ctl Utility

Direct CPLD register access via `/dev/mem` (no kernel driver needed):

```bash
cpld-ctl               # Dump all status registers
cpld-ctl read 0d       # Read fan PWM register
cpld-ctl write 0d 15   # Write fan PWM raw value
cpld-ctl fan 50        # Set fan to 50% duty
```
