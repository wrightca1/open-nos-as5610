# Root Filesystem — Debian 12 (Bookworm) PPC32

The rootfs is **Debian 12 (Bookworm)** bootstrapped for `powerpc` (PPC32 big-endian).
This matches the approach used by Cumulus (Debian 7) and ONL (Debian 8), but on a
current, supported Debian release.

## Why Debian

- Full PPC32 package archive — FRR, iproute2, lldpd, openssh, ethtool all install with `apt`
- Our components (SDK, switchd, platform-mgrd) ship as versioned `.deb` packages via `dpkg`
- Security patches via `apt upgrade` — no full rebuild required for CVE fixes
- Standard `debootstrap` + QEMU user-mode bootstrap is well-tested for PPC32

## Bootstrap Process (runs on x86 build host)

```bash
# 1. Install build-host dependencies
apt-get install gcc-powerpc-linux-gnu debootstrap qemu-user-static \
                squashfs-tools dpkg-dev

# 2. Stage 1: debootstrap (foreign, runs on x86)
debootstrap --arch=powerpc --foreign bookworm /mnt/rootfs \
    http://deb.debian.org/debian

# 3. Stage 2: complete inside QEMU PPC32 chroot
cp /usr/bin/qemu-ppc-static /mnt/rootfs/usr/bin/
chroot /mnt/rootfs /debootstrap/debootstrap --second-stage

# 4. Configure APT sources
cat > /mnt/rootfs/etc/apt/sources.list <<EOF
deb http://deb.debian.org/debian bookworm main
deb http://deb.debian.org/debian-security bookworm-security main
deb http://deb.debian.org/debian bookworm-updates main
EOF

# 5. Install NOS packages from Debian archive
chroot /mnt/rootfs apt-get update
chroot /mnt/rootfs apt-get install -y \
    frr \
    iproute2 \
    ifupdown2 \
    lldpd \
    openssh-server \
    python3 \
    ethtool \
    tcpdump \
    i2c-tools \
    pciutils \
    libpci3

# 6. Install our own cross-compiled .deb packages
dpkg --root=/mnt/rootfs -i \
    ../sdk/libbcm56846_*.deb \
    ../switchd/nos-switchd_*.deb \
    ../bde/nos-bde-modules_*.deb \
    ../platform/platform-mgrd_*.deb

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
| `frr` | Debian archive | `apt install frr` |
| `iproute2` | Debian archive | `apt install iproute2` |
| `ifupdown2` | Debian archive | `apt install ifupdown2` |
| `lldpd` | Debian archive | `apt install lldpd` |
| `openssh-server` | Debian archive | `apt install openssh-server` |
| `python3` | Debian archive | `apt install python3` |
| `ethtool` | Debian archive | `apt install ethtool` |
| `tcpdump` | Debian archive | `apt install tcpdump` |
| `i2c-tools` | Debian archive | `apt install i2c-tools` |
| `libbcm56846` | **our code** | cross-compiled .deb |
| `nos-switchd` | **our code** | cross-compiled .deb |
| `nos-bde-modules` | **our code** | cross-compiled .deb |
| `platform-mgrd` | **our code** | cross-compiled .deb |

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
