# open-nos-as5610 — Build and Implementation Status

**Last updated:** 2026-03-10 (session 2)

---

## Summary

| Area | Status | Notes |
|------|--------|-------|
| **Build system** | ✅ Working | Build server (Debian, PPC32 cross), kernel + BDE + SDK + switchd + platform-mgrd + tests |
| **Phase 1 — Boot + BDE** | ✅ Implemented | Kernel 5.10.0-nos, BDE modules, S-Channel, validation test |
| **Phase 2 — SDK** | 🟢 2a–2g + L2_USER_ENTRY | Config, SOC runner, S-Chan, L2 add/delete/get, L2_USER_ENTRY add/delete, L3/ECMP, VLAN, port+SerDes, pktio, stats. All modules migrated to sbus.h (proper SCHAN). XMAC access working. **DS100DF410 retimer unmuting + warm-boot firmware skip + PCS latched-low double-read added.** 40G + HW tests pending. |
| **Phase 3 — nos-switchd** | 🟢 Core complete | Netlink→SDK for link/addr/route/neigh; link-state poll; TX/RX threads. Ready for HW/FRR test. |
| **Phase 5 — Platform** | ✅ Complete | platform-mgrd: CPLD watchdog, thermal→fan PWM, PSU monitor, SFP EEPROM (sysfs + I2C fallback) |
| **Phase 1a DTB/initramfs/FIT** | ✅ In place | initramfs/build.sh, boot/build-fit.sh, real Cumulus DTB (boot/as5610_52x.dtb) |
| **Rootfs** | ✅ Debian Jessie PPC32 | rootfs/build.sh — jessie from archive.debian.org (last Debian with powerpc); glibc 2.19 + systemd 215 compatible with Linux 5.10 |
| **ONIE installer .bin** | ✅ Produced | 171MB; all NOS binaries included (nos-switchd, libbcm56846.so, BDE .ko, platform-mgrd); served at http://10.22.1.4:8000/ |
| **Hardware validation** | 🟡 In progress | SCHAN, I2C, thermal, fans, CPLD all verified working after reboot. I2C mux (PCA954x) loads at boot, hwmon sensors bind, thermal fan control active. XMAC access confirmed. SerDes SIGDET on 3 PHYs. **WARPcore firmware v0x0101 loaded on all PHYs** via MDIO serial. CL49 BLOCK_LOCK=1 achieved in loopback. **Retimer unmuting automated** (DS100DF410 OUT_MUX cleared during SerDes init). **Warm-boot firmware skip** (detects v0x0101 already loaded). **PCS latched-low double-read** for accurate link status. L2 flooding + VLAN 1 configured. External 10G link pending valid 10G peer. |

---

## What Builds Today

- **Kernel:** Linux 5.10.0-nos (mpc85xx_cds_defconfig + AS5610 patches), PPC32 uImage + modules
  - eTSEC/gianfar (eth0, management), I2C MPC + PCA954x mux (70 buses), AT24 EEPROM (SFP sysfs), ADM1021/MAX6697 hwmon, GPIO PCA953x
