# open-nos-as5610 — Master Implementation Plan

**Target**: Edgecore AS5610-52X (also Accton AS5610-52X)
**ASIC**: Broadcom BCM56846 (Trident+), 52× 10GbE + 4× 40GbE
**CPU**: PowerPC e500v2 (Freescale P2020), PPC32 big-endian
**Goal**: A fully functional, redistributable open-source NOS delivering Cumulus-equivalent L2/L3 switching — using **only** our reverse-engineered data and OSS components we can legally redistribute. Zero Broadcom proprietary SDK or Cumulus code.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [What We Reuse vs What We Write](#2-what-we-reuse-vs-what-we-write)
3. [Component Details](#3-component-details)
4. [Implementation Phases](#4-implementation-phases)
5. [SDK Design — BCM56846 API Surface](#5-sdk-design--bcm56846-api-surface)
6. [Control Plane Daemon Design](#6-control-plane-daemon-design)
7. [Packet I/O Design](#7-packet-io-design)
8. [Routing Protocol Integration](#8-routing-protocol-integration)
9. [Platform Management](#9-platform-management)
10. [ONIE Installer](#10-onie-installer)
11. [Build System](#11-build-system)
12. [Testing Strategy](#12-testing-strategy)
13. [Known Gaps and Risk Register](#13-known-gaps-and-risk-register)
14. [Reference Map to RE Docs](#14-reference-map-to-re-docs)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│  User Space                                                          │
│                                                                      │
│  ┌──────────┐  ┌────────────────────┐  ┌──────────────────────────┐ │
│  │  FRRouting│  │  nos-switchd       │  │  platform-mgrd           │ │
│  │  (FRR)   │  │  (our switchd      │  │  (CPLD, thermal,         │ │
│  │  BGP/OSPF│  │   replacement)     │  │   fans, SFP/QSFP)        │ │
│  │  /ISIS   │  │                    │  │                          │ │
│  └────┬─────┘  │  netlink listener  │  └──────────────────────────┘ │
│       │        │  TUN manager (swp) │                               │
│       │netlink │  SDK caller        │                               │
│       │        │                    │                               │
│       │        │  ┌──────────────┐  │                               │
│       │        │  │ libbcm56846  │  │  ← Our custom SDK             │
│       │        │  │ (our SDK)    │  │                               │
│       │        │  └──────┬───────┘  │                               │
│       │        └─────────┼──────────┘                               │
│                          │                                           │
│                          │ ioctl / mmap                              │
└──────────────────────────┼───────────────────────────────────────────┘
                           │
┌──────────────────────────┼───────────────────────────────────────────┐
│  Kernel Space            │                                            │
│                          ▼                                            │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │  nos-kernel-bde.ko   (our BDE — PCI probe, BAR, DMA, S-Chan)  │    │
│  │  nos-user-bde.ko     (our BDE — /dev/nos-bde userspace iface)│    │
│  │                                                               │    │
│  │  /dev/nos-bde                                                 │    │
│  └───────────────────────────────┬───────────────────────────────┘    │
│                                  │                                    │
│  ┌───────────────────────────────┼───────────────────────────────┐    │
│  │  TUN driver (tun.ko)          │  platform i2c/cpld drivers    │    │
│  │  /dev/net/tun → swp1..swp52   │  (existing kernel drivers)    │    │
│  └───────────────────────────────┴───────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────┘
                           │
                           │ PCI BAR0 DMA / S-Channel
                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│  BCM56846 ASIC (hardware)                                            │
│  BAR0 phys 0xa0000000                                                │
│  L2_ENTRY, L3_DEFIP, ECMP, ING/EGR nexthop, EGR_L3_INTF tables      │
│  XLPORT/MAC blocks, Warpcore WC-B0 SerDes                            │
└──────────────────────────────────────────────────────────────────────┘
```

### Data Flow Summary

| Direction | Path |
|-----------|------|
| **Table write** (route/neigh/link) | netlink → nos-switchd → libbcm56846 → S-Channel DMA → ASIC |
| **Packet TX** (CPU → port) | app → kernel → TUN fd → nos-switchd → libbcm56846 pktio → BDE DMA → ASIC |
| **Packet RX** (port → CPU) | ASIC punt → BDE DMA → libbcm56846 RX callback → nos-switchd → TUN write → kernel |
| **Routing** | FRR computes best path → kernel route table → RTM_NEWROUTE netlink → nos-switchd programs ASIC FIB |

---

## 2. What We Reuse vs What We Write

### Reused (OSS, redistributable)

| Component | Source | License | Notes |
|-----------|--------|---------|-------|
| **Linux kernel** | upstream kernel.org | GPL-2.0 | PPC32 big-endian, cross-compiled; must support tun.ko, i2c, PCI |
| **Debian 12 (Bookworm) PPC32** | debian.org | Various (all OSS) | Base rootfs — `debootstrap --arch powerpc`; provides dpkg/apt, libc, all userspace |
| **FRRouting (FRR)** | github.com/FRRouting/frr | GPL-2.0 / LGPL | BGP, OSPF, ISIS, static, BFD — installed as Debian package |
| **iproute2** | kernel.org | GPL-2.0 | Debian package |
| **ifupdown2** | github.com/CumulusNetworks/ifupdown2 | GPL-2.0 | Interface configuration — Debian package |
| **lldpd** | github.com/lldpd/lldpd | ISC | LLDP — Debian package |
| **ONIE** | github.com/opencomputeproject/onie | GPL-2.0 | Bootloader framework (already on switch) |
| **u-boot** | u-boot.org | GPL-2.0 | Already on switch; we only need fw_setenv |
| **OpenNetworkLinux (ONL)** | github.com/opennetworklinux/ONL | Apache 2.0 | Reference for ONLP platform code |
| **ONLP** | ONL project | Apache 2.0 | Platform abstraction library (thermal/PSU/fan/SFP) |

### We Write (our code, from RE docs)

| Component | What it is | Key RE Docs |
|-----------|-----------|-------------|
| **nos-kernel-bde.ko** | Kernel BDE module — PCI probe, BAR map, DMA pool, S-Channel transport | ASIC_INIT_AND_DMA_MAP.md, BDE_CMIC_REGISTERS.md |
| **nos-user-bde.ko** | User-space BDE module — `/dev/nos-bde` character device | ASIC_INIT_AND_DMA_MAP.md, WRITE_MECHANISM_ANALYSIS.md |
| **libbcm56846** | Custom SDK — replaces the Broadcom proprietary SDK and libopennsl | All ASIC docs |
| **nos-switchd** | Control plane daemon — replaces Cumulus switchd | netlink-handlers.md, api-patterns.md |
| **platform-mgrd** | Platform management (CPLD, thermal, SFP) — or integrate into ONLP | PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md |
| **onie-installer** | ONIE-compatible NOS installer | ONIE_BOOT_AND_PARTITION_LAYOUT.md |
| **nos-config** | Port/interface configuration tooling (ports.conf equivalent) | SDK_AND_ASIC_CONFIG_FROM_SWITCH.md |

---

## 3. Component Details

### 3.1 BDE Kernel Modules (Written by Us)

We write our own BDE kernel modules from scratch. OpenNSL BDE has proven kernel version compatibility issues and carries its own license complications. We have all the hardware data needed from reverse engineering.

**nos-kernel-bde.ko** — kernel module responsibilities:
- PCI device probe: match PCI vendor/device `0x14e4:0xb846` (BCM56846)
- Map BAR0 (`0xa0000000`, 256KB) via `ioremap()`
- Allocate DMA pool: `dma_alloc_coherent(dev, 8MB, &dma_handle, GFP_KERNEL)` + sub-allocator
- Register interrupt handler (IRQ assigned by U-Boot/PCI, confirm via `lspci -v`)
- Expose ioctl interface to `nos-user-bde.ko`

**nos-user-bde.ko** — character device `/dev/nos-bde`:
- `NOS_BDE_READ_REG(offset, &val)` — read 32-bit BAR0 register
- `NOS_BDE_WRITE_REG(offset, val)` — write 32-bit BAR0 register
- `NOS_BDE_GET_DMA_INFO(&pbase, &size)` — return DMA pool physical address + size
- `NOS_BDE_MMAP` — user-space maps DMA pool (via `vm_pgoff = dma_pbase >> PAGE_SHIFT`)
- `NOS_BDE_SCHAN_OP(&cmd)` — submit S-Channel command + wait for completion

**Why writing our own is straightforward**: The BDE is a thin PCI + DMA abstraction. We have complete register offsets, DMA layout, and the exact ioctl interface (captured from strace on Cumulus). There is no ASIC-specific logic in the BDE itself — all ASIC logic is in our SDK.

**Compile target**: PPC32 big-endian, targets whichever kernel version we choose (Linux 5.10 LTS recommended). No version compatibility concerns since we control the source.

**Key confirmed register data** (from RE):
```
BAR0 physical:           0xa0000000
BAR0 size:               256KB (0x40000)
CMIC_CMC0_SCHAN_CTRL:    BAR0 + 0x32800  ← S-Channel control
CMICM_CMC_BASE:          BAR0 + 0x31000
CMICM_DMA_CTRL(ch):      BAR0 + 0x31140 + 4*ch
CMICM_DMA_DESC0(ch):     BAR0 + 0x31158 + 4*ch
CMICM_DMA_HALT_ADDR(ch): BAR0 + 0x31120 + 4*ch
CMIC_MIIM_PARAM:         BAR0 + 0x00000158  ← SerDes MDIO
CMIC_MIIM_ADDRESS:       BAR0 + 0x000004a0
```

See: `../docs/reverse-engineering/ASIC_INIT_AND_DMA_MAP.md`, `../docs/reverse-engineering/BDE_CMIC_REGISTERS.md`, `../docs/reverse-engineering/WRITE_MECHANISM_ANALYSIS.md`

### 3.2 libbcm56846 — Our Custom SDK

This is the heart of the project. It is a C library that provides the control-plane API needed by nos-switchd to program the BCM56846 ASIC.

**It does NOT need to replicate the full Broadcom SDK.** It only needs the subset of operations that make the switch forward packets and route:

| Module | API Surface (our names) | Status of RE Data |
|--------|------------------------|-------------------|
| **init** | `bcm56846_attach()`, `bcm56846_init()`, rc.soc sequencer | ✅ Init sequence fully documented |
| **port** | `bcm56846_port_enable_set()`, `bcm56846_port_speed_set()`, `bcm56846_port_link_status_get()` | ✅ XLPORT/MAC regs + SerDes init sequence documented |
| **l2** | `bcm56846_l2_addr_add()`, `bcm56846_l2_addr_delete()`, `bcm56846_l2_addr_get()` | ✅ L2_ENTRY + L2_USER_ENTRY bit layouts verified on live switch |
| **l3** | `bcm56846_l3_egress_create()`, `bcm56846_l3_route_add()`, `bcm56846_l3_route_delete()`, `bcm56846_l3_host_add()` | ✅ Full L3/ECMP/nexthop chain verified on live switch |
| **ecmp** | `bcm56846_l3_ecmp_create()`, `bcm56846_l3_ecmp_destroy()` | ✅ L3_ECMP + L3_ECMP_GROUP format verified |
| **vlan** | `bcm56846_vlan_create()`, `bcm56846_vlan_port_add()`, `bcm56846_vlan_destroy()` | ✅ VLAN (ingress 0x12168000, 4096×40B) + EGR_VLAN (egress 0x0d260000, 4096×29B) fully verified. PORT_BITMAP + ING_PORT_BITMAP + UT_PORT_BITMAP bit positions confirmed. [VLAN_TABLE_FORMAT.md](../docs/reverse-engineering/VLAN_TABLE_FORMAT.md) |
| **pktio** | `bcm56846_tx()`, `bcm56846_rx_register()`, `bcm56846_rx_start()` | ✅ DCB type 21 (16 words, 64 bytes), TX LOCAL_DEST_PORT encoding, RX metadata layout, BDE ioctls (WAIT_FOR_INTERRUPT/SEM_OP), thread model all verified. [PKTIO_BDE_DMA_INTERFACE.md](../docs/reverse-engineering/PKTIO_BDE_DMA_INTERFACE.md) |
| **schan** | Internal: `schan_write()`, `schan_read()` | ✅ S-Channel DMA path + command word format documented |
| **serdes** | Internal: `wc_b0_init_10g()`, `wc_b0_mdio_write()` | ✅ Full Warpcore WC-B0 MDIO init sequence captured via GDB |
| **stats** | `bcm56846_stat_get()` | ✅ XLMAC counter register offsets (RPKT/RBYT/TPKT/TBYT/R64/T64 etc), S-Channel address formula, port→block/lane mapping all verified. [STATS_COUNTER_FORMAT.md](../docs/reverse-engineering/STATS_COUNTER_FORMAT.md) |
| **ipv6** | `bcm56846_l3_route_add()` IPv6, `bcm56846_l3_host_add()` IPv6 | ✅ L3_DEFIP_128 (/128 TCAM, 0x0a176000, 256×39B), L3_DEFIP double-wide (LPM ≤/64), L3_ENTRY_IPV6_UNICAST (unused) — all verified. [L3_IPV6_FORMAT.md](../docs/reverse-engineering/L3_IPV6_FORMAT.md) |

**Key design decision**: Our SDK calls into our own BDE via the `NOS_BDE_*` ioctl interface. We open `/dev/nos-bde`, use `NOS_BDE_READ_REG`, `NOS_BDE_WRITE_REG`, `NOS_BDE_GET_DMA_INFO`, and `NOS_BDE_SCHAN_OP` ioctls, and access the DMA pool via `mmap`. See `bde/README.md` for the full ioctl spec.

### 3.3 nos-switchd — Control Plane Daemon

Replaces Cumulus `switchd`. Written in C. Responsibilities:

1. **Startup**: Load port configuration from `ports.conf`, build port-to-BCM-port map.
2. **TUN creation**: Open `/dev/net/tun`, `ioctl(TUNSETIFF)` for each port (`swp1..swp52`, breakout `swp49s0..swp49s3`, etc.).
3. **SDK init**: Call `bcm56846_attach()`, `bcm56846_init()`, run SOC script sequences.
4. **Netlink listener**: Subscribe to `RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_NEIGH | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR`; dispatch to SDK calls.
5. **Link state polling loop**: RTM_NEWLINK fires only on admin-state changes, not physical link transitions. A dedicated poll thread calls `bcm56846_port_link_status_get()` every ~200 ms for all ports and synthesizes link-up/down events when physical link changes, triggering neighbor flush and FRR convergence.
6. **Packet I/O**: Read from TUN fds (TX path) and write to TUN fds (RX path).

Netlink event → SDK call mapping:

| Netlink Event | nos-switchd Action | SDK Call |
|---------------|-------------------|----------|
| `RTM_NEWLINK` (IFF_UP) | Port admin up | `bcm56846_port_enable_set(port, 1)` |
| `RTM_NEWLINK` (IFF_DOWN) | Port admin down | `bcm56846_port_enable_set(port, 0)` |
| `RTM_NEWADDR` | IP assigned to swp/SVI → create L3 intf | `bcm56846_l3_intf_create()` → write `EGR_L3_INTF` (SA_MAC + VLAN) |
| `RTM_DELADDR` | IP removed → destroy L3 intf if last user | `bcm56846_l3_intf_destroy()` |
| `RTM_NEWROUTE` | Add L3 route; resolve nexthop; create egress if needed | `bcm56846_l3_egress_create()` → `bcm56846_l3_route_add()` |
| `RTM_DELROUTE` | Remove L3 route | `bcm56846_l3_route_delete()` |
| `RTM_NEWNEIGH` | Learn/update L2/L3 neighbor | `bcm56846_l2_addr_add()` and/or `bcm56846_l3_host_add()` |
| `RTM_DELNEIGH` | Remove neighbor | `bcm56846_l2_addr_delete()` |

**RTM_NEWADDR / SVI creation**: When the kernel assigns an IP to an interface (`ip addr add 10.0.0.1/24 dev swp1`), we receive `RTM_NEWADDR`. We must create an `EGR_L3_INTF` entry containing the interface's MAC (from the preceding `RTM_NEWLINK` for that ifindex) and VLAN. This entry is referenced by all egress next-hop objects that exit this interface. Without `RTMGRP_IPV4_IFADDR` in the netlink subscription, L3 routing silently fails even when routes are installed correctly.

See: `../docs/reverse-engineering/netlink-handlers.md`, `../docs/reverse-engineering/api-patterns.md`

### 3.4 platform-mgrd — Platform Management

Wraps the CPLD, thermal, fan, PSU, and SFP/QSFP interfaces. Can be integrated with ONLP (Open Network Linux Platform library) which already supports this switch.

Responsibilities:
- Read temperature sensors (10 sensors via sysfs/hwmon)
- Read/write fan PWM (via CPLD sysfs)
- Read PSU status (via CPLD sysfs)
- Control status LEDs (via CPLD sysfs + ASIC LED program)
- SFP/QSFP EEPROM access (via i2c-22..i2c-73)
- Watchdog management

If using ONLP: the `onlp_platform_init()` for accton_as5610_52x is available in the ONL repository and handles most of this.

See: `../docs/reverse-engineering/PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md`, `../docs/reverse-engineering/SFP_TURNUP_AND_ACCESS.md`

### 3.5 Management Interface (eth0)

The AS5610-52X CPU (Freescale P2020) has a dedicated out-of-band management Ethernet port driven by the P2020's eTSEC (enhanced Three-Speed Ethernet Controller). This is completely separate from the BCM56846 switch ports. It appears as `eth0` in Linux and is the primary SSH/management interface.

**This interface is not mentioned elsewhere in the original plan and must be explicitly supported:**

- **Driver**: `gianfar` (Freescale eTSEC driver) — included in mainline Linux PPC32 builds with `CONFIG_GIANFAR=y`
- **DTB**: The eTSEC PHY node must appear in the Device Tree (`enet0`, `enet1` nodes on P2020); correct MAC address from EEPROM or U-Boot `ethaddr` variable
- **Kernel config**: `CONFIG_GIANFAR=y`, `CONFIG_NET_VENDOR_FREESCALE=y`
- **ifupdown2 config**: `/etc/network/interfaces` entry for `eth0` (DHCP or static)
- **Firewall**: SSH access restricted to eth0 by default; swp interfaces for transit/routing only

Without eth0, the switch has no management access after install (no console on most rack deployments).

---

## 4. Implementation Phases

### Phase 0 — Build Environment

- [ ] Set up PPC32 big-endian cross-compilation toolchain (Buildroot or Yocto)
- [ ] Verify toolchain produces PPC32 BE binaries: `powerpc-linux-gnu-gcc` or `powerpc-buildroot-linux-gnu-gcc`
- [ ] Build a minimal PPC32 Linux kernel (4.19 LTS or 5.10 LTS) with: `TUN`, `I2C`, `PCI`, `SFP` support
- [ ] Confirm BDE modules compile against chosen kernel version
- [ ] Set up QEMU PPC32 VM for software-only testing (before hardware)
- [ ] Create git repo structure (this directory)

**Deliverable**: Cross-compilation toolchain, kernel build, BDE modules that load on target.

### Phase 1 — Boot + Kernel + Our BDE

#### 1a — Kernel + DTB + initramfs + Basic Boot
- [x] Build Linux 5.10 LTS kernel for PPC32 e500v2 target with required `CONFIG_*` options
- [ ] **Device Tree Blob (DTB)**: Obtain or build the P2020-based DTB for the AS5610-52X. The FIT image (`uImage-powerpc.itb`) must bundle `kernel + dtb + initramfs` using `mkimage -f`. Without a correct DTB, Linux cannot enumerate the P2020 SoC peripherals (eTSEC eth0, I2C buses, CPLD local bus, PCIe). Reference: ONL tree has `as5610_52x.dts`; alternatively extract from Cumulus FIT image with `dumpimage -l`.
- [x] **initramfs**: Build a minimal initramfs that mounts the squashfs rootfs and overlayfs before `pivot_root`. (`initramfs/build.sh` + `init` script; busybox, mount, switch_root; mount sda6 squashfs + sda3 overlay → switch_root)
- [x] Pack FIT image: `boot/build-fit.sh` → `nos-powerpc.itb` (nos.its references kernel, dtb, initramfs)
- [ ] Boot via ONIE (temporary minimal installer) or test netboot
- [ ] Confirm PCI device visible: `lspci | grep 14e4:b846`
- [ ] Load `tun.ko`, create a test TUN device, confirm kernel networking works
- [ ] Confirm `eth0` comes up (P2020 eTSEC — management interface, see §3.5)

#### 1b — Write nos-kernel-bde.ko
- [x] PCI probe: match `PCI_VENDOR_ID_BROADCOM, 0xb846`; call `pci_enable_device()`, `pci_request_regions()`
- [x] BAR0 map: `ioremap(pci_resource_start(dev, 0), 256*1024)` → confirm VA reads back `0xa0000000`
- [x] DMA pool: `dma_alloc_coherent(8MB)` → store `_dma_vbase`, `_dma_pbase`; wrap with slab sub-allocator
- [x] Interrupt: `request_irq(pdev->irq, nos_bde_irq, IRQF_SHARED, ...)`; stub handler for now
- [x] S-Channel submit: write command to DMA buffer; write `CMICM_DMA_DESC0`; write `CMICM_DMA_CTRL |= START`; wait for IRQ or poll `CMICM_DMA_STAT`
- [x] Export symbols for `nos-user-bde.ko`

#### 1c — Write nos-user-bde.ko
- [x] Create char device `/dev/nos-bde` via `cdev_init()` + `cdev_add()`
- [x] Implement ioctls: `NOS_BDE_READ_REG`, `NOS_BDE_WRITE_REG`, `NOS_BDE_GET_DMA_INFO`, `NOS_BDE_SCHAN_OP`
- [x] Implement `mmap()` handler: map DMA pool physical pages to user VMA

#### 1d — Validation
- [x] Write C test: open `/dev/nos-bde`, `NOS_BDE_READ_REG(0)` → expect PCI config data
- [x] Write C test: mmap DMA pool, write pattern, read back
- [x] Write C test: read CMIC register `0x32800` (S-Channel control) → confirms BAR0 accessible
- [ ] Run `bde_validate` on target (requires BDE modules loaded)

**Deliverable**: Our own BDE operational on hardware. Can read/write CMIC registers and access DMA pool from userspace.

### Phase 2 — Custom SDK Core: libbcm56846

This is the longest phase. Build incrementally, test on hardware after each sub-module.

#### 2a — S-Channel and Register Access
- [x] Implement `schan_write(unit, cmd_word, data_words[], len)` using BDE ioctl + DMA
- [x] Implement `schan_read(unit, addr, data_words[], len)` (stub; format TBD from RE)
- [x] Implement `reg_write32(unit, offset, value)` and `reg_read32()` via BDE ioctl (READ_REG/WRITE_REG)
- [ ] Test: write a known register, read it back (on hardware)

Key data: `../docs/reverse-engineering/SCHAN_FORMAT_ANALYSIS.md`, `../docs/reverse-engineering/WRITE_MECHANISM_ANALYSIS.md`
S-Channel command word format: `0x2800XXXX`; DMA path: `FUN_10324084` → `FUN_103257B8`

#### 2b — ASIC Init
- [x] Implement `bcm56846_attach(unit)`: open BDE, mmap BAR0, get DMA info
- [x] Implement `bcm56846_init(unit)`: load .bcm config (config.bcm), run rc.soc + rc.datapath_0 if present
- [x] Implement SOC script runner: parse `rc.soc`, `rc.datapath_0`; execute `setreg`/`getreg` (numeric addr/val)
- [x] **config.bcm portmap entries**: Sample `etc/nos/config.bcm` with all 52 `portmap_N.0=...` entries (from RE doc). Ship in rootfs at `/etc/nos/config.bcm`.
- [ ] **rc.datapath_0**: Ship static pre-generated in rootfs; SOC runner loads it (runner in place, file TBD from capture).
- [ ] **LED programs**: Ship `led0.hex`/`led1.hex` in `/etc/nos/` (TBD from capture).
- [ ] Test: ASIC initializes, no crash, registers read back expected values (on hardware)

Key data: `../docs/reverse-engineering/initialization-sequence.md`, `../docs/reverse-engineering/SDK_AND_ASIC_CONFIG_FROM_SWITCH.md`

#### 2c — Port Bringup + SerDes
- [x] Implement XLPORT/MAC register writes for port enable/disable (S-Channel; XLPORT block map)
- [x] Implement Warpcore WC-B0 SerDes MDIO init sequence (10G SFI mode) — `sdk/src/serdes.c`, `bcm56846_serdes_init_10g()`; called from port enable
- [ ] Implement 40G port init (QSFP breakout)
- [x] Implement `bcm56846_port_enable_set()`, `bcm56846_port_speed_set()`, `bcm56846_port_link_status_get()`
- [ ] Test: `swp1` comes up, SFP transceiver negotiates, `ip link show swp1` shows LOWER_UP

Key data: `../docs/reverse-engineering/PORT_BRINGUP_REGISTER_MAP.md`, `../docs/reverse-engineering/SERDES_WC_INIT.md`
XLPORT formula: `block_base + 0x80000 + port_offset`; SerDes MDIO pages: 0x0008, 0x0a00, 0x1000, 0x3800

#### 2d — L2 Table
- [x] Implement `bcm56846_l2_addr_add(unit, l2_addr)`: pack L2_ENTRY 4 words (VALID, KEY_TYPE, VLAN, MAC, PORT, MODULE, STATIC), hash key `(MAC<<16)|(VLAN<<4)|KEY_TYPE`, linear probe 0..5; S-Channel WRITE_MEMORY
- [x] Implement `bcm56846_l2_addr_delete(unit, mac, vid)`: hash key, probe, write VALID=0 at slot
- [x] Implement `bcm56846_l2_addr_get(unit, mac, vid, l2_addr)`: hash lookup + S-Channel READ
- [ ] Implement L2_USER_ENTRY writes for guaranteed/BPDU entries
- [ ] Test: add a static MAC, verify in hardware (after S-Chan write path)

Key data: `../docs/reverse-engineering/L2_ENTRY_FORMAT.md`, `../docs/reverse-engineering/L2_WRITE_PATH_COMPLETE.md`
L2_ENTRY: 131072 entries × 13 bytes, base `0x07120000`; KEY = `(MAC<<16)|(VLAN<<4)|(KEY_TYPE<<1)`

#### 2e — L3 / ECMP
- [x] Implement `bcm56846_l3_intf_create()` / `bcm56846_l3_intf_destroy()`: EGR_L3_INTF
- [x] Implement `bcm56846_l3_egress_create()`: write ING_L3_NEXT_HOP + EGR_L3_NEXT_HOP
- [x] Implement `bcm56846_l3_route_add()`: write L3_DEFIP TCAM entry with prefix/mask
- [x] Implement `bcm56846_l3_route_delete()`: delete L3_DEFIP entry
- [x] Implement `bcm56846_l3_host_add()`: write L3_DEFIP host (/32) entry
- [x] Implement `bcm56846_l3_ecmp_create()`: write L3_ECMP + L3_ECMP_GROUP entries
- [ ] Test: add a route, verify ASIC forwards traffic to correct port without CPU involvement

Key data: `../docs/reverse-engineering/L3_NEXTHOP_FORMAT.md`, `../docs/reverse-engineering/L3_ECMP_VLAN_WRITE_PATH.md`
Chain: `L3_DEFIP[prefix] → NEXT_HOP_INDEX → ING_L3_NEXT_HOP[idx] → PORT_NUM[22:16]`
EGR chain: `EGR_L3_NEXT_HOP[idx] → DA_MAC[62:15] + INTF_NUM[14:3] → EGR_L3_INTF[intf] → SA_MAC[80:33] + VLAN[24:13]`

#### 2f — Packet I/O
- [x] Implement DMA ring setup for TX and RX channels (DCB type 21, PKTIO_BDE_DMA_INTERFACE)
- [x] Implement `bcm56846_tx(unit, pkt, pkt_len, port)`: copy packet to DMA buffer, DCB, kick CMICm DMA
- [x] Implement RX: register DMA completion callback, deliver packet + ingress port metadata
- [x] Implement poll/interrupt mode for RX (polling thread)
- [ ] Test: inject ARP packet via TUN → verify it goes out ASIC port; receive ARP reply from ASIC

Key data: `../docs/reverse-engineering/DMA_DCB_LAYOUT_FROM_KNET.md`, `../docs/reverse-engineering/PACKET_BUFFER_ANALYSIS.md`
DMA channels: `CMICM_DMA_DESC0r = 0x31158 + 4×chan`; DCB = packet buffer pointer + metadata

#### 2g — VLAN
- [x] Implement `bcm56846_vlan_create(unit, vid)` (VLAN + EGR_VLAN tables, shadow bitmaps)
- [x] Implement `bcm56846_vlan_port_add(unit, vid, port, tagged)`
- [x] Implement `bcm56846_vlan_destroy(unit, vid)`
- [ ] Test: create VLAN, add ports, verify inter-VLAN and intra-VLAN forwarding

### Phase 3 — nos-switchd

- [x] TUN device creation: open `/dev/net/tun`, `ioctl(TUNSETIFF, "swpN")` × N (from ports.conf or default 52)
- [x] Port configuration reader: parse `ports.conf` → port names/count (swp1..swp52); default 52 if file missing
- [x] Netlink socket setup: `RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_NEIGH | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR`
- [x] RTM_NEWLINK handler → `bcm56846_port_enable_set()` (swpN → port N 1-based)
- [x] **RTM_NEWADDR handler** → `bcm56846_l3_intf_create()`: ifindex→port cache, synthetic port MAC, create EGR_L3_INTF (stub). RTM_DELADDR → `bcm56846_l3_intf_destroy()`
- [x] RTM_NEWROUTE handler → parse dst/gateway/oif; neighbor cache for gateway MAC; `bcm56846_l3_egress_create()` + `bcm56846_l3_route_add()`. RTM_DELROUTE → `bcm56846_l3_route_delete()`
- [x] RTM_NEWNEIGH handler → `bcm56846_l2_addr_add()`; cache (ip, ifindex)→MAC for route egress. RTM_DELNEIGH → `bcm56846_l2_addr_delete()` + cache remove
- [x] **Link state polling thread**: poll `bcm56846_port_link_status_get()` at 200 ms; on state change, update TUN admin state via `tun_set_up()` (SIOCSIFFLAGS)
- [x] TX thread: `epoll` on all TUN fds; on read → `bcm56846_tx(unit, port, pkt, len)`
- [x] RX: `bcm56846_rx_register()` + `bcm56846_rx_start()`; callback writes to correct TUN fd from ingress port

**Deliverable**: nos-switchd running. Ping across two directly connected ports works entirely in hardware.

### Phase 4 — Routing Protocols + Integration

- [ ] Build FRR for PPC32 target
- [ ] Configure FRR: BGP + OSPF basic config
- [ ] Verify: FRR installs routes into kernel via `ip route`; nos-switchd picks up RTM_NEWROUTE; ASIC forwards
- [ ] Test ECMP: two BGP next-hops; nos-switchd calls `bcm56846_l3_ecmp_create()`; hardware hashes flows
- [ ] Configure BFD for fast failover
- [ ] Test failover: pull a cable → FRR reconverges → nos-switchd reprograms ASIC within target time

**Deliverable**: Full L3 routing stack. BGP peers can be established and traffic routes in hardware.

### Phase 5 — Platform Management

- [ ] Integrate ONLP (accton_as5610_52x platform) or write platform-mgrd
- [ ] Thermal monitoring daemon: read 10 temperature sensors; trigger fan speed adjustments
- [ ] Fan control: write CPLD `pwm1` (0–248 scale) based on thermal policy
- [ ] PSU monitoring: read `psu_pwr1_present`, `psu_pwr1_dc_ok` etc.
- [ ] Status LEDs: `led_psu1`, `led_diag`, `led_fan` via CPLD sysfs
- [ ] SFP/QSFP presence and EEPROM read via i2c-22..i2c-73

Key data: `../docs/reverse-engineering/PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md`, `../docs/reverse-engineering/SFP_TURNUP_AND_ACCESS.md`

### Phase 6 — ONIE Installer

- [x] Write `install.sh`: self-extracting shell script, detect platform, partition, install kernel+rootfs (onie-installer/install.sh)
- [x] Write `platform.conf` for accton_as5610_52x (cumulus/init/accton_as5610_52x/platform.conf)
- [x] Write `platform.fdisk` (MBR layout: sda1 persist, sda5/6 slot A, sda7/8 slot B, sda3 rw-overlay)
- [x] Write `uboot_env` fragments: `cl.active=1`, `bootsource=flashboot` (uboot_env/*.inc)
- [x] Build rootfs: Debian 12 PPC32 rootfs (rootfs/build.sh: debootstrap + our artifacts + squashfs)
- [x] Package: `onie-installer/build.sh` → `open-nos-as5610-YYYYMMDD.bin` (data.tar with FIT + squashfs)
- [ ] Test: factory-fresh switch → `onie-nos-install http://.../open-nos-as5610-YYYYMMDD.bin` → switch boots our NOS

Key data: `../docs/reverse-engineering/ONIE_BOOT_AND_PARTITION_LAYOUT.md`
Storage: internal USB at `/sys/bus/usb/devices/1-1.3:1.0`; partition: sda5=kernel-A, sda6=rootfs-A, sda7=kernel-B, sda8=rootfs-B

---

## 5. SDK Design — BCM56846 API Surface

### 5.1 Directory Layout

```
sdk/
├── include/
│   ├── bcm56846.h          # Public API (init, port, l2, l3, ecmp, vlan, pktio, stats)
│   ├── bcm56846_tables.h   # ASIC table definitions (L2_ENTRY, L3_DEFIP, etc.)
│   ├── bcm56846_regs.h     # Register offsets (CMIC, XLPORT, S-Channel)
│   └── bcm56846_types.h    # Common types (mac_t, ip4_t, l2_addr_t, l3_route_t, etc.)
└── src/
    ├── init/
    │   ├── attach.c         # BDE open, BAR mmap, DMA info
    │   ├── init.c           # ASIC init, config parse
    │   └── soc_script.c     # rc.soc runner
    ├── schan/
    │   └── schan.c          # S-Channel DMA write/read, ring management
    ├── port/
    │   ├── port.c           # Port enable/disable/speed API
    │   ├── xlport.c         # XLPORT/MAC register writes
    │   └── serdes_wc.c      # Warpcore WC-B0 MDIO init sequences
    ├── l2/
    │   ├── l2.c             # bcm56846_l2_addr_add/delete/get
    │   ├── l2_entry.c       # L2_ENTRY bitfield pack/unpack
    │   └── l2_hash.c        # Hash key computation
    ├── l3/
    │   ├── l3.c             # bcm56846_l3_route_add/del, l3_host_add
    │   ├── l3_egress.c      # bcm56846_l3_egress_create/destroy
    │   ├── l3_defip.c       # L3_DEFIP TCAM entry pack/unpack
    │   ├── l3_nexthop.c     # ING/EGR_L3_NEXT_HOP pack/unpack
    │   └── l3_intf.c        # EGR_L3_INTF pack/unpack
    ├── ecmp/
    │   └── ecmp.c           # bcm56846_l3_ecmp_create/destroy, L3_ECMP + L3_ECMP_GROUP
    ├── vlan/
    │   └── vlan.c           # bcm56846_vlan_create/port_add/destroy
    ├── pktio/
    │   ├── pktio.c          # bcm56846_tx, bcm56846_rx_register/start/stop
    │   └── dma_ring.c       # DCB ring setup and management
    └── stats/
        └── stats.c          # bcm56846_stat_get (counter reads)
```

### 5.2 Key Constants (from RE)

```c
/* ASIC identity */
#define BCM56846_DEVICE_ID      0xb846
#define BCM56846_BAR0_PHYS      0xa0000000UL
#define BCM56846_BAR0_SIZE      (256 * 1024)

/* CMIC registers (BAR0 offsets) */
#define CMIC_CMC0_SCHAN_CTRL    0x32800
#define CMICM_CMC_BASE          0x31000
#define CMICM_DMA_CTRL(ch)      (0x31140 + 4*(ch))
#define CMICM_DMA_DESC0(ch)     (0x31158 + 4*(ch))
#define CMICM_DMA_HALT_ADDR(ch) (0x31120 + 4*(ch))
#define CMICM_DMA_STAT          0x31150
#define CMIC_MIIM_PARAM         0x00000158  /* BAR0+0x158 — SerDes MDIO */
#define CMIC_MIIM_ADDRESS       0x000004a0  /* BAR0+0x4a0 */

/* S-Channel command word base */
#define SCHAN_CMD_BASE          0x2800

/* L2_ENTRY table */
#define L2_ENTRY_BASE           0x07120000
#define L2_ENTRY_COUNT          131072
#define L2_ENTRY_BYTES          13

/* L2_USER_ENTRY table (TCAM) */
#define L2_USER_ENTRY_BASE      0x06168000
#define L2_USER_ENTRY_COUNT     512
#define L2_USER_ENTRY_BYTES     20

/* L3_DEFIP (TCAM) */
#define L3_DEFIP_ENTRY_BYTES    30
/* KEY = (VRF<<33)|(IP<<1)|MODE */

/* L3_ECMP */
#define L3_ECMP_COUNT           4096
#define L3_ECMP_BYTES           2
/* NEXT_HOP_INDEX: bits [13:0] */

/* L3_ECMP_GROUP */
#define L3_ECMP_GROUP_COUNT     1024
#define L3_ECMP_GROUP_BYTES     25
/* BASE_PTR[21:10], COUNT[9:0] */

/* XLPORT block formula */
/* block_base = (block_id * 0x1000) + port_in_block_offset */
/* XLPORT_PORT_ENABLE: block_base + 0x80000 + 0x22a */
/* MAC_MODE: lane*0x1000 + 0x511 */
/* MAC_0: lane*0x1000 + 0x503 */
/* MAC_1: lane*0x1000 + 0x504 */
```

### 5.3 L2 Entry Bit Layout

```
L2_ENTRY (13 bytes = 104 bits, big-endian):
  bit  0:      VALID
  bits  3:1:   KEY_TYPE (0=L2, 1=L2_VFI)
  bits 15:4:   VLAN_ID
  bits 63:16:  MAC_ADDR (48 bits)
  bits 70:64:  PORT_NUM (7 bits)
  bits 78:71:  MODULE_ID (8 bits)
  bit  79:     T (trunk bit)
  bit  93:     STATIC_BIT

Hash key: (MAC_ADDR << 16) | (VLAN_ID << 4) | (KEY_TYPE << 1)
```

### 5.4 L3 Forwarding Chain

```
L3_DEFIP[prefix/len]
    → NEXT_HOP_INDEX (14 bits)
        → ING_L3_NEXT_HOP[idx]
            PORT_NUM:    bits [22:16]
            MODULE_ID:   bits [30:23]
        → EGR_L3_NEXT_HOP[idx]
            L3:MAC_ADDRESS: bits [62:15]  (48 bits DA)
            L3:INTF_NUM:    bits [14:3]   (12 bits → EGR_L3_INTF index)
                → EGR_L3_INTF[intf_num]
                    VID:        bits [24:13]  (12 bits VLAN)
                    MAC:        bits [80:33]  (48 bits SA)
```

### 5.5 Warpcore SerDes Init (10GbE SFI)

MDIO write sequence via CMIC_MIIM_PARAM register (`0x0291xxxx`):
- `INTERNAL_SEL=1`, `BUS_ID=lane/4`, `PHY_ADDR=lane%4`, `DATA=value`

```
page=0x0008, reg[0x1e] = 0x8000   -- IEEE block enable
page=0x0a00, reg[0x10] = 0xffe0   -- SerDes digital: fiber/SFI mode
page=0x1000, reg[0x18] = 0x8010   -- Clock recovery
page=0x3800, reg[0x01] = 0x0010   -- WC_CORE sequencer start
page=0x0000, reg[0x17] = 0x8010   -- TX config
page=0x0000, reg[0x18] = 0x8370   -- TX pre-emphasis
page=0x1000, reg[0x19..0x1d]      -- RX equalizer settings
```

---

## 6. Control Plane Daemon Design

### 6.1 Thread Architecture

```
nos-switchd main process
├── main thread
│   ├── SDK init (bcm56846_attach + bcm56846_init)
│   ├── TUN device creation (swp1..swpN)
│   └── Signal handling
├── netlink thread
│   ├── poll(netlink_fd)
│   └── RTM_* dispatch → SDK API calls (serialized via mutex)
│       RTM_NEWLINK, RTM_NEWADDR/DELADDR, RTM_NEWROUTE/DELROUTE, RTM_NEWNEIGH/DELNEIGH
├── link-state poll thread          ← NEW: physical link monitoring
│   ├── sleep(200ms)
│   ├── for each port: bcm56846_port_link_status_get()
│   └── on change: SIOCSIFFLAGS on TUN, flush neighbors, notify FRR
├── tx thread
│   ├── epoll(all TUN fds)
│   └── on readable → bcm56846_tx()
└── rx thread
    ├── bcm56846_rx_start() callback registered
    └── callback: write(tun_fd[ingress_port], pkt, len)
```

### 6.2 Port Mapping (porttab)

Format from RE: `linux_intf sdk_intf unit is_fabric`

```
swp1   xe0   0   0
swp2   xe1   0   0
...
swp48  xe47  0   0
swp49  xe48  0   0   (40G QSFP, no breakout)
# or with breakout:
swp49s0  xe48  0   0
swp49s1  xe49  0   0
swp49s2  xe50  0   0
swp49s3  xe51  0   0
```

### 6.3 Config Files

`ports.conf` (user-facing):

```ini
# Port mode: 10G, 40G, or 4x10G (breakout)
1=10G
2=10G
...
48=10G
49=40G      # or 49=4x10G for breakout
50=40G
51=40G
52=40G
```

`rc.forwarding` (ASIC forwarding behavior):

```
l3_mtu_fail_tocpu=1
l3_slowpath_tocpu=1
v4l3dstmiss_tocpu=1
v6l3dstmiss_tocpu=1
ttl1_tocpu=1
rtag7_hash_sel=4        # ECMP hash: srcIP+dstIP+proto+srcPort+dstPort
```

---

## 7. Packet I/O Design

### 7.1 TX Path (CPU → ASIC port)

```
nos-switchd tx_thread:
  while (true):
    epoll_wait(tun_fds[])
    for each ready fd:
      port = tun_fd_to_port[fd]
      n = read(fd, pkt_buf, MTU)
      bcm56846_tx(unit, port, pkt_buf, n)

bcm56846_tx():
  get DMA tx ring slot
  copy packet to DMA buffer (or set pointer if zero-copy)
  fill DCB: pktaddr=dma_phys, len=n, port=port, flags=START|END
  write DCB to ring
  write CMICM_DMA_DESC0(TX_CHAN) = ring_phys
  write CMICM_DMA_CTRL(TX_CHAN) |= START bit
  wait for completion interrupt or poll CMICM_DMA_STAT
```

### 7.2 RX Path (ASIC → CPU)

```
bcm56846_rx_start():
  allocate DMA rx ring (N DCB slots, each pointing to a packet buffer)
  write CMICM_DMA_DESC0(RX_CHAN) = rx_ring_phys
  write CMICM_DMA_HALT_ADDR(RX_CHAN) = last_dcb_phys
  write CMICM_DMA_CTRL(RX_CHAN) |= START bit
  register interrupt handler for RX_CHAN completion

interrupt handler / poll:
  walk completed DCBs
  for each complete DCB:
    ingress_port = DCB.SRCPORT field
    pkt = DCB.pktaddr (DMA buffer)
    len = DCB.pktlen
    invoke rx_callback(ingress_port, pkt, len)

nos-switchd rx_callback:
  fd = port_to_tun_fd[ingress_port]
  write(fd, pkt, len)
```

### 7.3 Punt Configuration

The ASIC automatically punts to CPU (sets COPYTO_CPU in the pipeline) for:
- TTL=1 packets (routed to CPU for ICMP TTL-exceeded generation)
- L3 destination miss (routes not in FIB → CPU can handle and install)
- ARP/NDP (not L3-routed; punted to CPU for Linux ARP stack)
- BPDUs (STP, punted via L2_USER_ENTRY with `CPU@129=1`)

These punt behaviors are configured by `rc.forwarding` register writes during ASIC init.

---

## 8. Routing Protocol Integration

### 8.1 FRRouting (FRR)

FRR is the only routing stack needed. It is fully open-source and handles:
- **BGP** (eBGP, iBGP, route reflectors, communities, ECMP)
- **OSPF** (OSPFv2, OSPFv3)
- **ISIS**
- **Static routes**
- **BFD** (fast failure detection, sub-second)
- **VRFs** (if needed)

FRR talks to the Linux kernel via:
- Netlink: install routes (`RTM_NEWROUTE`), query neighbors
- Zebra daemon manages the kernel FIB

Our nos-switchd reads the same netlink stream → programs ASIC in sync with kernel FIB.

### 8.2 ECMP in Hardware

When FRR installs a multipath route (multiple nexthops):
- Linux kernel creates a multipath route with multiple `RTA_MULTIPATH` nexthops
- nos-switchd detects multiple nexthops in `RTM_NEWROUTE`
- Creates egress objects for each nexthop
- Calls `bcm56846_l3_ecmp_create()` with all egress IDs
- ASIC uses RTAG7 hash (5-tuple) to load-balance across all nexthops in hardware

### 8.3 Convergence Path

```
Link failure detected (link down interrupt)
  → nos-switchd: port_link_status_get() → down
  → nos-switchd: RTM_DELNEIGH for neighbors on that port
  → FRR BFD: session down (or BGP hold timer)
  → FRR: route withdrawn, new best path selected
  → FRR: RTM_NEWROUTE with updated nexthops
  → nos-switchd: bcm56846_l3_route_add() reprograms ASIC
  → traffic restored via new path
```

---

## 9. Platform Management

### 9.1 CPLD Interface

Driver: `accton_as5610_52x_cpld` (upstream Linux or ONL tree)
Sysfs base: `/sys/devices/ff705000.localbus/ea000000.cpld`

| Function | Sysfs Attribute |
|----------|----------------|
| PSU 1/2 presence | `psu_pwr1_present`, `psu_pwr2_present` |
| PSU 1/2 DC OK | `psu_pwr1_dc_ok`, `psu_pwr2_dc_ok` |
| Fan OK | `system_fan_ok`, `system_fan_present` |
| Fan airflow | `system_fan_air_flow` (front-to-back or back-to-front) |
| Fan PWM | `pwm1` (0–248) |
| LED PSU 1/2 | `led_psu1`, `led_psu2` (green/yellow/off) |
| LED diag/fan | `led_diag`, `led_fan`, `led_locator` |
| Watchdog | `watch_dog_enable`, `watch_dog_timeout`, `watch_dog_keep_alive` |

### 9.2 Temperature Sensors

| Sensor | Path |
|--------|------|
| ASIC die | `/sys/devices/pci0000:00/.../0000:01:00.0/temp1_input` |
| Board (×7) | `/sys/devices/soc.0/.../i2c-9/9-004d/temp1..7_input` |
| NE1617A (×2) | `/sys/devices/soc.0/.../i2c-9/9-0018/temp1_input`, `temp2_input` |

### 9.3 SFP/QSFP

I2C buses: i2c-22..i2c-69 for SFP ports 1–48; i2c-70..i2c-73 for QSFP 49–52
Address 0x50: EEPROM data; 0x27: presence/control
Requires platform I2C mux drivers (from ONL or written from sysfs paths)

### 9.4 ONLP Integration

The Open Network Linux Platform (ONLP) library supports accton_as5610_52x in the ONL repository. If we integrate ONLP:
- `onlp_sfp_is_present(port)` — SFP presence check
- `onlp_thermal_info_get(id, &info)` — temperature
- `onlp_fan_info_get(id, &info)` — fan status and RPM
- `onlp_psu_info_get(id, &info)` — PSU status

---

## 10. ONIE Installer

### 10.1 Partition Layout (AS5610-52X)

```
/dev/sda (USB flash, 2GB+):
  sda1: persist    ext2  (startup config, licenses)
  sda2: extended   (container)
    sda5: kernel-A   raw uImage FIT
    sda6: rootfs-A   squashfs
    sda7: kernel-B   raw uImage FIT
    sda8: rootfs-B   squashfs
  sda3: rw-overlay ext2  (writable layer on top of squashfs)
```

### 10.2 Installer Script Structure

```sh
#!/bin/sh
# ... header: detect ONIE, load platform ...
# ... functions: format_disk, image_install, image_env_handler ...
exit 0
__ARCHIVE__
# binary: control.tar.xz + data.tar
```

`control.tar.xz` contents:
- `control`: arch=powerpc, platforms=accton_as5610_52x, version=X.Y.Z
- `cumulus/init/accton_as5610_52x/platform.conf`
- `cumulus/init/accton_as5610_52x/platform.fdisk`
- `uboot_env/as5610_52x.platform.inc`
- `uboot_env/common_env.inc`

`data.tar` contents:
- `uImage-powerpc.itb` (kernel + initramfs FIT image)
- `sysroot.squash.xz` (squashfs rootfs with all packages)

### 10.3 U-Boot Variables Set by Installer

```
cl.active=1
bootsource=flashboot
cl.platform=accton_as5610_52x
```

### 10.4 A/B Slot Upgrade Procedure

The two-slot layout (A=sda5/6, B=sda7/8) enables in-service upgrades without risk of bricking:

```
Current active: slot A (cl.active=1 → U-Boot boots sda5/sda6)

Upgrade steps:
1. Download new image to running system
2. Write kernel to inactive slot B: dd if=nos-powerpc.itb of=/dev/sda7
3. Write rootfs to inactive slot B: dd if=sysroot.squash.xz of=/dev/sda8
4. Set U-Boot active slot: fw_setenv cl.active 2
5. Reboot → U-Boot reads cl.active=2 → boots sda7/sda8
6. Verify new version works
7. (Optional rollback: fw_setenv cl.active 1 && reboot)
8. After verification: fw_setenv cl.active 2 (permanent)
```

**Rollback**: If the new slot fails to boot, U-Boot increments a boot counter. After N failed boots it can revert to the previous slot (requires U-Boot scripting in `bootcmd`). Alternatively, operator runs `fw_setenv cl.active 1` from ONIE rescue shell.

The `rw-overlay` partition (sda3) is shared between both slots. On a slot switch, the overlay may contain stale files from the previous version. The upgrade script should wipe sda3 or use a per-slot overlay directory.

---

## 11. Build System

### 11.1 Rootfs: Debian 12 (Bookworm) PPC32

The rootfs is a Debian 12 (Bookworm) system bootstrapped for `powerpc` (PPC32 big-endian).
Debian maintains a full PPC32 archive with all the packages we need.

**Bootstrap flow** (runs on x86 build host):

```bash
# Stage 1: foreign debootstrap on x86
debootstrap --arch=powerpc --foreign bookworm /mnt/rootfs \
    http://deb.debian.org/debian

# Stage 2: complete inside QEMU user-mode emulation
cp /usr/bin/qemu-ppc-static /mnt/rootfs/usr/bin/
chroot /mnt/rootfs /debootstrap/debootstrap --second-stage

# Stage 3: install NOS packages
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
    libpci-dev

# Stage 4: install our own .deb packages
dpkg --root=/mnt/rootfs -i \
    nos-switchd_*.deb \
    libbcm56846_*.deb \
    platform-mgrd_*.deb

# Stage 5: pack to squashfs
mksquashfs /mnt/rootfs sysroot.squash.xz -comp xz
```

**Why Debian 12:**
- Full PPC32 package archive — FRR, iproute2, lldpd, openssh all install with `apt`
- `dpkg` means our own code (SDK, switchd) ships as versioned `.deb` packages
- Security updates via `apt upgrade` — no rebuild required for CVE fixes
- Same approach as Cumulus (Debian 7) and ONL (Debian 8), but on a supported release
- `debootstrap` + QEMU user-mode is the standard cross-bootstrap method for PPC32

### 11.2 Toolchain

Cross-compiler: `powerpc-linux-gnu-gcc` (from `gcc-powerpc-linux-gnu` Debian package on build host)

```bash
# Install on build host (x86 Debian/Ubuntu)
apt-get install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu \
                qemu-user-static debootstrap
```

### 11.3 Kernel Build

```bash
# Linux 5.10 LTS for PPC32 e500v2
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
     p2020rdb_defconfig        # baseline for P2020; then customize

make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
     uImage modules

# Key config options:
# CONFIG_PPC32=y
# CONFIG_E500=y               Freescale P2020 (e500v2 core)
# CONFIG_PCI=y
# CONFIG_NET_VENDOR_BROADCOM=n  we don't use BCM ethernet drivers
# CONFIG_TUN=y                TUN/TAP for swp interfaces
# CONFIG_I2C=y, CONFIG_I2C_MUX=y
# CONFIG_SFP=y
# CONFIG_HWMON=y
```

### 11.4 Our Code: .deb Packages

Our components ship as Debian packages, cross-built with the PPC32 toolchain:

```bash
# SDK
cd sdk && cmake -DCMAKE_TOOLCHAIN_FILE=../tools/ppc32-toolchain.cmake ..
make && make package   # produces libbcm56846_1.0.0_powerpc.deb

# switchd
cd switchd && cmake -DCMAKE_TOOLCHAIN_FILE=../tools/ppc32-toolchain.cmake ..
make && make package   # produces nos-switchd_1.0.0_powerpc.deb

# BDE modules
make -C bde ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
     KERNEL_SRC=/path/to/linux-5.10
# produces nos-bde-modules_1.0.0_powerpc.deb (DKMS or pre-built)
```

### 11.5 ONIE Installer Assembly

```bash
cd onie-installer && ./build.sh
# Inputs:  uImage-powerpc.itb  (kernel FIT image)
#          sysroot.squash.xz   (Debian 12 rootfs + our packages)
# Output:  open-nos-as5610-YYYYMMDD.bin
```

---

## 12. Testing Strategy

### 12.1 Unit Tests (x86, QEMU)

- **SDK unit tests**: mock BDE layer; test bitfield pack/unpack for L2_ENTRY, L3 tables, S-Channel command word
- **nos-switchd netlink tests**: inject synthetic netlink messages; verify SDK API calls
- Run on x86 with mock hardware layer before ever touching the switch

### 12.2 Hardware Integration Tests

In order of increasing complexity:

| Test | What It Validates |
|------|------------------|
| BDE opens, device ID reads back 0xb846 | BDE + ASIC accessible |
| `bcm56846_init()` completes without error | ASIC initializes |
| swp1 comes up after `ip link set swp1 up` | SerDes + port bringup |
| `ping` across two ports on same VLAN | L2 forwarding in hardware |
| `ping` across two ports on different VLANs via SVI | L3 forwarding in hardware |
| `iperf3` at line rate (10Gbps) no CPU spike | Traffic NOT hitting CPU |
| BGP session between AS5610 and another router | FRR + nos-switchd + ASIC routing |
| Pull cable → BGP reconverges → traffic resumes | Fast failover |
| ECMP across two ports, 5-tuple hashing | ECMP in hardware |
| `ssh` to switch IP via swp interface | Punt (host-bound) traffic path |

### 12.3 Regression Checks

- After any SDK change: run L2/L3 table read-back against known values
- After nos-switchd change: netlink unit tests pass
- Before any release: full hardware integration test suite

---

## 13. Known Gaps and Risk Register

### 13.1 Critical Gaps (switch will not function without these)

| Gap | Risk | Status | Mitigation |
|-----|------|--------|-----------|
| **initramfs** | High — without initramfs the kernel cannot mount squashfs+overlayfs and panics at boot | **Added to Phase 1** | Build minimal busybox initramfs; embed in FIT image; mount squashfs ro + overlayfs rw then pivot_root |
| **Device Tree Blob (DTB)** | High — without DTB the PPC32 kernel cannot enumerate P2020 SoC peripherals (eTSEC, I2C, PCIe, CPLD) | **Added to Phase 1** | Extract from Cumulus FIT image via `dumpimage -l` + patch, or build from ONL's `as5610_52x.dts` |
| **config.bcm portmap entries** | High — `bcm_init` segfaults during port enumeration without correct `portmap_N.0=...` lines | **Added to Phase 2b** | All 52 portmap entries captured from live switch; ship verbatim in `/etc/nos/config.bcm` |
| **RTM_NEWADDR / RTMGRP_IPV4_IFADDR** | High — without this netlink subscription, `EGR_L3_INTF` is never created; L3 routing silently broken | **Added to §3.3 and Phase 3** | Subscribe to `RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR`; handle `RTM_NEWADDR` to create `EGR_L3_INTF` |
| **Link state polling loop** | High — without it, physical link-down events are never seen by FRR; BGP stays up on dead links | **Added to §3.3 and Phase 3** | Dedicated thread polls `bcm56846_port_link_status_get()` at 200 ms; synthesizes link events for FRR |
| **rc.datapath_0** | High — without this SOC script the datapath pipeline is unconfigured; no packet forwarding | **Added to Phase 2b** | Ship static pre-generated file from live switch capture at `/etc/nos/rc.datapath_0` |
| **Management interface (eth0)** | High — without eth0 there is no way to SSH into the switch after install | **Added to §3.5** | Enable `CONFIG_GIANFAR=y`; DTB must have correct eTSEC nodes; ifupdown2 config for eth0 |

### 13.2 Important Gaps (degraded functionality without these)

| Gap | Risk | Status | Mitigation |
|-----|------|--------|-----------|
| **LED programs (led0.hex, led1.hex)** | Medium — port LEDs dark without them; switch still forwards | **Added to Phase 2b** | LED microcode captured from live switch (led0.asm=5104B, led1.asm=5223B); ship in `/etc/nos/` |
| **Serial console / UART** | Low — no console access during boot failures | ⚠️ DTB dependency | P2020 has DUART at 0xffe04500; DTB must define `serial0`; `CONFIG_SERIAL_8250=y`; baud 115200 |

### 13.3 Deferred / Nice-to-Have

| Item | Notes |
|------|-------|
| **ACL / Field Processor** | Advanced feature; defer to future phase |
| **STP / RSTP** | Only needed for L2-switched deployments; FRR includes `pathd` / `bfdd` but not STP |
| **LACP / LAG** | Bond interfaces; kernel bonding driver handles LACP; ASIC trunk group table needs programming |
| **L2XMSG hardware MAC learning** | Hardware-assisted MAC learning via interrupt; start with software-only (ARP/NDP → neighbor table) |
| **SNMP** | snmpd from Debian; MIBs need nos-switchd counter data |
| **Zero Touch Provisioning (ZTP)** | DHCP option 239; useful for production deployments |
| **Port watchdog** | CPLD watchdog: `watch_dog_enable` sysfs; reset if nos-switchd hangs |
| **VLAN table format (implementation)** | RE data complete: ingress VLAN 0x12168000 (4096×40B), EGR_VLAN 0x0d260000 (4096×29B), PORT_BITMAP bit positions all verified. Implementation deferred until L2 switching is needed. |

### 13.4 Low Risk / Resolved

| Gap | Resolution |
|-----|-----------|
| **Packet I/O DCB exact format** | ✅ DCB type 21 confirmed: 16 words (64 bytes). TX LOCAL_DEST_PORT encoding, RX ingress port metadata layout, BDE `WAIT_FOR_INTERRUPT`/`SEM_OP` ioctl sequence all verified. See `PKTIO_BDE_DMA_INTERFACE.md` |
| **S-Channel DMA completion** | ✅ Protocol documented in `WRITE_MECHANISM_ANALYSIS.md`; GDB trace confirmed command word format `0x2800XXXX` and DMA path |
| **IPv6 L3_DEFIP format** | ✅ L3_DEFIP_128 table (0x0a176000, 256×39B) for /128; L3_DEFIP double-wide (MODE0=MODE1=1) for prefixes ≤/64; L3_ENTRY_IPV6_UNICAST unused. See `L3_IPV6_FORMAT.md` |
| **Hardware stats / counters** | ✅ XLMAC counter register offsets verified: RPKT=0x0b, RBYT=0x34, TPKT=0x45, TBYT=0x64, R64=0x06, T64=0x3f etc. S-Channel address formula and port→block/lane mapping confirmed. See `STATS_COUNTER_FORMAT.md` |
| **A/B slot upgrade procedure** | ✅ Documented in §10.4. `fw_setenv cl.active 2` → write inactive sda7/sda8 → reboot; rollback via `cl.active 1` from ONIE shell |
| **VLAN table RE data** | ✅ Ingress VLAN 0x12168000 (4096×40B), EGR_VLAN 0x0d260000 (4096×29B), PORT_BITMAP + ING_PORT_BITMAP + UT_PORT_BITMAP bit positions all verified. See `VLAN_TABLE_FORMAT.md` |
| **PPC32 kernel version** | ✅ We write our own BDE; no external BDE version dependency |
| **mb_led_rst GPIO** | ✅ Not found under sysfs on live switch; skip LED reset if GPIO absent; non-fatal |

---

## 14. Reference Map to RE Docs

All RE docs are in `docs/reverse-engineering/` in this repository (relative to repo root).

| This Plan Section | Key RE Documents |
|------------------|-----------------|
| ONIE installer | `ONIE_BOOT_AND_PARTITION_LAYOUT.md` |
| BDE modules | `ASIC_INIT_AND_DMA_MAP.md`, `BDE_CMIC_REGISTERS.md` |
| ASIC init sequence | `initialization-sequence.md`, `SDK_AND_ASIC_CONFIG_FROM_SWITCH.md` |
| S-Channel | `SCHAN_FORMAT_ANALYSIS.md`, `WRITE_MECHANISM_ANALYSIS.md`, `SCHAN_AND_L2_ANALYSIS.md` |
| Port bringup | `PORT_BRINGUP_REGISTER_MAP.md`, `PORT_BRINGUP_ANALYSIS.md` |
| SerDes init | `SERDES_WC_INIT.md` |
| L2 tables | `L2_ENTRY_FORMAT.md`, `L2_WRITE_PATH_COMPLETE.md`, `L2_WRITE_PATH_ANALYSIS.md` |
| L3 tables | `L3_NEXTHOP_FORMAT.md`, `L3_ECMP_VLAN_WRITE_PATH.md`, `L3_DEFIP` fields in `PATH_B_COMPLETION_STATUS.md` |
| Packet I/O | `PKTIO_BDE_DMA_INTERFACE.md`, `PACKET_IO_VERIFIED.md`, `PACKET_BUFFER_ANALYSIS.md`, `DMA_DCB_LAYOUT_FROM_KNET.md` |
| Netlink → SDK | `netlink-handlers.md`, `netlink-message-flow.md`, `api-patterns.md` |
| Port config + porttab | `QSFP_BREAKOUT_CONFIGURATION.md`, `COMPLETE_INTERFACE_ANALYSIS.md` |
| Platform / CPLD | `PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md`, `SFP_TURNUP_AND_ACCESS.md` |
| Architecture overview | `WHAT_MAKES_THE_SWITCH_WORK.md`, `CUMULUS_VS_OPENNSL_ARCHITECTURE.md` |
| Gap analysis | `GAPS_FOR_CUSTOM_SWITCHD_SDK.md`, `STACK_READINESS_AS5610.md` |
| Register map | `SDK_REGISTER_MAP.md`, `GHIDRA_REGISTER_TABLE_ANALYSIS.md` |
| Config file formats | `bcm-config-format.md`, `SDK_AND_ASIC_CONFIG_FROM_SWITCH.md` |

---

## Summary: What Makes This Legal and Redistributable

- **No Broadcom SDK code**: We write our own SDK (libbcm56846) from RE documentation and public specs.
- **No Cumulus code**: We write our own installer, switchd, and platform tools from RE docs.
- **BDE written from scratch**: nos-kernel-bde.ko and nos-user-bde.ko are our own code, written from RE-documented register offsets. No OpenNSL or Broadcom BDE code included.
- **All other components** (Linux, FRR, iproute2, ONLP, Debian) are established open-source projects with permissive or copyleft licenses that permit redistribution.
- **RE methodology**: All findings obtained via legal analysis of hardware we own (see `docs/reverse-engineering/` in this repository).

---

*Generated: 2026-02-26*
