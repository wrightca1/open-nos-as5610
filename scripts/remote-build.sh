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
            gcc-powerpc-linux-gnu g++-powerpc-linux-gnu binutils-powerpc-linux-gnu \
            libc6-dev-powerpc-cross \
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
    # Storage drivers required for USB flash boot (all built-in, not modules)
    ./scripts/config \
        -e CONFIG_BLK_DEV_SD \
        -e CONFIG_USB_STORAGE \
        -e CONFIG_USB_EHCI_HCD \
        -e CONFIG_USB_EHCI_FSL \
        -e CONFIG_SQUASHFS \
        -e CONFIG_MSDOS_PARTITION \
        -e CONFIG_OVERLAY_FS \
        2>/dev/null || true
    # Management Ethernet (P2020 eTSEC → eth0); without this no SSH after install
    ./scripts/config \
        -e CONFIG_NET_VENDOR_FREESCALE \
        -e CONFIG_GIANFAR \
        2>/dev/null || true
    # I2C: chardev (/dev/i2c-*), mux support, PCA954x (PCA9548/PCA9546 on AS5610)
    # GPIO: PCA953x (pca9506/pca9538 I/O expanders for SFP presence/LEDs)
    ./scripts/config \
        -e CONFIG_I2C_CHARDEV \
        -e CONFIG_I2C_MUX \
        -e CONFIG_I2C_MUX_PCA954X \
        -e CONFIG_GPIO_PCA953X \
        2>/dev/null || true
    # Hwmon: LM75 (board temp sensors on i2c-9), NE1617A/LM90 (ASIC+board)
    # AT24 EEPROM driver: binds to SFP 0x50 → /sys/class/eeprom_dev/eepromN/device/eeprom
    ./scripts/config \
        -e CONFIG_SENSORS_LM75 \
        -e CONFIG_SENSORS_LM90 \
        -e CONFIG_EEPROM_AT24 \
        2>/dev/null || true

    # Apply AS5610-52X machine description (fixes "No suitable machine description found")
    # The DTB compatible "accton,as5610_52x" has no match in mainline Linux 5.10.
    PATCH_DIR="$REPO_ROOT/boot/kernel-patches"
    PLAT_85XX="arch/powerpc/platforms/85xx"
    if [ -f "$PATCH_DIR/$PLAT_85XX/accton_as5610_52x.c" ]; then
        log "Applying AS5610-52X machine description patch..."
        cp "$PATCH_DIR/$PLAT_85XX/accton_as5610_52x.c" "$PLAT_85XX/"
        # Add Kconfig entry if not already present
        if ! grep -q "ACCTON_AS5610_52X" "$PLAT_85XX/Kconfig"; then
            sed -i '/^endif # FSL_SOC_BOOKE/i\
\
config ACCTON_AS5610_52X\
\tbool "Accton/Edgecore AS5610-52X"\
\tselect DEFAULT_UIMAGE\
\thelp\
\t  Support for the Accton/Edgecore AS5610-52X switching platform.\
\t  Matches DTB compatible strings "accton,as5610_52x" and "accton,5652".\
' "$PLAT_85XX/Kconfig"
        fi
        # Add Makefile entry if not already present
        if ! grep -q "ACCTON_AS5610_52X" "$PLAT_85XX/Makefile"; then
            echo 'obj-$(CONFIG_ACCTON_AS5610_52X) += accton_as5610_52x.o' >> "$PLAT_85XX/Makefile"
        fi
        # Enable in config
        ./scripts/config -e CONFIG_ACCTON_AS5610_52X 2>/dev/null || \
            echo "CONFIG_ACCTON_AS5610_52X=y" >> .config
        log "AS5610-52X machine description patch applied."
    else
        warn "Kernel patch not found at $PATCH_DIR/$PLAT_85XX/accton_as5610_52x.c -- skipping."
        warn "Without this patch the kernel will panic: 'No suitable machine description found'"
    fi

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
# Compatibility requirement: nos-switchd and libbcm56846.so must link against the
# same glibc that's in the rootfs. Set PPC32_SYSROOT to rootfs staging if available.
# rootfs/build.sh must run first (or set PPC32_SYSROOT manually) for exact matching.
cd "$REPO_ROOT"
if [ -f "CMakeLists.txt" ] && [ -d "sdk" ] && [ -d "switchd" ]; then
    if ! command -v cmake &>/dev/null; then
        log "Installing cmake..."
        export DEBIAN_FRONTEND=noninteractive
        sudo apt-get update -qq && sudo apt-get install -y -qq cmake || { warn "apt install cmake failed"; exit 1; }
    fi

    # Use rootfs sysroot if built; otherwise cross-compile against default headers
    ROOTFS_STAGING="${REPO_ROOT}/rootfs/staging"
    if [ -d "$ROOTFS_STAGING/lib" ]; then
        export PPC32_SYSROOT="$ROOTFS_STAGING"
        log "Using rootfs sysroot for libc compatibility: $PPC32_SYSROOT"
    else
        warn "rootfs/staging not found. Run rootfs/build.sh first for exact libc matching."
        warn "Building against default cross-sysroot (may have glibc version mismatch)."
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

# --- Version matrix check ---
log "Version matrix:"
[ -n "$KERNEL_SRC" ] && log "  Kernel: $(make -s -C "$KERNEL_SRC" kernelversion 2>/dev/null || echo '?')"
log "  GCC:    $(powerpc-linux-gnu-gcc -dumpversion 2>/dev/null || echo '?')"
if [ -f "$ROOTFS_STAGING/lib/libc.so.6" ] || [ -f "$ROOTFS_STAGING/lib/powerpc-linux-gnu/libc.so.6" ]; then
    LIBC_VER=$(chroot "$ROOTFS_STAGING" /lib/libc.so.6 --version 2>/dev/null | head -1 || echo '?')
    log "  Target glibc: $LIBC_VER"
fi
[ -f "$REPO_ROOT/bde/nos_kernel_bde.ko" ] && \
    log "  BDE vermagic: $(modinfo -F vermagic "$REPO_ROOT/bde/nos_kernel_bde.ko" 2>/dev/null || echo '?')"
