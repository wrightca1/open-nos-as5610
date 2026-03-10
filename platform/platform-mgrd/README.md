# platform-mgrd

Platform management daemon for the Edgecore AS5610-52X. Runs as a systemd service
and handles thermal fan control, PSU/fan health monitoring, LED management, and
SFP/QSFP EEPROM scanning.

## Features

- **Thermal fan control**: Reads all hwmon temperature sensors, sets fan PWM based on
  max temperature across 4 zones (35/45/55C thresholds). Fail-safe: 100% if no sensors found.
- **CPLD watchdog**: Pets the hardware watchdog every 15 seconds.
- **PSU monitoring**: Checks PSU presence and health, drives PSU LEDs (green/amber/off).
- **Fan monitoring**: Checks fan presence and health, drives fan LED. Logs airflow direction.
- **SFP/QSFP EEPROM**: Scans all 52 ports for installed optics, logs vendor/PN/SN.
  Uses sysfs (at24 driver) with I2C fallback.

## Dependencies

- **`i2c-mux-pca954x.ko`** must be loaded before starting (expands I2C buses for hwmon sensors).
- **`accton_as5610_cpld.ko`** must be loaded for CPLD sysfs attributes (fan PWM, PSU, LEDs, watchdog).

Both are loaded by `nos-bde-modules.service` which runs before `platform-mgrd.service`.

## Build

```bash
# Cross-compile for PPC32 (in Docker on build server)
make CROSS_COMPILE=powerpc-linux-gnu-

# Or specify compiler directly
powerpc-linux-gnu-gcc -std=gnu99 -Wall -O2 -static -o platform-mgrd main.c
```

## Install

```bash
make DESTDIR=/path/to/rootfs install
# Installs to $DESTDIR/usr/sbin/platform-mgrd
```

## Systemd Service

```ini
# /etc/systemd/system/platform-mgrd.service
[Unit]
Description=NOS Platform Manager (fan/temp/LEDs)
After=local-fs.target

[Service]
Type=simple
ExecStart=/usr/sbin/platform-mgrd
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

## Configuration

All tunables are compile-time defines in `main.c`:

| Define | Default | Description |
|--------|---------|-------------|
| `POLL_INTERVAL_S` | 30 | Full platform poll period (seconds) |
| `WDT_KEEPALIVE_S` | 15 | Watchdog keepalive interval (seconds) |
| `TEMP_LOW_MC` | 35000 | Low temp threshold (millidegrees C) |
| `TEMP_MED_MC` | 45000 | Medium temp threshold |
| `TEMP_HIGH_MC` | 55000 | High temp threshold |
| `PWM_LOW` | 96 | CPLD sysfs value for low fan (raw 12, ~39%) |
| `PWM_MED` | 168 | CPLD sysfs value for medium fan (raw 21, ~68%) |
| `PWM_HIGH` / `PWM_MAX` | 248 | CPLD sysfs value for max fan (raw 31, 100%) |

## Logging

Output goes to journald (line-buffered via `setvbuf`):

```
platform-mgrd: starting (AS5610-52X; poll=30s wdt=15s)
platform-mgrd: max_temp=41.000 C
platform-mgrd: fan airflow: back-to-front
platform-mgrd: WARNING PSU1 present but not OK
platform-mgrd: swp1 SFP vendor="FINISAR" pn="FTLX8571D3BCL" sn="ABC1234"
```
