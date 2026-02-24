# open-nos-as5610

An open-source Network Operating System for the **Edgecore / Accton AS5610-52X** whitebox switch.

Built entirely from reverse-engineered hardware data and redistributable open-source components.
No Broadcom SDK, no Cumulus Linux code.

---

## Hardware

| | |
|--|--|
| **Platform** | Edgecore AS5610-52X (also Accton AS5610-52X) |
| **ASIC** | Broadcom BCM56846 (Trident+) — 52× 10GbE SFP+ + 4× 40GbE QSFP |
| **CPU** | Freescale P2020 (PowerPC e500v2, 32-bit big-endian) |
| **RAM** | 2 GB DDR3 |
| **Storage** | Internal USB flash (A/B partition layout) |
| **Bootloader** | ONIE + U-Boot (pre-installed on switch) |

---

## What This Is

A complete, from-scratch NOS stack that gives the AS5610-52X:

- **L2 switching** — hardware MAC learning, VLANs
- **L3 routing** — IPv4/IPv6 routing in ASIC at line rate, ECMP
- **Routing protocols** — BGP, OSPF, ISIS, BFD via FRRouting (FRR)
- **Platform management** — thermal, fans, PSU, SFP/QSFP via CPLD
- **ONIE installer** — installs from a factory-fresh switch; A/B upgrade support
- **Debian 12 userspace** — apt, openssh, standard Linux tools

The switch forwards packets in hardware (BCM56846 ASIC) with FRR handling routing protocols in software. The CPU only handles control traffic (ARP, BGP, OSPF, SSH).

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  User Space                                              │
│                                                          │
│  FRRouting (BGP/OSPF/ISIS)                               │
│       │ netlink                                          │
│  nos-switchd ──── libbcm56846 (our SDK)                  │
│       │                  │                               │
│  TUN devices          ioctl/mmap                         │
│  swp1..swp52                                             │
└───────────────────────────┼─────────────────────────────┘
                            │
┌───────────────────────────┼─────────────────────────────┐
│  Kernel                   ▼                              │
│  nos-kernel-bde.ko  (PCI, BAR0, DMA, S-Channel)         │
│  nos-user-bde.ko    (/dev/nos-bde character device)      │
└───────────────────────────┼─────────────────────────────┘
                            │ PCI / S-Channel DMA
                            ▼
                     BCM56846 ASIC