- **BDE:** `nos_kernel_bde.ko`, `nos_user_bde.ko` — vermagic `5.10.0-nos`; PCI probe, BAR0, 8MB DMA pool, S-Channel, ioctl READ_REG/WRITE_REG/GET_DMA_INFO/SCHAN_OP, mmap DMA
- **SDK:** `libbcm56846.so` — attach/detach/init, config.bcm, SOC runner, sbus SCHAN (mem_read/write, reg_read/write); **port** (enable, link, SerDes 10G via WARPcore MDIO + CL45 PCS link); **SerDes** (WARPcore 8051 firmware download via MDIO serial, 10G SFI forced speed, 64B/66B sync words, CL49 PCS, **DS100DF410 retimer unmuting via I2C**, **warm-boot firmware skip**, **PCS latched-low double-read**); **I2C** (userspace /dev/i2c-N helper for retimer access); **L2** add/delete/get + **L2_USER_ENTRY** add/delete; **L3** intf/egress/route/host + **ECMP**; **VLAN**; **pktio** (TX/RX DCB21, CMICe DMA); **stats** (RPKT/RBYT/TPKT/TBYT). All modules use proper sbus.h SCHAN transport.
- **nos-switchd:** PPC32 executable — attach, init, TUN creation, netlink (NEWLINK→port enable, NEWADDR→l3_intf, NEWROUTE/DELROUTE→l3_egress+route, NEWNEIGH/DELNEIGH→l2_addr), link-state poll, TX thread, RX callback→TUN write.
- **platform-mgrd:** PPC32 executable — CPLD watchdog keepalive (15s), thermal→fan PWM (4 zones, 35/45/55°C), PSU presence/ok monitor, SFP EEPROM read (sysfs at24 + I2C fallback)
- **Tests:** `bde_validate` — READ_REG(0), mmap DMA write/read, READ_REG(0x32800)

**Build command:** `BUILD_KERNEL=1 ./scripts/remote-build.sh` (on build server 10.22.1.5, inside docker)
**Artifacts:** See [BUILD.md](BUILD.md).

---

## ONIE Installer Readiness Checklist

| Component | Status | Notes |
|-----------|--------|-------|
| Kernel FIT (nos-powerpc.itb) | ✅ | 5.10.0-nos, DTB (real Cumulus as5610_52x.dtb), initramfs |
| Rootfs squashfs | ✅ | Debian jessie PPC32; xz-compressed |
| nos-switchd | ✅ | In `/usr/sbin/nos-switchd` |
| libbcm56846.so | ✅ | In `/usr/lib/libbcm56846.so` |
| nos_kernel_bde.ko | ✅ | In `/lib/modules/5.10.0-nos/` |
| nos_user_bde.ko | ✅ | In `/lib/modules/5.10.0-nos/` |
| platform-mgrd | ✅ | In `/usr/sbin/platform-mgrd` |
| systemd service units | ✅ | nos-bde-modules.service, nos-switchd.service, platform-mgrd.service |
| config.bcm | ✅ | In `/etc/nos/config.bcm` (52 portmap entries) |
| install.sh | ✅ | Self-extracting, partitions, writes kernel+rootfs |
| platform.conf | ✅ | accton_as5610_52x |
| uboot_env | ✅ | cl.active, bootsource, cl.platform |
| CPLD kernel driver | ✅ | accton_as5610_cpld.ko built out-of-tree, loads at boot |
| i2c-mux-pca954x.ko | ✅ | Copied from kernel build; required for 70 I2C buses (SFP, retimer, hwmon) |
| MTD/CFI modules | ✅ | Copied from kernel build; needed for fw_setenv (boot_count reset) |
| Hardware boot test | ✅ | Boots on AS5610-52X via ONIE |

---

## Hardware Validation Progress (AS5610-52X at 10.1.1.233)

