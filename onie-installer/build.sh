#!/bin/bash
# Build ONIE installer .bin for open-nos-as5610.
# Produces: open-nos-as5610-YYYYMMDD.bin (self-extracting script + data.tar)
#
# Prereqs: FIT image (boot/nos-powerpc.itb or uImage-powerpc.itb), sysroot.squash.xz
# Set KERNEL_FIT and ROOTFS_SQUASH to override paths.
#
# Usage:
#   ./build.sh
#   KERNEL_FIT=../boot/nos-powerpc.itb ROOTFS_SQUASH=./sysroot.squash.xz ./build.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Inputs (defaults for after full build)
KERNEL_FIT="${KERNEL_FIT:-$REPO_ROOT/boot/nos-powerpc.itb}"
if [ ! -f "$KERNEL_FIT" ]; then
	KERNEL_FIT="${KERNEL_FIT:-$SCRIPT_DIR/uImage-powerpc.itb}"
fi
ROOTFS_SQUASH="${ROOTFS_SQUASH:-$SCRIPT_DIR/sysroot.squash.xz}"

OUT_BIN="open-nos-as5610-$(date +%Y%m%d).bin"
DATA_DIR="$SCRIPT_DIR/.data.$$"
trap "rm -rf '$DATA_DIR'" EXIT
mkdir -p "$DATA_DIR"

# control file (metadata for NOS)
cat > "$DATA_DIR/control" <<EOF
Architecture: powerpc
Platforms: accton_as5610_52x
OS-Release: open-nos-as5610
Installer-Version: 1.0
EOF

# Platform files (already in repo)
mkdir -p "$DATA_DIR/cumulus/init/accton_as5610_52x"
cp -f "$SCRIPT_DIR/cumulus/init/accton_as5610_52x/platform.conf" "$DATA_DIR/cumulus/init/accton_as5610_52x/"
cp -f "$SCRIPT_DIR/cumulus/init/accton_as5610_52x/platform.fdisk" "$DATA_DIR/cumulus/init/accton_as5610_52x/"
mkdir -p "$DATA_DIR/uboot_env"
cp -f "$SCRIPT_DIR/uboot_env/"*.inc "$DATA_DIR/uboot_env/" 2>/dev/null || true

# Kernel FIT and rootfs (required)
if [ -f "$KERNEL_FIT" ]; then
	cp -f "$KERNEL_FIT" "$DATA_DIR/nos-powerpc.itb"
	[ -f "$DATA_DIR/nos-powerpc.itb" ] && ln -sf nos-powerpc.itb "$DATA_DIR/uImage-powerpc.itb" 2>/dev/null || true
else
	echo "WARN: No kernel FIT at $KERNEL_FIT; run boot/build-fit.sh first (needs kernel, DTB, initramfs)."
fi
if [ -f "$ROOTFS_SQUASH" ]; then
	cp -f "$ROOTFS_SQUASH" "$DATA_DIR/sysroot.squash.xz"
else
	echo "WARN: No rootfs at $ROOTFS_SQUASH; run rootfs/build.sh first."
fi

# Build data.tar (list only existing files)
TAR_FILES="control cumulus uboot_env"
[ -f "$DATA_DIR/nos-powerpc.itb" ] && TAR_FILES="$TAR_FILES nos-powerpc.itb"
[ -f "$DATA_DIR/sysroot.squash.xz" ] && TAR_FILES="$TAR_FILES sysroot.squash.xz"
tar -cf "$SCRIPT_DIR/data.tar" -C "$DATA_DIR" $TAR_FILES

# Self-extracting installer: script + payload (everything after __ARCHIVE__ is data.tar)
{
	cat "$SCRIPT_DIR/install.sh"
	cat "$SCRIPT_DIR/data.tar"
} > "$OUT_BIN"
chmod +x "$OUT_BIN"
rm -f "$SCRIPT_DIR/data.tar"
echo "Built: $SCRIPT_DIR/$OUT_BIN"
echo "Install on switch (ONIE): onie-nos-install http://<server>/$OUT_BIN"
