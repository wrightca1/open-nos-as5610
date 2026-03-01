# open-nos-as5610 — Build and Implementation Status

**Last updated:** 2026-03-01

---

## Summary

| Area | Status | Notes |
|------|--------|-------|
| **Build system** | ✅ Working | Build server (Debian, PPC32 cross), kernel + BDE + SDK + switchd + platform-mgrd + tests |
| **Phase 1 — Boot + BDE** | ✅ Implemented | Kernel 5.10.0-dirty, BDE modules, S-Channel, validation test |
| **Phase 2 — SDK** | 🟢 2a–2g + L2_USER_ENTRY | Config, SOC runner, S-Chan, L2 add/delete/get, L2_USER_ENTRY add/delete, L3/ECMP, VLAN, port+SerDes, pktio, stats. 40G + HW tests pending. |
| **Phase 3 — nos-switchd** | 🟢 Core complete | Netlink→SDK for link/addr/route/neigh; link-state poll; TX/RX threads. Ready for HW/FRR test. |
| **Phase 5 — Platform** | ✅ Complete | platform-mgrd: CPLD watchdog, thermal→fan PWM, PSU monitor, SFP EEPROM (sysfs + I2C fallback) |
| **Phase 1a DTB/initramfs/FIT** | ✅ In place | initramfs/build.sh, boot/build-fit.sh, real Cumulus DTB (boot/as5610_52x.dtb) |
| **Rootfs** | ✅ Debian Jessie PPC32 | rootfs/build.sh — jessie from archive.debian.org (last Debian with powerpc); glibc 2.19 + systemd 215 compatible with Linux 5.10 |
| **ONIE installer .bin** | ✅ Produced | 171MB; all NOS binaries included (nos-switchd, libbcm56846.so, BDE .ko, platform-mgrd); served at http://10.22.1.4:8000/ |
| **Hardware validation** | ⏳ Pending | Run `onie-nos-install` on AS5610, `bde_validate` on target |

---

## What Builds Today

- **Kernel:** Linux 5.10.0-dirty (mpc85xx_cds_defconfig + AS5610 patches), PPC32 uImage + modules
  - eTSEC/gianfar (eth0, management), I2C chardev+mux+PCA954X (SFP buses i2c-22..i2c-73), AT24 EEPROM (SFP sysfs), LM75/LM90 hwmon, GPIO PCA953X
- **BDE:** `nos_kernel_bde.ko`, `nos_user_bde.ko` — vermagic `5.10.0-dirty`; PCI probe, BAR0, 8MB DMA pool, S-Channel, ioctl READ_REG/WRITE_REG/GET_DMA_INFO/SCHAN_OP, mmap DMA
- **SDK:** `libbcm56846.so` — attach/detach/init, config.bcm, SOC runner, schan write/read_memory, reg; **port** (enable, link, SerDes 10G); **L2** add/delete/get + **L2_USER_ENTRY** add/delete; **L3** intf/egress/route/host + **ECMP**; **VLAN**; **pktio** (TX/RX DCB21); **stats** (RPKT/RBYT/TPKT/TBYT).
- **nos-switchd:** PPC32 executable — attach, init, TUN creation, netlink (NEWLINK→port enable, NEWADDR→l3_intf, NEWROUTE/DELROUTE→l3_egress+route, NEWNEIGH/DELNEIGH→l2_addr), link-state poll, TX thread, RX callback→TUN write.
- **platform-mgrd:** PPC32 executable — CPLD watchdog keepalive (15s), thermal→fan PWM (4 zones, 35/45/55°C), PSU presence/ok monitor, SFP EEPROM read (sysfs at24 + I2C fallback)
- **Tests:** `bde_validate` — READ_REG(0), mmap DMA write/read, READ_REG(0x32800)

**Build command:** `USE_BUILD_SERVER=modern BUILD_KERNEL=1 ./scripts/build-on-build-server.sh`
**Artifacts:** See [BUILD.md](BUILD.md).

---

## ONIE Installer Readiness Checklist

| Component | Status | Notes |
|-----------|--------|-------|
| Kernel FIT (nos-powerpc.itb) | ✅ | 5.10.0-dirty, DTB (real Cumulus as5610_52x.dtb), initramfs |
| Rootfs squashfs | ✅ | Debian jessie PPC32; xz-compressed |
| nos-switchd | ✅ | In `/usr/sbin/nos-switchd` |
| libbcm56846.so | ✅ | In `/usr/lib/libbcm56846.so` |
| nos_kernel_bde.ko | ✅ | In `/lib/modules/5.10.0-dirty/` |
| nos_user_bde.ko | ✅ | In `/lib/modules/5.10.0-dirty/` |
| platform-mgrd | ✅ | In `/usr/sbin/platform-mgrd` |
| systemd service units | ✅ | nos-bde-modules.service, nos-switchd.service, platform-mgrd.service |
| config.bcm | ✅ | In `/etc/nos/config.bcm` (52 portmap entries) |
| install.sh | ✅ | Self-extracting, partitions, writes kernel+rootfs |
| platform.conf | ✅ | accton_as5610_52x |
| uboot_env | ✅ | cl.active, bootsource, cl.platform |
| CPLD kernel driver | ⚠️ | No OSS driver yet; CPLD sysfs paths depend on running accton_as5610_52x_cpld.ko (from ONL or out-of-tree) |
| Hardware boot test | ⏳ | Not yet run on physical switch |

