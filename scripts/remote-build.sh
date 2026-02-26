#!/bin/bash
#
# Runs on the BUILD SERVER. Do not run locally (except for testing in dry-run).
# Installs cross-toolchain if needed, optionally builds kernel, builds BDE.
#
# Env (set by build-on-build-server.sh or caller):
#   BUILD_KERNEL=1     Clone and build Linux 5.10 (slow)
#   KERNEL_SRC=/path   Use this kernel tree for BDE build
#

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

log() { echo "[$(date +'%H:%M:%S')] $1"; }
warn() { echo "[WARN] $1"; }

# If toolchain install fails on old distros (e.g. Debian 8),
# fall back to a modern Docker container build.
IN_DOCKER="${IN_DOCKER:-0}"
DOCKER_IMAGE="${DOCKER_IMAGE:-debian:bookworm}"

docker_fallback_build() {
    if [ "$IN_DOCKER" = "1" ]; then
        return 1
    fi
    if ! command -v docker &>/dev/null; then
        return 1
    fi

    log "Falling back to Docker build ($DOCKER_IMAGE)..."
    # Re-run this script inside container with a modern apt.
    docker run --rm \
        -e BUILD_KERNEL="${BUILD_KERNEL:-0}" \
        -e KERNEL_SRC="${KERNEL_SRC:-}" \
        -e IN_DOCKER=1 \
        -e DEBIAN_FRONTEND=noninteractive \
        -v "$REPO_ROOT:/work" \
        -w /work \
        "$DOCKER_IMAGE" \
        bash -lc "
          set -e
          apt-get update -qq
          apt-get install -y -qq --no-install-recommends \
            ca-certificates git make cmake \
            gcc g++ \
            gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu \
            bc libssl-dev libelf-dev flex bison \
            pkg-config file u-boot-tools
          ./scripts/remote-build.sh
        "
    exit $?
}

# --- Toolchain ---
log "Checking PPC32 cross-toolchain..."
if ! command -v powerpc-linux-gnu-gcc &>/dev/null; then
    log "Installing gcc-powerpc-linux-gnu and binutils..."
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update -qq
    sudo apt-get install -y -qq gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu \
        make bc libssl-dev libelf-dev flex bison || {
        warn "apt install failed (e.g. on Debian 8). Trying Docker fallback..."
        docker_fallback_build
        warn "Docker fallback unavailable; install manually: apt-get install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu"
        exit 1
    }
fi
GCC_VER=$(powerpc-linux-gnu-gcc -dumpversion 2>/dev/null || echo "?")
log "Using powerpc-linux-gnu-gcc $GCC_VER"

# --- Kernel (optional) ---
KERNEL_SRC="${KERNEL_SRC:-}"
BUILD_KERNEL="${BUILD_KERNEL:-0}"

if [ -n "$KERNEL_SRC" ] && [ -d "$KERNEL_SRC" ]; then
    log "Using existing kernel tree: $KERNEL_SRC"
elif [ "$BUILD_KERNEL" = "1" ]; then
    log "Cloning and building Linux 5.10 (this will take a long time)..."
    KERNEL_DIR="$REPO_ROOT/linux-5.10"
    if [ -d "$KERNEL_DIR" ] && [ ! -f "$KERNEL_DIR/arch/powerpc/configs/85xx/mpc85xx_cds_defconfig" ]; then
        log "Removing incomplete kernel tree..."
        rm -rf "$KERNEL_DIR"
    fi
    if [ ! -d "$KERNEL_DIR" ]; then
        git clone --depth 1 --branch v5.10 https://github.com/torvalds/linux.git "$KERNEL_DIR"
    fi
    cd "$KERNEL_DIR"
    # Linux 5.10+ uses mpc85xx_smp_defconfig (P2020 is 85xx); older had p2020rdb_defconfig
    DEFCONFIG=""
    for d in mpc85xx_smp_defconfig 85xx/mpc85xx_cds_defconfig p2020rdb_defconfig; do
        if [ -f "arch/powerpc/configs/$d" ]; then
            DEFCONFIG="$d"
            break
        fi
    done
    if [ -z "$DEFCONFIG" ]; then
        warn "No suitable powerpc defconfig found. List: ls arch/powerpc/configs/"
        exit 1
    fi
    log "Using defconfig: $DEFCONFIG"
    make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- "$DEFCONFIG"
    # Enable loadable modules (required for BDE .ko) and TUN, I2C, PCI, etc. for AS5610
    ./scripts/config -e CONFIG_MODULES -e CONFIG_TUN -e CONFIG_PCI -e CONFIG_I2C -e CONFIG_HWMON 2>/dev/null || true
    make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- olddefconfig
    make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- uImage modules -j$(nproc)
    make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- modules_prepare
    KERNEL_SRC="$KERNEL_DIR"
    cd "$REPO_ROOT"
else
    # Try common locations on build server
    for d in "$REPO_ROOT/linux-5.10" "$REPO_ROOT/linux" "/home/$(whoami)/linux-5.10"; do
        if [ -d "$d" ] && [ -f "$d/Makefile" ] && grep -q "powerpc" "$d/arch/powerpc/Makefile" 2>/dev/null; then
            KERNEL_SRC="$d"
            log "Found kernel tree: $KERNEL_SRC"
            break
        fi
    done
fi

# --- BDE ---
if [ -n "$KERNEL_SRC" ] && [ -d "$KERNEL_SRC" ] && [ -f "$KERNEL_SRC/Makefile" ]; then
    cd "$REPO_ROOT/bde"
    log "Building BDE modules (KERNEL_SRC=$KERNEL_SRC)..."
    make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
        -C "$KERNEL_SRC" \
        M="$(pwd)" \
        modules
    log "BDE build complete."
    ls -la nos_kernel_bde.ko nos_user_bde.ko 2>/dev/null || true
else
    warn "No kernel tree. Set KERNEL_SRC or BUILD_KERNEL=1 to build BDE modules. Skipping BDE."
fi

# --- SDK + switchd (PPC32 userspace) ---
cd "$REPO_ROOT"
if [ -f "CMakeLists.txt" ] && [ -d "sdk" ] && [ -d "switchd" ]; then
    if ! command -v cmake &>/dev/null; then
        log "Installing cmake..."
        export DEBIAN_FRONTEND=noninteractive
        sudo apt-get update -qq && sudo apt-get install -y -qq cmake || { warn "apt install cmake failed"; exit 1; }
    fi
    log "Building SDK (libbcm56846) and nos-switchd..."
    rm -rf build
    cmake -DCMAKE_TOOLCHAIN_FILE=tools/ppc32-toolchain.cmake -B build . || {
        warn "CMake configure failed"
        exit 1
    }
    cmake --build build -j$(nproc) || {
        warn "SDK/switchd build failed"
        exit 1
    }
    log "SDK + switchd + tests build complete."
    ls -la build/sdk/libbcm56846.so build/switchd/nos-switchd build/tests/bde_validate 2>/dev/null || true
else
    warn "No top-level CMakeLists.txt or sdk/switchd dirs; skipping SDK/switchd build."
fi
