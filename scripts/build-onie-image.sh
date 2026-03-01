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
#   SKIP_ROOTFS=1           Skip rootfs (kernel-only .bin). Default: build Debian rootfs locally or on server (Docker).
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
    # Fallback: build minimal DTB from .dts on build server (kernel tree has dtc)
    REMOTE_DIR=$(ssh "${BUILD_USER}@${BUILD_HOST}" 'ls -td open-nos-as5610-build-* 2>/dev/null | head -1')
    if [ -n "$REMOTE_DIR" ] && [ -f "$REPO_ROOT/boot/as5610_52x_minimal.dts" ]; then
        log "Building minimal DTB on server from boot/as5610_52x_minimal.dts..."
        scp "$REPO_ROOT/boot/as5610_52x_minimal.dts" "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/boot/"
        ssh "${BUILD_USER}@${BUILD_HOST}" "cd $REMOTE_DIR && ./linux-5.10/scripts/dtc/dtc -I dts -O dtb -p 0x3000 -o boot/as5610_52x.dtb boot/as5610_52x_minimal.dts 2>/dev/null" || true
        scp "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR}/boot/as5610_52x.dtb" "$REPO_ROOT/boot/as5610_52x.dtb" 2>/dev/null || true
    fi
    # Or build locally if dtc available
    if [ ! -f "$DTB_IMAGE" ] && command -v dtc &>/dev/null && [ -f "$REPO_ROOT/boot/as5610_52x_minimal.dts" ]; then
        log "Building minimal DTB locally..."
        dtc -I dts -O dtb -p 0x3000 -o "$REPO_ROOT/boot/as5610_52x.dtb" "$REPO_ROOT/boot/as5610_52x_minimal.dts"
    fi
fi
if [ ! -f "$DTB_IMAGE" ]; then
    err "DTB required. Set DTB_IMAGE=<path> or CUMULUS_BIN=<Cumulus.bin>. Or add boot/as5610_52x_minimal.dts and run with build server (or install dtc). See boot/README.md"
fi
# Pad DTB so U-Boot has room to add chosen node (bootargs, etc.); otherwise bootm can hang after "Loading Device Tree ... OK"
if [ -f "$DTB_IMAGE" ]; then
    if command -v dtc &>/dev/null; then
        log "Padding DTB for U-Boot fixups..."
        dtc -I dtb -O dtb -p 0x3000 -o "${DTB_IMAGE}.padded" "$DTB_IMAGE" && mv "${DTB_IMAGE}.padded" "$DTB_IMAGE"
    elif command -v docker &>/dev/null; then
        log "Padding DTB for U-Boot fixups (via Docker)..."
        DTB_DIR="$(cd "$(dirname "$DTB_IMAGE")" && pwd)" DTB_NAME="$(basename "$DTB_IMAGE")"
        if docker run --rm -v "$DTB_DIR:/d:rw" -w /d debian:bookworm bash -c "apt-get update -qq && apt-get install -y -qq device-tree-compiler >/dev/null && dtc -I dtb -O dtb -p 0x3000 -o ${DTB_NAME}.padded ${DTB_NAME} && mv ${DTB_NAME}.padded ${DTB_NAME}"; then
            : # padded
        else
            log "WARNING: DTB padding failed; boot may hang after 'Loading Device Tree ... OK'. Install dtc or Docker."
        fi
    else
        log "WARNING: dtc and Docker not found; DTB not padded. Boot may hang. Install device-tree-compiler or Docker."
    fi
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
if command -v mkimage &>/dev/null; then
    ( cd "$REPO_ROOT/boot" && ./build-fit.sh "$KERNEL_IMAGE" "$DTB_IMAGE" "$INITRAMFS" )
else
    REMOTE_DIR_FIT="open-nos-as5610-fit-$(date +%Y%m%d-%H%M%S)"
    ssh "${BUILD_USER}@${BUILD_HOST}" "mkdir -p $REMOTE_DIR_FIT"
    if [ -n "$REMOTE_DIR_FIT" ]; then
        log "mkimage not found locally; building FIT on server..."
        R="$REMOTE_DIR_FIT"
        ssh "${BUILD_USER}@${BUILD_HOST}" "mkdir -p ${R}/boot"
        scp "$KERNEL_IMAGE" "${BUILD_USER}@${BUILD_HOST}:${R}/boot/uImage"
        scp "$DTB_IMAGE" "${BUILD_USER}@${BUILD_HOST}:${R}/boot/as5610_52x.dtb"
        scp "$INITRAMFS" "${BUILD_USER}@${BUILD_HOST}:${R}/boot/initramfs.cpio.gz"
        scp "$REPO_ROOT/boot/build-fit.sh" "$REPO_ROOT/boot/nos.its" "${BUILD_USER}@${BUILD_HOST}:${R}/boot/"
        ssh "${BUILD_USER}@${BUILD_HOST}" "docker run --rm -v \$(pwd)/${R}:/work -w /work/boot debian:bookworm bash -c 'apt-get update -qq && apt-get install -y -qq u-boot-tools device-tree-compiler >/dev/null && ./build-fit.sh /work/boot/uImage /work/boot/as5610_52x.dtb /work/boot/initramfs.cpio.gz'"
        scp "${BUILD_USER}@${BUILD_HOST}:${R}/boot/nos-powerpc.itb" "$REPO_ROOT/boot/nos-powerpc.itb"
    fi
    [ ! -f "$REPO_ROOT/boot/nos-powerpc.itb" ] && err "FIT build failed (install u-boot-tools for mkimage or ensure build server has Docker)"
