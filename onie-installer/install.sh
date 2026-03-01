#!/bin/sh
# open-nos-as5610 ONIE installer (self-extracting).
# Run by ONIE: onie-nos-install http://.../open-nos-as5610-YYYYMMDD.bin
# Payload after __ARCHIVE__: control.tar.xz, data.tar

set -e
INSTALLER_DIR="${INSTALLER_DIR:-/tmp/onie-nos-install}"
ARCHIVE_MARKER="__ARCHIVE__"

# Find archive boundary: payload is everything after the line __ARCHIVE__
echo "Extracting installer payload..."
# Only feed first 200 lines to awk (script is ~120 lines) so we don't read the whole 137MB file
LINE=$(head -n 200 "$0" | awk "/^${ARCHIVE_MARKER}\$/ { print NR; exit }")
[ -z "$LINE" ] && { echo "Invalid installer: __ARCHIVE__ not found"; exit 1; }
SKIP=$(head -n "$LINE" "$0" | wc -c)
# Use 4k blocks for speed (avoid single-byte dd which is extremely slow on 137MB)
# Skip exactly SKIP bytes: first block may be partial, then copy rest in 4k chunks
{
  dd if="$0" bs=4096 skip=$((SKIP / 4096)) count=1 2>/dev/null | dd bs=1 skip=$((SKIP % 4096)) 2>/dev/null
  dd if="$0" bs=4096 skip=$((SKIP / 4096 + 1)) 2>/dev/null
} > /tmp/_nos_payload.bin

# Extract control.tar.xz and data.tar from payload
# Format: control.tar.xz (xz), then data.tar (uncompressed)
cd /tmp
# Try to split: first part is control (tar.xz), rest is data.tar
# Simpler: we build with control.tar.xz followed by data.tar; use fixed offsets or headers.
# Minimal: assume payload is control.tar.xz concatenated with data.tar
# We need length of control.tar.xz. Store it in control as first line? Or use a single data.tar that contains everything.
# Cumulus uses: control.tar.xz, then data.tar. So payload = control.tar.xz + data.tar.
# Parse: xz magic FD 37 7A; find end of xz stream. Easier: build payload as control.tar.xz (with known size in script) + data.tar.
# Even simpler: use a single data.tar that includes control/ and data/ subdirs, and have install.sh extract data.tar only, then read control from data/control/...
# Standard approach: after __ARCHIVE__, we have:
#   [control.tar.xz][data.tar]
# To extract without knowing sizes: use a sentinel. e.g. __CONTROL_END__ after control.tar.xz.
# build.sh can write: control.tar.xz, then 8-byte length of control.tar.xz (for skip), then data.tar.
# Installer: read 8 bytes (length L), read L bytes = control.tar.xz, rest = data.tar.
# Implement: build.sh writes payload as: control.tar.xz + data.tar. Installer needs to know where control ends. So we write a 0-padded 12-char length in ASCII at start of payload, e.g. "000000123456" = 123456 bytes control. Then installer reads 12 chars, skips 12 bytes, reads control.tar.xz, then data.tar.
# Simpler for v1: single data.tar containing everything (control file, platform.conf, platform.fdisk, uboot_env, uImage-powerpc.itb, sysroot.squash.xz). Then install.sh just extracts data.tar and uses paths inside it.
# So: payload = data.tar only. data.tar contains:
#   control
#   cumulus/init/accton_as5610_52x/platform.conf
#   cumulus/init/accton_as5610_52x/platform.fdisk
#   uboot_env/...
#   uImage-powerpc.itb
#   sysroot.squash.xz
# Then we don't need control.tar.xz separately. Installer extracts data.tar, sources platform.conf, runs fdisk, dd, fw_setenv, reboot.
# Let's do that.
mkdir -p "$INSTALLER_DIR"
cd "$INSTALLER_DIR"
# Payload is data.tar only (see build.sh)
# Extract only small files (control, platform, uboot_env); stream kernel/rootfs later to avoid /tmp size limits
tar -xf /tmp/_nos_payload.bin control \
	cumulus/init/accton_as5610_52x/platform.conf \
	cumulus/init/accton_as5610_52x/platform.fdisk \
	uboot_env \
	2>/dev/null || { echo "Failed to extract installer metadata"; exit 1; }

# Detect platform (ONIE)
if [ -n "$onie_sysinfo" ]; then
	PLATFORM=$($onie_sysinfo -p 2>/dev/null || true)
