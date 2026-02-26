#!/bin/bash
# Build Debian 12 (Bookworm) PPC32 rootfs and pack as squashfs for ONIE installer.
# Run on x86 build host with: debootstrap, qemu-user-static, squashfs-tools.
#
# Usage:
#   ./build.sh
#   REPO_ROOT=/path BUILD_DIR=/path/build KERNEL_VERSION=5.10.0 ./build.sh
#
# Optional: set BUILD_ARTIFACTS=0 to skip copying our binaries (rootfs-only for testing).
# Output: sysroot.squash.xz in onie-installer/ (or ROOTFS_OUT).

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
STAGING="${ROOTFS_STAGING:-$SCRIPT_DIR/staging}"
OUT_DIR="${ROOTFS_OUT_DIR:-$REPO_ROOT/onie-installer}"
OUT_FILE="${ROOTFS_OUT:-$OUT_DIR/sysroot.squash.xz}"
KERNEL_VERSION="${KERNEL_VERSION:-5.10.0}"
BUILD_ARTIFACTS="${BUILD_ARTIFACTS:-1}"

log() { echo "[rootfs/build] $1"; }

if ! command -v debootstrap &>/dev/null; then
	echo "Need debootstrap. Install: apt-get install debootstrap qemu-user-static squashfs-tools"
	exit 1
fi
if ! [ -x /usr/bin/qemu-ppc-static ] && ! [ -x /usr/bin/qemu-powerpc-static ]; then
	echo "Need qemu-user-static for PPC32 second-stage. Install: apt-get install qemu-user-static"
	exit 1
fi

QEMU_STATIC=""
for q in /usr/bin/qemu-ppc-static /usr/bin/qemu-powerpc-static; do
	[ -x "$q" ] && QEMU_STATIC="$q" && break
done
[ -z "$QEMU_STATIC" ] && { echo "qemu-ppc-static or qemu-powerpc-static not found"; exit 1; }

rm -rf "$STAGING"
mkdir -p "$STAGING"

# Stage 1: foreign debootstrap
log "debootstrap (foreign) bookworm powerpc..."
debootstrap --arch=powerpc --foreign bookworm "$STAGING" http://deb.debian.org/debian

# Stage 2: second-stage inside chroot via QEMU
cp "$QEMU_STATIC" "$STAGING/usr/bin/" 2>/dev/null || mkdir -p "$STAGING/usr/bin" && cp "$QEMU_STATIC" "$STAGING/usr/bin/"
chroot "$STAGING" /debootstrap/debootstrap --second-stage

# APT sources
log "Configuring APT..."
cat > "$STAGING/etc/apt/sources.list" <<'EOF'
deb http://deb.debian.org/debian bookworm main
deb http://security.debian.org/debian-security bookworm-security main
deb http://deb.debian.org/debian bookworm-updates main
EOF

# Install packages (minimal set for NOS)
log "Installing packages..."
chroot "$STAGING" apt-get update -qq
chroot "$STAGING" env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
	iproute2 openssh-server python3 ethtool tcpdump i2c-tools pciutils libpci3 \
	ca-certificates systemd-sysv 2>/dev/null || true
# Optional: frr, ifupdown2, lldpd (may not be in minimal bookworm)
chroot "$STAGING" env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
	frr ifupdown2 lldpd 2>/dev/null || true

# Our artifacts from build server or local build
if [ "$BUILD_ARTIFACTS" = "1" ]; then
	BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
	if [ -f "$BUILD_DIR/sdk/libbcm56846.so" ]; then
		log "Installing libbcm56846.so, nos-switchd, BDE modules..."
		mkdir -p "$STAGING/usr/lib" "$STAGING/usr/sbin" "$STAGING/lib/modules/$KERNEL_VERSION"
		cp -f "$BUILD_DIR/sdk/libbcm56846.so" "$STAGING/usr/lib/"
		[ -f "$BUILD_DIR/switchd/nos-switchd" ] && cp -f "$BUILD_DIR/switchd/nos-switchd" "$STAGING/usr/sbin/" && chmod +x "$STAGING/usr/sbin/nos-switchd"
		for ko in "$REPO_ROOT/bde/nos_kernel_bde.ko" "$REPO_ROOT/bde/nos_user_bde.ko"; do
			[ -f "$ko" ] && cp -f "$ko" "$STAGING/lib/modules/$KERNEL_VERSION/"
		done
	else
		log "BUILD_DIR ($BUILD_DIR) has no libbcm56846.so; copy build artifacts to staging or set BUILD_DIR."
	fi
fi

# Default config from repo
mkdir -p "$STAGING/etc/nos"
[ -f "$REPO_ROOT/etc/nos/config.bcm" ] && cp -f "$REPO_ROOT/etc/nos/config.bcm" "$STAGING/etc/nos/"
if [ ! -f "$STAGING/etc/nos/ports.conf" ]; then
	echo "# open-nos-as5610 default: 52 ports" > "$STAGING/etc/nos/ports.conf"
	echo "swp1=10G" >> "$STAGING/etc/nos/ports.conf"
fi

# Overlay files if present
OVERLAY="$SCRIPT_DIR/overlay"
if [ -d "$OVERLAY" ]; then
	log "Applying overlay..."
	cp -a "$OVERLAY"/* "$STAGING/"
fi

# Hostname
echo "as5610" > "$STAGING/etc/hostname"

log "Packing squashfs to $OUT_FILE..."
mkdir -p "$(dirname "$OUT_FILE")"
mksquashfs "$STAGING" "$OUT_FILE" -comp xz -noappend -no-duplicates
log "Done: $OUT_FILE"
