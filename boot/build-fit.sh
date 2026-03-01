#!/bin/bash
# Build FIT image (nos-powerpc.itb) for AS5610-52X.
# Requires: mkimage (u-boot-tools), kernel uImage, DTB, initramfs (initramfs/build.sh).
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

command -v mkimage >/dev/null 2>&1 || { echo "Missing mkimage (u-boot-tools)"; exit 1; }
command -v dumpimage >/dev/null 2>&1 || { echo "Missing dumpimage (u-boot-tools)"; exit 1; }

if [ ! -f "$KERNEL_IMAGE" ]; then
	echo "Missing kernel: $KERNEL_IMAGE (build kernel first or copy from build server linux-5.10/arch/powerpc/boot/uImage)"
	exit 1
fi
if [ ! -f "$DTB_IMAGE" ]; then
	echo "Missing DTB: $DTB_IMAGE (obtain from ONL as5610_52x.dts or extract from Cumulus FIT)"
	exit 1
fi

# mkimage -f <its> requires dtc. If missing, try to rebuild inside a Debian container.
if ! command -v dtc >/dev/null 2>&1; then
	if [ "${NO_DOCKER_FALLBACK:-0}" != "1" ] && command -v docker >/dev/null 2>&1; then
		echo "WARN: dtc not found; building FIT via Docker (device-tree-compiler + u-boot-tools)..."
		exec docker run --rm \
			-v "$SCRIPT_DIR:/work" -w /work \
			-e NO_DOCKER_FALLBACK=1 \
			debian:bookworm bash -c \
			'apt-get update -qq && apt-get install -y -qq u-boot-tools device-tree-compiler >/dev/null && ./build-fit.sh '"$(printf '%q' "$KERNEL_IMAGE")"' '"$(printf '%q' "$DTB_IMAGE")"' '"$(printf '%q' "$INITRAMFS_IMAGE")"''
	fi
	echo "Missing dtc (device-tree-compiler). Install it (e.g. apt-get install device-tree-compiler) or ensure Docker is available."
	exit 1
fi

OUT="nos-powerpc.itb"
# Copy inputs into CWD for mkimage -f nos.its
cp -f "$KERNEL_IMAGE" uImage || true
cp -f "$DTB_IMAGE" as5610_52x.dtb || true

# Kernel payload: uImage is a legacy wrapper around a gzip stream. U-Boot FIT expects
# the raw kernel data, not the uImage header, otherwise bootm can fail with:
#   "ERROR: can't get kernel image!"
dumpimage -T kernel -p 0 -o kernel.gz uImage

ITS_IN="$SCRIPT_DIR/nos.its"

# Always include initramfs in the FIT so the config has kernel + ramdisk + fdt (match Cumulus).
# U-Boot 2013.01 on this machine can fail with "can't get kernel image!" if ramdisk is omitted.
if [ -f "$INITRAMFS_IMAGE" ]; then
	cp -f "$INITRAMFS_IMAGE" initramfs.cpio.gz || true
else
	echo "WARN: initramfs missing ($INITRAMFS_IMAGE); creating minimal stub so FIT has ramdisk."
	echo -n | gzip -n -9 > initramfs.cpio.gz
fi

if [ ! -f "$ITS_IN" ]; then
	echo "Missing ITS: $ITS_IN"
	exit 1
fi

# Build a FIT with configuration nodes matching ${cl.platform} (accton_as5610_52x / edgecore_as5610_52x).
mkimage -f "$ITS_IN" "$OUT"

echo "Built: $SCRIPT_DIR/$OUT"