elif [ -f /etc/onielabel ]; then
	PLATFORM=$(grep -o 'accton_as5610[^ ]*' /etc/onielabel 2>/dev/null | head -1) || true
fi
PLATFORM="${PLATFORM:-accton_as5610_52x}"
# Normalize: Edgecore AS5610-52X and Accton AS5610-52X use same partition layout
case "$PLATFORM" in
	*edgecore*as5610*52*|*edgecore*5610*52*) PLATFORM="accton_as5610_52x";;
	*accton*as5610*52*) PLATFORM="accton_as5610_52x";;
	*as5610*52*) PLATFORM="accton_as5610_52x";;
esac

# Block device (USB flash on AS5610)
BLK_DEV="sda"
if [ ! -b "/dev/${BLK_DEV}" ]; then
	for d in sda sdb; do
		[ -b "/dev/$d" ] && BLK_DEV="$d" && break
	done
fi
if [ ! -b "/dev/${BLK_DEV}" ]; then
	echo "No block device found for install"
	exit 1
fi

# Load platform config
CONF_DIR="$INSTALLER_DIR/cumulus/init/accton_as5610_52x"
blk_dev="$BLK_DEV"
if [ -f "$CONF_DIR/platform.conf" ]; then
	. "$CONF_DIR/platform.conf"
else
	persist_part="${blk_dev}1"
	rw_rootpart="${blk_dev}3"
	kernel_part1="${blk_dev}5"
	ro_part1="${blk_dev}6"
	kernel_part2="${blk_dev}7"
	ro_part2="${blk_dev}8"
fi

# Partition and format (strip # comments only; keep blank lines for fdisk "default" answers)
echo "Partitioning /dev/${BLK_DEV}..."
grep -v '^#' "$CONF_DIR/platform.fdisk" | fdisk -u /dev/${BLK_DEV} || { echo "Partitioning failed"; exit 1; }
sync
# Force kernel to re-read partition table (sda5/sda6 in single-slot layout)
blockdev --rereadpt /dev/${BLK_DEV} 2>/dev/null || partprobe /dev/${BLK_DEV} 2>/dev/null || true
sleep 1
mkfs.ext2 -q /dev/${persist_part} || { echo "mkfs persist failed"; exit 1; }
mkfs.ext2 -q /dev/${rw_rootpart} || { echo "mkfs rw-overlay failed"; exit 1; }

# Install kernel and rootfs (single-slot: sda5=kernel, sda6=rootfs)
echo "Installing kernel and rootfs..."
KDEV="$kernel_part1"
RDEV="$ro_part1"
tar -xOf /tmp/_nos_payload.bin nos-powerpc.itb 2>/dev/null | dd of="/dev/${KDEV}" bs=4k conv=fsync || { echo "Kernel write to ${KDEV} failed"; exit 1; }
tar -xOf /tmp/_nos_payload.bin sysroot.squash.xz 2>/dev/null | dd of="/dev/${RDEV}" bs=4k conv=fsync || { echo "Rootfs write to ${RDEV} failed"; exit 1; }

# U-Boot environment: apply via script file (Cumulus/ONL style) so values are not mangled by shell.
# Do NOT call onie-nos-mode -s; it sets nos_bootcmd=true and overwrites our full boot sequence.
	if command -v fw_setenv >/dev/null 2>&1 && [ -d "$INSTALLER_DIR/uboot_env" ]; then
	env_script="$INSTALLER_DIR/env.txt"
	: > "$env_script"
	for inc in common_env.inc as5610_52x.platform.inc; do
		f="$INSTALLER_DIR/uboot_env/$inc"
		[ -f "$f" ] || continue
		sed '/^[	 ]*#/d' "$f" >> "$env_script"
	done
	fw_setenv -f -s "$env_script" 2>/dev/null || { echo "Warning: fw_setenv -s failed"; true; }
	# CRITICAL: Override onie_boot_reason=install (set by ONIE as "sticky").
	# Deleting the variable does not work reliably; setting it to 'nos'
	# prevents check_boot_reason from re-entering ONIE install mode.
	# To trigger reinstall from a running NOS: fw_setenv onie_boot_reason install && reboot
	fw_setenv onie_boot_reason nos 2>/dev/null || true
	fi

echo "Install complete. Rebooting..."
reboot
exit 0
__ARCHIVE__