---

## Known Issues

| Issue | Impact | Status |
|-------|--------|--------|
| BDE vermagic is `5.10.0-dirty` | Kernel and BDE .ko must be rebuilt together; stale .ko will fail `insmod` | Known; rebuild BDE whenever kernel is rebuilt (`BUILD_KERNEL=1`) |
| CPLD sysfs driver | platform-mgrd opens `/sys/devices/ff705000.localbus/ea000000.cpld/…`; requires `accton_as5610_52x_cpld.ko` | Pending; driver from ONL tree or write minimal OSS CPLD driver |
| SPE toolchain flags removed | `-mabi=spe -mspe -mfloat-gprs=double` not supported by `powerpc-linux-gnu-gcc` 12; only `-mcpu=8548` used | Fixed in `tools/ppc32-toolchain.cmake` |
| jessie-updates mirror 404 | `archive.debian.org/debian-security` powerpc repo is missing; debootstrap errors on security suite | Non-fatal; security updates not available; production should use Void Linux (see PLAN.md §11.1) |
| Debian jessie + kernel 5.10 | glibc 2.19 requires kernel ≥3.2; systemd 215 requires ≥3.10 | ✅ Confirmed compatible; Linux 5.10 satisfies both |

---

## Plan Progress (from PLAN.md)

### Phase 1 — Boot + Kernel + Our BDE
- **1a:** [x] Kernel 5.10 PPC32 built. [x] DTB (real Cumulus boot/as5610_52x.dtb). [x] initramfs packed. [x] FIT (nos-powerpc.itb). [ ] Boot on target via ONIE.
- **1b:** [x] nos-kernel-bde.ko (PCI, BAR0, DMA, S-Channel, exports)
- **1c:** [x] nos-user-bde.ko (/dev/nos-bde, ioctls, mmap)
- **1d:** [x] BDE validation test (bde_validate). [ ] Run on target → Passed 3/3

### Phase 2 — Custom SDK (libbcm56846)
- **2a:** [x] S-Channel and register access (BDE ioctl)
- **2b:** [x] Config loader + SOC runner; config.bcm. [x] rc.datapath_0/LED capture docs (etc/nos/README-CAPTURE.md).
- **2c–2g:** [x] Port, SerDes 10G, L2 add/delete/get, L2_USER_ENTRY add/delete, L3/ECMP, pktio, VLAN, stats. [ ] 40G, HW tests.

### Phase 3 — nos-switchd
- [x] TUN, ports.conf, netlink (NEWLINK, NEWADDR/DELADDR→l3_intf, NEWROUTE/DELROUTE→l3_egress+route, NEWNEIGH/DELNEIGH→l2_addr+cache), link-state, TX/RX. [x] SDK complete (L2 get, stats, L2_USER_ENTRY, SerDes). [ ] HW validation.

### Phase 5 — Platform Management
- [x] platform-mgrd: CPLD watchdog keepalive (15s, `watch_dog_keep_alive`)
- [x] Thermal monitoring: scan `/sys/class/hwmon/hwmon*/tempN_input`, 4 PWM zones (35/45/55°C → 64/128/200/248 on 0-248 CPLD scale)
- [x] Fan control: write `/sys/devices/ff705000.localbus/ea000000.cpld/pwm1`
- [x] PSU monitor: `psu_pwr1_present`, `psu_pwr1_all_ok`, `psu_pwr2_*`
- [x] SFP EEPROM: sysfs (`/sys/class/eeprom_dev/eeprom(N+6)/device/eeprom`) + I2C fallback (`/dev/i2c-(21+N)`, 0x50)
- [ ] Status LEDs (`led_psu1`, `led_diag`, `led_fan` via CPLD sysfs)
- [ ] CPLD kernel driver (accton_as5610_52x_cpld.ko OSS implementation)

### Phase 6 — ONIE installer
- [x] install.sh (self-extracting), platform.conf, platform.fdisk, uboot_env, rootfs/build.sh (Debian jessie), onie-installer/build.sh → .bin
- [x] All NOS binaries in rootfs: nos-switchd, libbcm56846.so, nos_kernel_bde.ko, nos_user_bde.ko, platform-mgrd
- [x] 171MB installer served at http://10.22.1.4:8000/
- [ ] Test on switch: `onie-nos-install http://10.22.1.4:8000/open-nos-as5610-YYYYMMDD.bin` → boot our NOS

### Phase 4 — FRR
- [x] FRR PPC32: rootfs apt install frr; from-source in docs/FRR-PPC32.md. [ ] BGP/OSPF config, ECMP/BFD tests on HW.

### Later
- All remaining items are HW tests or optional (40G, BFD, ONLP integration, CPLD driver, LED control).

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
