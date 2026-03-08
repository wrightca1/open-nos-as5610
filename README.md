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
| [`bde/`](bde/) | Our own BDE kernel modules (`nos-kernel-bde.ko`, `nos-user-bde.ko`) — PCI probe, BAR0 map, DMA pool, S-Channel transport | **Working on HW** |
| [`sdk/`](sdk/) | `libbcm56846` — custom SDK replacing Broadcom's proprietary SDK, written from RE data | **Working** (init, port, SerDes, L2, L3, ECMP, VLAN, pktio, stats) |
| [`switchd/`](switchd/) | `nos-switchd` — control plane daemon, netlink listener, TUN manager, packet I/O | **Building** |
| [`tests/`](tests/) | BDE validation test (`bde_validate`) — Phase 1d | **Passed on HW** |
| [`platform/`](platform/) | `platform-mgrd` — CPLD, thermal, fans, PSU, SFP/QSFP | **Working on HW** |
| [`rootfs/`](rootfs/) | Debian Jessie PPC32 rootfs — debootstrap + squashfs | **Working** |
| [`onie-installer/`](onie-installer/) | ONIE-compatible NOS installer for the AS5610-52X | **Working** |
| [`tools/`](tools/) | Diagnostic scripts (retimer init, SFP TX enable) + cmake toolchain | **Working** |

See [STATUS.md](STATUS.md) for detailed build and implementation status.

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

**Debian userspace.** Debian Jessie PPC32 (last Debian release with PowerPC support) provides an apt ecosystem — FRR, iproute2, openssh available. Our custom code (SDK, switchd, BDE) is installed directly into the rootfs.

**Minimal SDK surface.** `libbcm56846` implements only what's needed: init, port bringup, L2/L3 table programming, ECMP, and packet I/O. No feature creep.

---

## Status

This project is in **active development**. The reverse engineering phase is complete. Kernel, BDE modules, SDK skeleton, and nos-switchd skeleton build on our build server (PPC32 cross). See [STATUS.md](STATUS.md) for details.

Code phases:

- [x] Phase 0 — Build environment (cross-toolchain, kernel, repo structure)
- [x] Phase 1 — Kernel + BDE modules + validation test (passed on HW)
- [x] Phase 2 — libbcm56846 SDK (init, SCHAN, port, SerDes 10G, L2/L2_USER_ENTRY, L3/ECMP, VLAN, pktio, stats)
- [x] Phase 3 — nos-switchd (TUN, netlink, link state polling, TX/RX)
- [x] Phase 4 — FRR integration (installed from Debian packages)
- [x] Phase 5 — Platform management (thermal, fans, PSU, SFP, CPLD watchdog)
- [x] Phase 6 — ONIE installer (171MB .bin, boots on HW)
- [ ] HW validation — SerDes link-up with external 10G peers, L2/L3 forwarding tests

---

## Building

> **Prerequisites** (on an x86 Debian/Ubuntu build host):

```bash
apt-get install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu \
                qemu-user-static debootstrap u-boot-tools \
                squashfs-tools cmake
```

All building is done on the build server (10.22.1.5) inside a `debian:bookworm` Docker container for cross-compilation. Do NOT build on macOS.

### Full build (kernel + BDE + SDK + rootfs + installer)

```bash
# On build server, inside docker:
cd ~/open-nos-as5610
BUILD_KERNEL=1 ./scripts/remote-build.sh           # Kernel + BDE + SDK
IN_DOCKER=1 KERNEL_VERSION=5.10.0-nos \
  KERNEL_SRC=/work/linux-5.10 rootfs/build.sh      # Rootfs
boot/build-fit.sh                                   # FIT image
onie-installer/build.sh                             # ONIE installer .bin
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

The reverse engineering data lives in **`docs/reverse-engineering/`** in this repository — register layouts, table formats, initialization sequences, and captured traces. It may also be published at [wrightca1/edgecore-5610-reverse-engineering](https://github.com/wrightca1/edgecore-5610-reverse-engineering).

If you have an AS5610-52X and want to contribute implementation work, open an issue or pull request against this repo.

Areas that would benefit most from contribution:
- Device Tree Source (`.dts`) for the AS5610-52X on Linux 5.10
- initramfs init script
- libbcm56846 implementation (any module)
- nos-switchd implementation
- Testing on hardware