| Step | Status | Notes |
|------|--------|-------|
| BDE modules load | ✅ | `nos_kernel_bde.ko` + `nos_user_bde.ko` load cleanly |
| SCHAN PIO mode | ✅ | Cold power cycle; CMICe SCHAN at BAR0+0x0050 |
| SBUS ring map | ✅ | 0x204..0x220 programmed; ring map reads back 0x43052100/0x33333343 |
| LINK40G_ENABLE | ✅ | 0x1c CMIC_MISC_CONTROL bit 0 set; required for XLMAC SBUS access |
| I2C / thermal | ✅ | 70 buses, MAX1617+MAX6697 hwmon, SFP EEPROMs, PCA954x muxes |
| CPLD / fans / PSU | ✅ | accton_as5610_cpld.ko loaded at boot; platform-mgrd: watchdog, thermal fan PWM (4 zones), PSU monitor, LED control. Verified across reboots. |
| 52 TAP interfaces | ✅ | swp1..swp52 as TUN/TAP; verified via `ip link show` |
| DS100DF410 retimer | ✅ | **Automated unmuting** in SerDes init (I2C bus 10-13, addr 0x27, reg 0x1E=0x00). Verified: OUT_MUX reads 0xE9 on cold boot → cleared to 0x00. 3 SFPs with RX light → SIGDET on WARPcore PHYs 13,17,31 |
| WARPcore MDIO | ✅ | MIIM CTRL=0x50, PARAM=0x158, ADDR=0x4A0; PRBS/HiGig2 cleared |
| SerDes SIGDET | ✅ | SIGDET=0x1 on PHY 13,17,31 (MDIO bus 1) with SFP+ installed |
| XLPORT/XMAC init | ✅ | xport_reset → XLPORT_MODE → PORT_ENABLE → XMAC_CONTROL; TX_CTRL reads 0xc802 |
| VLAN 1 + STG | ✅ | VLAN_TABm, EGR_VLANm, STG_TABm configured; PORT_VID=1 default |
| L2 flooding | ✅ | UNKNOWN_UCAST/MCAST_BLOCK_MASK zeroed (flood to all VLAN ports) |
| WARPcore firmware | ✅ | 8051 firmware v0x0101 loaded on all PHYs via MDIO serial download. **Warm-boot safe**: detects v0x0101 already loaded and skips download. Aggressive 8051 reset (STOP+10ms) for clean re-download when needed. |
| CL49 block lock | ✅ (loopback) | BLOCK_LOCK=1 achieved in IEEE loopback with correct forced speed (FV_fdr_10G_SFI=0x29) + 64B/66B sync words. Retimer unmuting now automated. |
| Port link (10G) | 🟡 | CL45 PCS link status with **latched-low double-read** (devad=3 reg=1 bit2). Retimer unmuting working. Pending valid 10G peer for external link-up. |
| L2/L3 forwarding | ⏳ | Pending port link-up; ASIC datapath fully configured |

**Warm-boot note**: CMC2 remains in DMA ring-buffer mode after warm reboot; PIO SCHAN requires
cold hardware power cycle (unplug + replug). WARPcore firmware download now handles warm boot
correctly: detects v0x0101 already loaded and skips download; aggressive 8051 STOP+reset for
cases requiring re-download.

**NOS_BDE_WRITE_REG ioctl**: Source uses `_IOR` (not `_IOW`) to match the deployed `.ko` binary.

---

## Known Issues

| Issue | Impact | Status |
|-------|--------|--------|
| BDE vermagic `5.10.0-nos` | Kernel and BDE .ko must be rebuilt together; stale .ko will fail `insmod` | Known; rebuild BDE whenever kernel is rebuilt (`BUILD_KERNEL=1`) |
| WRITE_REG ioctl direction | Deployed .ko uses `_IOR` (0x80084202); source has `_IOW` | Workaround in `bde_ioctl.c`; needs .ko rebuild |
| UCMEM SCHAN WRITE_MEM fails | SCHAN writes to XLPORT UCMEM complete with DONE but data readback is zeros | Workaround: firmware loaded via MDIO serial instead; root cause unknown |
| External 10G link pending | CL49 BLOCK_LOCK works in loopback; retimer unmuting now automated | Need valid 10G peer sending 64B/66B for external link-up |
| Retimer CDR never locks | DS100DF410 CDR status bit4=0 on all channels despite signal | May not be in active signal path; retimer output unmuted but CDR/EQ may be passthrough only |
| fw_setenv not working | MTD modules load but physmap finds no flash (/proc/mtd empty) | LBC/ELBC controller may not be probed; CONFIG_MTD_PHYSMAP_OF may need =y not =m |
| boot_count not reset | NOS cannot reset U-Boot boot_count; >3 reboots → ONIE | Blocked by fw_setenv; nos-boot-success.sh exists but MTD backend not working |
| PHY 1 bus 0 firmware | Always fails download (SERDESID0=0x0000) | May not exist; doesn't affect 10G SFP+ ports |
| CPLD sysfs driver | platform-mgrd opens `/sys/devices/…/ea000000.cpld/`; requires CPLD .ko | Working with accton_as5610_cpld.ko; loads at boot via nos-bde-modules.service |
| IFP_METER_PARITY_CONTROLr | SCHAN write to 0x0a400000 fails; IFP block may not exist on BCM56846 | Non-critical; IFP meter parity is an optional errata workaround |
| CMICe DMA CTRL model | CMIC_DMA_CTRL (0x100) is a single register with per-channel bit fields, not per-channel registers like CMICm | pktio.c CMICM_DMA_CTRL(ch) alias ignores channel — may need channel-aware bit manipulation |