fi

# --- 5. Rootfs (Debian 12 Bookworm PPC32 per plan) ---
ROOTFS_SQUASH="${ROOTFS_SQUASH:-$REPO_ROOT/onie-installer/sysroot.squash.xz}"
if [ -f "$ROOTFS_SQUASH" ]; then
    log "Using existing rootfs: $ROOTFS_SQUASH"
elif [ "$SKIP_ROOTFS" = "1" ]; then
    log "SKIP_ROOTFS=1: .bin will contain kernel FIT only (no Debian rootfs)."
    ROOTFS_SQUASH=""
else
    # Try local rootfs build first (Linux with debootstrap, qemu-user-static, squashfs-tools)
    if command -v debootstrap &>/dev/null && { [ -x /usr/bin/qemu-ppc-static ] || [ -x /usr/bin/qemu-powerpc-static ]; }; then
        log "Building rootfs locally (Debian bookworm PPC32)..."
        ( cd "$REPO_ROOT/rootfs" && ./build.sh ) || true
    fi
    # If no rootfs yet, build on server in Docker (Debian jessie = last with PPC32; use archive.debian.org)
    if [ ! -f "$ROOTFS_SQUASH" ]; then
        log "Building rootfs on server (Debian jessie PPC32 in Docker)..."
        REMOTE_DIR_ROOTFS="open-nos-as5610-rootfs-$(date +%Y%m%d-%H%M%S)"
        ssh "${BUILD_USER}@${BUILD_HOST}" "mkdir -p $REMOTE_DIR_ROOTFS"
        log "Syncing repo to server for rootfs build..."
        rsync -az --exclude='.git' --exclude='rootfs/staging' \
            "$REPO_ROOT/" "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR_ROOTFS}/" || err "rsync to server failed"
        # Reuse build artifacts from latest kernel build if present (sdk, switchd, bde)
        LATEST_BUILD="$(ssh "${BUILD_USER}@${BUILD_HOST}" 'ls -td open-nos-as5610-build-* 2>/dev/null | head -1')"
        LATEST_BUILD="${LATEST_BUILD//[$'\r\n']}"
        if [ -n "$LATEST_BUILD" ]; then
            log "Copying build artifacts from $LATEST_BUILD..."
            ssh "${BUILD_USER}@${BUILD_HOST}" "cp -r ${LATEST_BUILD}/build ${LATEST_BUILD}/bde ${REMOTE_DIR_ROOTFS}/ 2>/dev/null || true"
        fi
        # Host needs qemu-user-static for binfmt (chroot runs PPC binaries); rootfs built in Docker
        ssh "${BUILD_USER}@${BUILD_HOST}" "sudo apt-get install -y -qq qemu-user-static 2>/dev/null || true"
        ssh "${BUILD_USER}@${BUILD_HOST}" "docker run --rm -v \$(pwd)/${REMOTE_DIR_ROOTFS}:/work -w /work debian:bookworm bash -c 'apt-get update -qq && apt-get install -y -qq debootstrap qemu-user-static squashfs-tools && REPO_ROOT=/work BUILD_DIR=/work/build ROOTFS_OUT=/work/onie-installer/sysroot.squash.xz ./rootfs/build.sh'" || err "Rootfs build on server failed (need Docker + qemu-user-static on $BUILD_HOST)"
        mkdir -p "$REPO_ROOT/onie-installer"
        scp "${BUILD_USER}@${BUILD_HOST}:${REMOTE_DIR_ROOTFS}/onie-installer/sysroot.squash.xz" "$REPO_ROOT/onie-installer/sysroot.squash.xz" || err "Could not copy sysroot.squash.xz from server"
        ROOTFS_SQUASH="$REPO_ROOT/onie-installer/sysroot.squash.xz"
    fi
    if [ ! -f "$ROOTFS_SQUASH" ]; then
        err "Could not build rootfs. Need: Linux host with debootstrap/qemu-user-static/squashfs-tools, or reachable build server with Docker. Set ROOTFS_SQUASH=<path> or SKIP_ROOTFS=1 for kernel-only .bin"
    fi
fi

# --- 6. ONIE .bin ---
log "Packaging ONIE installer .bin..."
KERNEL_FIT="$REPO_ROOT/boot/nos-powerpc.itb" ROOTFS_SQUASH="${ROOTFS_SQUASH:-}" "$REPO_ROOT/onie-installer/build.sh"

OUT_BIN=$(ls -t "$REPO_ROOT/onie-installer/open-nos-as5610-"*.bin 2>/dev/null | head -1)
[ -n "$OUT_BIN" ] && [ -f "$OUT_BIN" ] || OUT_BIN="$REPO_ROOT/onie-installer/open-nos-as5610-$(date +%Y%m%d).bin"
log "Done. Loadable image: $OUT_BIN"
log "On Edgecore AS5610 (ONIE): onie-nos-install http://<server>/$(basename "$OUT_BIN")"
