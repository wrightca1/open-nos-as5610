# open-nos-as5610 — Build and Implementation Status

**Last updated:** 2026-02-26

---

## Summary

| Area | Status | Notes |
|------|--------|-------|
| **Build system** | ✅ Working | Build server (Debian, PPC32 cross), kernel + BDE + SDK + switchd + tests |
| **Phase 1 — Boot + BDE** | ✅ Implemented | Kernel 5.10, BDE modules, S-Channel, validation test |
| **Phase 2 — SDK** | 🟢 2a–2g + L2_USER_ENTRY | Config, SOC runner, S-Chan, L2 add/delete/get, L2_USER_ENTRY add/delete, L3/ECMP, VLAN, port+SerDes, pktio, stats. 40G + HW tests pending. |
| **Phase 3 — nos-switchd** | 🟢 Core complete | Netlink→SDK for link/addr/route/neigh; link-state poll; TX/RX threads. Ready for HW/FRR test. |
| **Phase 1a DTB/initramfs/FIT** | ✅ In place | initramfs/build.sh, boot/build-fit.sh (mkimage -f auto), DTB from ONL/Cumulus or minimal .dts |
| **Rootfs** | ✅ Debian Jessie PPC32 | rootfs/build.sh — jessie from archive.debian.org (last Debian with powerpc); build locally or on server in Docker |
| **ONIE installer .bin** | ✅ Produced | One-command: `./scripts/build-onie-image.sh` → `onie-installer/open-nos-as5610-YYYYMMDD.bin`; loadable via ONIE |
| **Hardware validation** | ⏳ Pending | Run `bde_validate` and `onie-nos-install` on AS5610 |

---

## What Builds Today

- **Kernel:** Linux 5.10 (85xx/mpc85xx_cds_defconfig), PPC32 uImage + modules
- **BDE:** `nos_kernel_bde.ko`, `nos_user_bde.ko` — PCI probe, BAR0, 8MB DMA pool, S-Channel submit (DMA path), ioctl READ_REG/WRITE_REG/GET_DMA_INFO/SCHAN_OP, mmap DMA
- **SDK:** `libbcm56846.so` — attach/detach/init, config.bcm, SOC runner, schan write/read_memory, reg; **port** (enable, link, SerDes 10G); **L2** add/delete/get + **L2_USER_ENTRY** add/delete; **L3** intf/egress/route/host + **ECMP**; **VLAN**; **pktio** (TX/RX DCB21); **stats** (RPKT/RBYT/TPKT/TBYT).
- **nos-switchd:** PPC32 executable — attach, init, TUN creation, netlink (NEWLINK→port enable, NEWADDR→l3_intf, NEWROUTE/DELROUTE→l3_egress+route, NEWNEIGH/DELNEIGH→l2_addr), link-state poll, TX thread, RX callback→TUN write.
- **Tests:** `bde_validate` — READ_REG(0), mmap DMA write/read, READ_REG(0x32800)

**Build command:** `USE_BUILD_SERVER=modern BUILD_KERNEL=1 ./scripts/build-on-build-server.sh`  
**Artifacts:** See [BUILD.md](BUILD.md).

---

## Plan Progress (from PLAN.md)

### Phase 1 — Boot + Kernel + Our BDE
- **1a:** [x] Kernel 5.10 PPC32 built. [x] DTB (minimal .dts or from Cumulus/ONL). [x] initramfs packed. [x] FIT (nos-powerpc.itb). [ ] Boot on target via ONIE.
- **1b:** [x] nos-kernel-bde.ko (PCI, BAR0, DMA, S-Channel, exports)
- **1c:** [x] nos-user-bde.ko (/dev/nos-bde, ioctls, mmap)
- **1d:** [x] BDE validation test (bde_validate). [ ] Run on target → Passed 3/3

### Phase 2 — Custom SDK (libbcm56846)
- **2a:** [x] S-Channel and register access (BDE ioctl)
- **2b:** [x] Config loader + SOC runner; config.bcm. [x] rc.datapath_0/LED capture docs (etc/nos/README-CAPTURE.md).
- **2c–2g:** [x] Port, SerDes 10G, L2 add/delete/get, L2_USER_ENTRY add/delete, L3/ECMP, pktio, VLAN, stats. [ ] 40G, HW tests.

### Phase 3 — nos-switchd
- [x] TUN, ports.conf, netlink (NEWLINK, NEWADDR/DELADDR→l3_intf, NEWROUTE/DELROUTE→l3_egress+route, NEWNEIGH/DELNEIGH→l2_addr+cache), link-state, TX/RX. [x] SDK complete (L2 get, stats, L2_USER_ENTRY, SerDes). [ ] HW validation.

### Phase 6 — ONIE installer
- [x] install.sh (self-extracting), platform.conf, platform.fdisk, uboot_env, rootfs/build.sh (Debian jessie), onie-installer/build.sh → .bin
- [x] One-command image: `./scripts/build-onie-image.sh` produces loadable .bin (kernel + jessie rootfs). Rootfs built locally or on server (Docker).
- [ ] Test on switch: `onie-nos-install http://10.22.1.4:8000/open-nos-as5610-YYYYMMDD.bin` → boot our NOS

### Phase 4 — FRR
- [x] FRR PPC32: rootfs apt install frr; from-source in docs/FRR-PPC32.md. [ ] BGP/OSPF config, ECMP/BFD tests on HW.

### Phase 5 — Platform
- [x] platform-mgrd scaffold (platform/platform-mgrd: hwmon thermal poll). [ ] ONLP or full CPLD/fan/PSU/LED.

### Later
- All remaining items are HW tests or optional (40G, BFD, ONLP integration).

---

## Repository Layout (current)

```
open-nos-as5610/
├── STATUS.md           # This file
├── BUILD.md            # Build instructions, artifacts, copy commands
├── PLAN.md             # Full implementation plan (checkboxes updated)
├── CMakeLists.txt      # Top-level: sdk, switchd, tests
├── bde/                # Kernel BDE (nos_kernel_bde.c, nos_user_bde.c, Makefile)
├── sdk/                # libbcm56846 (include/, src/, CMakeLists.txt)
├── switchd/            # nos-switchd (src/main.c, CMakeLists.txt)
├── tests/              # bde_validate (Phase 1d)
├── tools/              # ppc32-toolchain.cmake
├── scripts/            # build-on-build-server.sh, remote-build.sh, run-bde-validate.sh
├── etc/nos/            # config.bcm (sample portmap for 52 ports)
├── initramfs/          # init script, build.sh (scaffolding)
├── boot/               # nos.its FIT template, README (DTB instructions)
├── rootfs/             # build.sh (debootstrap PPC32 + squashfs), overlay/ (fstab, systemd units)
├── platform/           # platform-mgrd (minimal hwmon daemon), README
└── onie-installer/     # install.sh, build.sh, platform.conf, platform.fdisk, uboot_env/ → .bin
```

---

## How to Run Validation on Target

1. Copy to AS5610: `bde_validate`, `nos_kernel_bde.ko`, `nos_user_bde.ko`.
2. Load BDE: `insmod nos_kernel_bde.ko`, `insmod nos_user_bde.ko`.
3. Run: `./bde_validate` → expect **Passed 3/3**.

Without hardware, the binary can be run under QEMU on the build server (`qemu-ppc-static -L /usr/powerpc-linux-gnu ./bde_validate`); it will fail on open(/dev/nos-bde) as expected.
