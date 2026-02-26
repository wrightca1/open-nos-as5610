# open-nos-as5610 — Build and Implementation Status

**Last updated:** 2026-02-26

---

## Summary

| Area | Status | Notes |
|------|--------|-------|
| **Build system** | ✅ Working | Build server (Debian, PPC32 cross), kernel + BDE + SDK + switchd + tests |
| **Phase 1 — Boot + BDE** | ✅ Implemented | Kernel 5.10, BDE modules, S-Channel, validation test |
| **Phase 2 — SDK** | 🟡 Skeleton + 2a | libbcm56846 builds; attach/schan/reg/ioctl; port/L2/L3/pktio/VLAN stubs |
| **Phase 3 — nos-switchd** | 🟡 Skeleton | Main links to SDK, attach/init loop; TUN/netlink not yet |
| **Phase 1a DTB/initramfs** | 📋 Scaffolding | initramfs/ and boot/nos.its; DTB still to obtain/build |
| **Hardware validation** | ⏳ Pending | Run `bde_validate` on AS5610 with BDE loaded |

---

## What Builds Today

- **Kernel:** Linux 5.10 (85xx/mpc85xx_cds_defconfig), PPC32 uImage + modules
- **BDE:** `nos_kernel_bde.ko`, `nos_user_bde.ko` — PCI probe, BAR0, 8MB DMA pool, S-Channel submit (DMA path), ioctl READ_REG/WRITE_REG/GET_DMA_INFO/SCHAN_OP, mmap DMA
- **SDK:** `libbcm56846.so` — attach/detach/init, BDE ioctl layer, schan_write/read, reg_read32/reg_write32; port/L2/L3/ECMP/VLAN/pktio stubs
- **nos-switchd:** PPC32 executable — attach, init, sleep loop (no TUN/netlink yet)
- **Tests:** `bde_validate` — READ_REG(0), mmap DMA write/read, READ_REG(0x32800)

**Build command:** `USE_BUILD_SERVER=modern BUILD_KERNEL=1 ./scripts/build-on-build-server.sh`  
**Artifacts:** See [BUILD.md](BUILD.md).

---

## Plan Progress (from PLAN.md)

### Phase 1 — Boot + Kernel + Our BDE
- **1a:** [x] Kernel 5.10 PPC32 built. [ ] DTB, [ ] initramfs packed, [ ] FIT, [ ] boot on target
- **1b:** [x] nos-kernel-bde.ko (PCI, BAR0, DMA, S-Channel, exports)
- **1c:** [x] nos-user-bde.ko (/dev/nos-bde, ioctls, mmap)
- **1d:** [x] BDE validation test (bde_validate). [ ] Run on target → Passed 3/3

### Phase 2 — Custom SDK (libbcm56846)
- **2a:** [x] S-Channel and register access (BDE ioctl)
- **2b–2g:** [ ] ASIC init, port bringup, L2, L3/ECMP, pktio, VLAN (stubs in place)

### Phase 3 — nos-switchd
- [ ] TUN creation, netlink listener, link-state polling, TX/RX threads

### Later
- Phase 4 (FRR integration), Phase 5 (platform), Phase 6 (ONIE installer)

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
├── initramfs/          # init script, build.sh (scaffolding)
├── boot/               # nos.its FIT template, README (DTB instructions)
├── rootfs/             # README only
├── platform/           # README only
└── onie-installer/     # README only
```

---

## How to Run Validation on Target

1. Copy to AS5610: `bde_validate`, `nos_kernel_bde.ko`, `nos_user_bde.ko`.
2. Load BDE: `insmod nos_kernel_bde.ko`, `insmod nos_user_bde.ko`.
3. Run: `./bde_validate` → expect **Passed 3/3**.

Without hardware, the binary can be run under QEMU on the build server (`qemu-ppc-static -L /usr/powerpc-linux-gnu ./bde_validate`); it will fail on open(/dev/nos-bde) as expected.
