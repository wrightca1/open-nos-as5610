#!/bin/bash
# Build FIT image (nos-powerpc.itb) for AS5610-52X.
# Requires: mkimage (u-boot-tools), kernel uImage, DTB, optional initramfs.
#
# Usage:
#   ./build-fit.sh [kernel_uImage] [dtb] [initramfs.cpio.gz]
# Or set: KERNEL_IMAGE DTB_IMAGE INITRAMFS_IMAGE
#
# DTB: Get as5610_52x.dtb from ONL tree or extract from Cumulus:
#   dumpimage -l /path/to/cumulus.itb
#   dumpimage -i /path/to/cumulus.itb -p <fdt_index> -o as5610_52x.dtb

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

KERNEL_IMAGE="${1:-${KERNEL_IMAGE:-uImage}}"
DTB_IMAGE="${2:-${DTB_IMAGE:-as5610_52x.dtb}}"
INITRAMFS_IMAGE="${3:-${INITRAMFS_IMAGE:-../initramfs/initramfs.cpio.gz}}"

if [ ! -f "$KERNEL_IMAGE" ]; then
	echo "Missing kernel: $KERNEL_IMAGE (build kernel first or copy from build server linux-5.10/arch/powerpc/boot/uImage)"
	exit 1
fi
if [ ! -f "$DTB_IMAGE" ]; then
	echo "Missing DTB: $DTB_IMAGE (obtain from ONL as5610_52x.dts or extract from Cumulus FIT)"
	exit 1
fi

OUT="nos-powerpc.itb"
# Build FIT with mkimage; its file must have paths relative to CWD or absolute
cp -f "$KERNEL_IMAGE" uImage 2>/dev/null || true
cp -f "$DTB_IMAGE" as5610_52x.dtb 2>/dev/null || true

if [ -f "$INITRAMFS_IMAGE" ]; then
	cp -f "$INITRAMFS_IMAGE" initramfs.cpio.gz 2>/dev/null || true
	mkimage -f nos.its -k . -d . "$OUT" 2>/dev/null || {
		# If its doesn't include initramfs, build without
		mkimage -f nos.its "$OUT"
	}
else
	mkimage -f nos.its "$OUT"
fi

echo "Built: $SCRIPT_DIR/$OUT"