```

| Traffic direction | Path |
|------------------|------|
| Route/neighbor install | FRR → kernel FIB → netlink → nos-switchd → libbcm56846 → S-Channel DMA → ASIC |
| Packet TX (CPU→port) | app → TUN → nos-switchd → DMA → ASIC |
| Packet RX (port→CPU) | ASIC punt → DMA → nos-switchd → TUN write → kernel |

---

## Components

| Directory | What it is | Status |
|-----------|-----------|--------|
| [`bde/`](bde/) | Our own BDE kernel modules (`nos-kernel-bde.ko`, `nos-user-bde.ko`) — PCI probe, BAR0 map, DMA pool, S-Channel transport | Planning |
| [`sdk/`](sdk/) | `libbcm56846` — custom SDK replacing Broadcom's proprietary SDK, written from RE data | Planning |
| [`switchd/`](switchd/) | `nos-switchd` — control plane daemon, netlink listener, TUN manager, packet I/O | Planning |
| [`platform/`](platform/) | `platform-mgrd` — CPLD, thermal, fans, PSU, SFP/QSFP | Planning |
| [`rootfs/`](rootfs/) | Debian 12 (Bookworm) PPC32 rootfs — bootstrap scripts and package list | Planning |
| [`onie-installer/`](onie-installer/) | ONIE-compatible NOS installer for the AS5610-52X | Planning |

Reused open-source components (installed as Debian packages):

| Component | Purpose |
|-----------|---------|
| Linux 5.10 LTS (PPC32) | Kernel |
| FRRouting (FRR) | BGP, OSPF, ISIS, BFD |
| iproute2 / ifupdown2 | Interface management |
| lldpd | LLDP |
| OpenSSH | Remote access |

---

## Design Principles

**No proprietary code.** Every line we write is based on RE documentation from hardware we own. No Broadcom SDK source, no Cumulus Linux code.

**No OpenNSL BDE.** We write our own BDE kernel modules from scratch. OpenNSL BDE has kernel version compatibility issues. With the full register map from RE, writing our own is straightforward and gives us complete control.

**Debian userspace.** Debian 12 PPC32 provides a full apt ecosystem — FRR, iproute2, openssh all install from the standard archive. Our custom code (SDK, switchd, BDE) ships as versioned `.deb` packages.

**Minimal SDK surface.** `libbcm56846` implements only what's needed: init, port bringup, L2/L3 table programming, ECMP, and packet I/O. No feature creep.

---

## Status

This project is in **planning / early development**. The reverse engineering phase is complete — all ASIC register layouts, table formats, DMA paths, and initialization sequences have been documented from a live Cumulus Linux 2.5.1 switch.

Code phases:

- [ ] Phase 0 — Build environment (cross-toolchain, kernel, QEMU)
- [ ] Phase 1 — Kernel + DTB + initramfs + BDE modules
- [ ] Phase 2 — `libbcm56846` SDK (S-Channel, ASIC init, ports, L2, L3, packet I/O)
- [ ] Phase 3 — `nos-switchd` (netlink, TUN, link state polling)
- [ ] Phase 4 — Routing protocol integration (FRR + ECMP)
- [ ] Phase 5 — Platform management (CPLD, thermal, SFP)
- [ ] Phase 6 — ONIE installer + A/B upgrade

---

## Building

> **Prerequisites** (on an x86 Debian/Ubuntu build host):

```bash
apt-get install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu \
                qemu-user-static debootstrap u-boot-tools \
                squashfs-tools cmake
```

### Kernel

```bash
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- p2020rdb_defconfig
# enable CONFIG_TUN, CONFIG_GIANFAR, CONFIG_I2C_MUX, CONFIG_HWMON
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- uImage modules
```

### BDE modules

```bash
make -C bde ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
     KERNEL_SRC=/path/to/linux-5.10
```

### SDK + switchd

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=tools/ppc32-toolchain.cmake -B build sdk/
cmake --build build --target package
# produces libbcm56846_*.deb, nos-switchd_*.deb
```

### Rootfs + installer

```bash
cd rootfs && ./bootstrap.sh    # debootstrap Debian 12 PPC32
cd onie-installer && ./build.sh  # produces open-nos-as5610-YYYYMMDD.bin
```

### Install on switch

```bash
# From ONIE shell:
onie-nos-install http://your-server/open-nos-as5610.bin
```

---

## Documentation

- [PLAN.md](PLAN.md) — complete implementation plan with component specs, API surfaces, bit layouts, and implementation phases
- [bde/README.md](bde/README.md) — BDE module design, ioctl interface, register offsets
- [sdk/README.md](sdk/README.md) — SDK API, table layouts, S-Channel format
- [switchd/README.md](switchd/README.md) — nos-switchd design, netlink handlers, thread architecture
- [platform/README.md](platform/README.md) — CPLD, thermal, SFP interface
- [rootfs/README.md](rootfs/README.md) — Debian 12 bootstrap procedure, package list, filesystem layout
- [onie-installer/README.md](onie-installer/README.md) — installer format, partition layout, A/B upgrade

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Note: components pulled in as Debian packages (Linux, FRR, iproute2, etc.) carry their own licenses (GPL-2.0, LGPL, etc.). Our original code in this repository is Apache 2.0.

---

## Contributing

The reverse engineering data is published at **[wrightca1/edgecore-5610-reverse-engineering](https://github.com/wrightca1/edgecore-5610-reverse-engineering)** — all register layouts, table formats, initialization sequences, and captured traces are there.

If you have an AS5610-52X and want to contribute implementation work, open an issue or pull request against this repo.

Areas that would benefit most from contribution:
- Device Tree Source (`.dts`) for the AS5610-52X on Linux 5.10
- initramfs init script
- libbcm56846 implementation (any module)
- nos-switchd implementation
- Testing on hardware
