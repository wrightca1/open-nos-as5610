#!/bin/sh
# Reset U-Boot boot_count to 0 on successful NOS boot.
# U-Boot nos_bootcmd increments boot_count before each boot attempt;
# after 3 consecutive increments it triggers ONIE reinstall. We must reset
# it on every successful startup.
#
# fw_setenv requires /etc/fw_env.config pointing at the U-Boot env MTD partition.
# The AS5610-52X P2020 stores the env in SPI NOR flash; detect it from /proc/mtd.

# Detect the MTD partition named "uboot-env" or similar in /proc/mtd
# /proc/mtd format: dev:  size  erasesize  name
MTD_DEV=""
if [ -f /proc/mtd ]; then
    MTD_DEV=$(awk -F: '/"[uU][bB]oot.env|u.boot.env|uboot_env|UBOOT_ENV"/ { sub(/^mtd/, "/dev/mtd", $1); print $1; exit }' /proc/mtd 2>/dev/null || true)
fi

# Fallback: AS5610-52X P2020 ONIE SPI flash typical layout
# mtd0=u-boot, mtd1=u-boot-env (64KB, erasesize 64KB)
# Verify on your hardware with: cat /proc/mtd
if [ -z "$MTD_DEV" ]; then
    MTD_DEV="/dev/mtd1"
fi

if [ ! -e "$MTD_DEV" ]; then
    echo "nos-boot-success: $MTD_DEV not found; skipping boot_count reset"
    exit 0
fi

# Create fw_env.config: <device>  <offset>  <env-size>  <flash-sector-size>
echo "$MTD_DEV  0x000000  0x010000  0x010000" > /etc/fw_env.config

if command -v fw_setenv >/dev/null 2>&1; then
    fw_setenv boot_count 0 && echo "nos-boot-success: boot_count reset to 0" || \
        echo "nos-boot-success: WARNING fw_setenv boot_count 0 failed"
else
    echo "nos-boot-success: WARNING fw_setenv not found (install u-boot-tools)"
fi