---

## Plan Progress (from PLAN.md)

### Phase 1 — Boot + Kernel + Our BDE
- **1a:** [x] Kernel 5.10 PPC32 built. [x] DTB (real Cumulus boot/as5610_52x.dtb). [x] initramfs packed. [x] FIT (nos-powerpc.itb). [ ] Boot on target via ONIE.
- **1b:** [x] nos-kernel-bde.ko (PCI, BAR0, DMA, S-Channel, exports)
- **1c:** [x] nos-user-bde.ko (/dev/nos-bde, ioctls, mmap)
- **1d:** [x] BDE validation test (bde_validate). [x] Run on target → SCHAN working; XLPORT reset pending

### Phase 2 — Custom SDK (libbcm56846)
- **2a:** [x] S-Channel and register access (BDE ioctl)
- **2b:** [x] Config loader + SOC runner; config.bcm. [x] rc.datapath_0/LED capture docs (etc/nos/README-CAPTURE.md).
- **2c–2g:** [x] Port, SerDes 10G, L2 add/delete/get, L2_USER_ENTRY add/delete, L3/ECMP, pktio, VLAN, stats. [x] All modules migrated from schan_write_memory to sbus.h (proper SCHAN transport). [x] XLPORT init (xport_reset + MODE + ENABLE + XMAC_CONTROL). [x] VLAN/STG/L2 flooding in init_datapath. [x] CMICe DMA register offsets. [ ] 40G, HW tests.

### Phase 3 — nos-switchd
- [x] TUN, ports.conf, netlink (NEWLINK, NEWADDR/DELADDR→l3_intf, NEWROUTE/DELROUTE→l3_egress+route, NEWNEIGH/DELNEIGH→l2_addr+cache), link-state, TX/RX. [x] SDK complete (L2 get, stats, L2_USER_ENTRY, SerDes). [ ] HW validation.

### Phase 5 — Platform Management
- [x] platform-mgrd: CPLD watchdog keepalive (15s, `watch_dog_keep_alive`)
- [x] Thermal monitoring: scan `/sys/class/hwmon/hwmon*/tempN_input`, 4 PWM zones (35/45/55C thresholds, sysfs 96/168/248 on 0-248 scale)
- [x] Fan control: write `/sys/devices/ff705000.localbus/ea000000.cpld/pwm1`; verified working across reboots with correct sysfs→CPLD raw scaling
- [x] PSU monitor: `psu_pwr1_present`, `psu_pwr1_all_ok`, `psu_pwr2_*`
- [x] SFP EEPROM: sysfs (`/sys/class/eeprom_dev/eeprom(N+6)/device/eeprom`) + I2C fallback (`/dev/i2c-(21+N)`, 0x50)
- [x] Status LEDs (`led_psu1`, `led_psu2`, `led_diag`, `led_fan` via CPLD sysfs) — driven by platform-mgrd based on PSU/fan health
- [x] CPLD kernel driver (accton_as5610_cpld.ko) — built out-of-tree, loads at boot via nos-bde-modules.service
- [x] I2C mux module (i2c-mux-pca954x.ko) — loads first in boot sequence; required for hwmon sensor binding
- [x] Line-buffered stdout for journald logging (setvbuf fix)

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
