# Root Filesystem — Debian 8 (Jessie) PPC32

The rootfs is **Debian 8 (Jessie)** bootstrapped for `powerpc` (PPC32 big-endian).
Debian 8 is the last Debian release with an official PowerPC (PPC32 big-endian) port.
This matches the approach used by Cumulus (Debian 7) and ONL (Debian 8). The archive
is EOL but available at `archive.debian.org`.

## Why Debian 8 (Jessie)

- Last Debian release with official PPC32 big-endian port
- Full PPC32 package archive — iproute2, lldpd, openssh, ethtool all install with `apt`
- Our components (SDK, switchd, platform-mgrd) are installed directly into the rootfs
- Standard `debootstrap` + QEMU user-mode bootstrap is well-tested for PPC32
- Note: FRR is not available in jessie repos and must be cross-compiled separately

## Bootstrap Process (runs on x86 build host)

```bash
# 1. Install build-host dependencies
apt-get install gcc-powerpc-linux-gnu debootstrap qemu-user-static \
                squashfs-tools dpkg-dev

# 2. Stage 1: debootstrap (foreign, runs on x86)
debootstrap --arch=powerpc --foreign --no-check-gpg jessie /mnt/rootfs \
    http://archive.debian.org/debian

# 3. Stage 2: complete inside QEMU PPC32 chroot
cp /usr/bin/qemu-ppc-static /mnt/rootfs/usr/bin/
chroot /mnt/rootfs /debootstrap/debootstrap --second-stage

# 4. Configure APT sources (jessie is EOL; use archive.debian.org)
cat > /mnt/rootfs/etc/apt/sources.list <<EOF
deb http://archive.debian.org/debian jessie main
EOF
# Disable release date validation (jessie is EOL)
echo 'Acquire::Check-Valid-Until "false";' > \
    /mnt/rootfs/etc/apt/apt.conf.d/99ignore-release-date

# 5. Install NOS packages from Debian archive
chroot /mnt/rootfs apt-get update
chroot /mnt/rootfs apt-get install -y \
    iproute2 \
    isc-dhcp-client \
    lldpd \
    openssh-server \
    python3-minimal \
    ethtool \
    tcpdump \
    i2c-tools \
    pciutils \
    u-boot-tools \
    net-tools \
    curl
# Note: frr is NOT available in jessie repos (must be cross-compiled separately)

# 6. Install our cross-compiled artifacts directly
cp build/sdk/libbcm56846.so /mnt/rootfs/usr/lib/
cp build/switchd/nos-switchd /mnt/rootfs/usr/sbin/
cp bde/*.ko /mnt/rootfs/lib/modules/5.10.0-nos/

# 7. Configure systemd services, hostname, fstab, etc.
# (see rootfs/overlay/ for files copied in)

# 8. Pack to squashfs for ONIE installer
mksquashfs /mnt/rootfs ../onie-installer/sysroot.squash.xz \
    -comp xz -Xbcj powerpc -noappend
```

## Filesystem Layout on Target

```
squashfs (ro, mounted at /, /dev/sda6 or /dev/sda8):
  /usr/sbin/nos-switchd
  /usr/lib/libbcm56846.so
  /usr/sbin/frr/           (zebra, bgpd, ospfd, ...)
  /etc/nos/                (default config templates)
  /lib/modules/5.10.x/     (kernel modules incl. nos-kernel-bde.ko, nos-user-bde.ko)
  ...standard Debian tree...

overlay (rw, /dev/sda3 via overlayfs):
  /etc/frr/                (frr.conf, bgpd.conf, etc.)
  /etc/nos/ports.conf      (port speed configuration)
  /etc/nos/config.bcm      (ASIC config)
  /etc/network/interfaces  (ifupdown2 config)
  /var/log/

persist (/dev/sda1, ext2, mounted at /mnt/persist):
  SSH host keys
  Startup configuration backup
  License files (if any)
```

## Package List

| Package | Source | How |
|---------|--------|-----|
| `iproute2` | Debian jessie archive | `apt install iproute2` |
| `isc-dhcp-client` | Debian jessie archive | `apt install isc-dhcp-client` |
| `lldpd` | Debian jessie archive | `apt install lldpd` |
| `openssh-server` | Debian jessie archive | `apt install openssh-server` |
| `python3-minimal` | Debian jessie archive | `apt install python3-minimal` |
| `ethtool` | Debian jessie archive | `apt install ethtool` |
| `tcpdump` | Debian jessie archive | `apt install tcpdump` |
| `i2c-tools` | Debian jessie archive | `apt install i2c-tools` |
| `u-boot-tools` | Debian jessie archive | `apt install u-boot-tools` |
| `frr` | **not available** | Must be cross-compiled separately |
| `libbcm56846.so` | **our code** | cross-compiled, installed directly |
| `nos-switchd` | **our code** | cross-compiled, installed directly |
| `nos_kernel_bde.ko` | **our code** | cross-compiled, installed directly |
| `nos_user_bde.ko` | **our code** | cross-compiled, installed directly |
| `platform-mgrd` | **our code** | cross-compiled, installed directly |

## Init System

**systemd** — standard Debian default.

Boot order:
1. `nos-bde-modules.service` — load `nos-kernel-bde.ko`, `nos-user-bde.ko`
2. `platform-mgrd.service` — CPLD, thermal, fan, PSU
3. `nos-switchd.service` — ASIC init + TUN + netlink (after BDE ready)
4. `frr.service` — routing daemons (after switchd ready)

## Overlay Directory

```
rootfs/overlay/           # Files copied verbatim into the rootfs during build
├── etc/
│   ├── nos/
│   │   ├── config.bcm    # Default ASIC config
│   │   └── ports.conf    # Default: all ports 10G
│   ├── systemd/system/
│   │   ├── nos-switchd.service
│   │   ├── nos-bde-modules.service
│   │   └── platform-mgrd.service
│   └── fstab             # sda1→/mnt/persist, sda3 overlay
└── usr/
    └── share/nos/
        └── rc.forwarding  # ASIC forwarding defaults
```
