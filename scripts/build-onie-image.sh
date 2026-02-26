#!/bin/bash
# Produce a loadable ONIE installer .bin for Edgecore/Accton AS5610-52X.
# Builds kernel (on server), FIT (kernel+DTB+initramfs), rootfs, then packages .bin
# with correct partition layout and platform info for the switch to boot.
#
# Usage:
#   ./scripts/build-onie-image.sh
#   DTB_IMAGE=boot/as5610_52x.dtb ./scripts/build-onie-image.sh
#   CUMULUS_BIN=/path/to/CumulusLinux-*.bin ./scripts/build-onie-image.sh   # extract DTB from Cumulus
#
# Options (env):
#   BUILD_SERVER=1          Run full build on server and copy back (default)
#   BUILD_SERVER=0          Use existing boot/uImage, bde/*.ko, build/ (skip server)
#   DTB_IMAGE=<path>        Path to as5610_52x.dtb (required unless CUMULUS_BIN set)
#   CUMULUS_BIN=<path>      Extract DTB from this Cumulus FIT (run scripts/extract-dtb.sh)
#   SKIP_ROOTFS=1           Use existing onie-installer/sysroot.squash.xz
#   SKIP_INITRAMFS=1        Use existing initramfs/initramfs.cpio.gz
#
# Output: onie-installer/open-nos-as5610-YYYYMMDD.bin (load via onie-nos-install)

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_SERVER="${BUILD_SERVER:-1}"
SKIP_ROOTFS="${SKIP_ROOTFS:-0}"
SKIP_INITRAMFS="${SKIP_INITRAMFS:-0}"

# Build host (match build-on-build-server.sh)
BUILD_USER="${BUILD_USER:-smiley}"
if [ -f "$REPO_ROOT/../build-config.sh" ]; then
    source "$REPO_ROOT/../build-config.sh"
    BUILD_HOST=$(get_build_host 2>/dev/null) || true
fi
BUILD_HOST="${BUILD_HOST:-10.22.1.4}"
[ "$USE_BUILD_SERVER" = "debian8" ] && BUILD_HOST="${BUILD_HOST:-10.22.1.5}"

log() { echo "[$(date +'%H:%M:%S')] $1"; }
err() { echo "[ERROR] $1" >&2; exit 1; }

cd "$REPO_ROOT"

# --- 1. Build on server and copy back ---
if [ "$BUILD_SERVER" = "1" ]; then
    log "Running full build on server (kernel + BDE + SDK + switchd)..."
    USE_BUILD_SERVER="${USE_BUILD_SERVER:-modern}" BUILD_KERNEL=1 ./scripts/build-on-build-server.sh
    REMOTE_DIR=$(ssh "${BUILD_USER}@${BUILD_HOST}" 'ls -td open-nos-as5610-build-* 2>/dev/null | head -1')
    [ -z "$REMOTE_DIR" ] && err "Could not find remote build dir on $BUILD_HOST"
    log "Copying artifacts from $REMOTE_DIR..."
    mkdir -p "$REPO_ROOT/boot" "$REPO_ROOT/bde" "$REPO_ROOT/build/sdk" "$REPO_ROOT/build/switchd"
    scp "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/linux-5.10/arch/powerpc/boot/uImage" "$REPO_ROOT/boot/uImage" 2>/dev/null || log "No uImage (kernel not built?)"
    scp "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/bde/"*.ko "$REPO_ROOT/bde/" 2>/dev/null || true
    scp "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/build/sdk/libbcm56846.so" "$REPO_ROOT/build/sdk/" 2>/dev/null || true
    scp "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/build/switchd/nos-switchd" "$REPO_ROOT/build/switchd/" 2>/dev/null || true
fi

# --- 2. DTB (required for FIT) ---
DTB_IMAGE="${DTB_IMAGE:-$REPO_ROOT/boot/as5610_52x.dtb}"
if [ -n "$CUMULUS_BIN" ] && [ -f "$CUMULUS_BIN" ]; then
    log "Extracting DTB from Cumulus image..."
    "$SCRIPT_DIR/extract-dtb.sh" "$CUMULUS_BIN" "$REPO_ROOT/boot/as5610_52x.dtb"
    DTB_IMAGE="$REPO_ROOT/boot/as5610_52x.dtb"
fi
if [ ! -f "$DTB_IMAGE" ]; then
    err "DTB required. Set DTB_IMAGE=<path> or CUMULUS_BIN=<Cumulus.bin>. See boot/README.md and scripts/extract-dtb.sh"
fi

# --- 3. Initramfs ---
INITRAMFS="${INITRAMFS:-$REPO_ROOT/initramfs/initramfs.cpio.gz}"
if [ "$SKIP_INITRAMFS" != "1" ]; then
    log "Building initramfs..."
    ( cd "$REPO_ROOT/initramfs" && ./build.sh ) || log "Initramfs build failed (need busybox?); will use existing if present"
fi
[ ! -f "$INITRAMFS" ] && err "Need initramfs at $INITRAMFS (run initramfs/build.sh or set INITRAMFS=<path>)"

# --- 4. FIT image ---
log "Building FIT image (kernel + DTB + initramfs)..."
KERNEL_IMAGE="${KERNEL_IMAGE:-$REPO_ROOT/boot/uImage}"
[ ! -f "$KERNEL_IMAGE" ] && err "Need kernel uImage at $KERNEL_IMAGE (run with BUILD_SERVER=1 or copy from build server)"
( cd "$REPO_ROOT/boot" && ./build-fit.sh "$KERNEL_IMAGE" "$DTB_IMAGE" "$INITRAMFS" )

# --- 5. Rootfs ---
if [ "$SKIP_ROOTFS" != "1" ]; then
    log "Building rootfs (debootstrap + our artifacts; may take a while)..."
    ( cd "$REPO_ROOT/rootfs" && ./build.sh ) || err "Rootfs build failed (need debootstrap, qemu-user-static, squashfs-tools on this host)"
fi
ROOTFS_SQUASH="${ROOTFS_SQUASH:-$REPO_ROOT/onie-installer/sysroot.squash.xz}"
[ ! -f "$ROOTFS_SQUASH" ] && err "Need rootfs at $ROOTFS_SQUASH (run rootfs/build.sh or set SKIP_ROOTFS=1 and provide one)"

# --- 6. ONIE .bin ---
log "Packaging ONIE installer .bin..."
KERNEL_FIT="$REPO_ROOT/boot/nos-powerpc.itb" ROOTFS_SQUASH="$ROOTFS_SQUASH" "$REPO_ROOT/onie-installer/build.sh"

OUT_BIN=$(ls -t "$REPO_ROOT/onie-installer/open-nos-as5610-"*.bin 2>/dev/null | head -1)
[ -n "$OUT_BIN" ] && [ -f "$OUT_BIN" ] || OUT_BIN="$REPO_ROOT/onie-installer/open-nos-as5610-$(date +%Y%m%d).bin"
log "Done. Loadable image: $OUT_BIN"
log "On Edgecore AS5610 (ONIE): onie-nos-install http://<server>/$(basename "$OUT_BIN")"
