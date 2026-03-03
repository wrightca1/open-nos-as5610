# AS5610-52X Open NOS — Status Report
**Date:** 2026-03-02 (updated session 2)
**Switch:** Accton/Edgecore AS5610-52X (PowerPC P2020, BCM56846)
**SSH:** root@10.1.1.233
**Kernel:** Linux 5.10.0-dirty #4, ppc, built 2026-03-01
**Rootfs:** Debian GNU/Linux 8 (jessie) + Open NOS overlay

---

## Session 2 Changes

| Component | Change | Result |
|-----------|--------|--------|
| `platform/drivers/accton_as5610_cpld.c` | NEW — CPLD kernel driver | Fans/LEDs controllable via sysfs |
| `platform/drivers/accton_as5610_cpld.ko` | NEW — pre-built PPC32 .ko (vermagic: 5.10.0-dirty) | Deployed; CPLD sysfs at `/sys/devices/platform/ff705000.localbus/ea000000.cpld/` |
| `platform/platform-mgrd/main.c` | Fixed CPLD_BASE path (`/platform/` missing) | C daemon reads/writes CPLD |
| `platform/platform-mgrd/platform-mgrd-bin` | NEW — statically linked PPC32 binary | Fans running at 70% |
| `switchd/src/tun.c` | `IFF_TUN` → `IFF_TAP` | swp interfaces have `link/ether` MACs |
| `switchd/src/main.c` | Added `bcm56846_port_enable_set()` loop | Port enable called at startup |
| `rootfs/overlay/etc/systemd/system/nos-bde-modules.service` | Added CPLD driver load | CPLD driver loaded at boot |
| `rootfs/build.sh` | Added CPLD .ko to rootfs copy | Persistent in next build |

---

## Summary

Session 1: The core NOS stack boots. BCM56846 detected, BDE functional. 52 port interfaces created.
Session 2: CPLD kernel driver deployed → fan control at 70%. TAP interfaces deployed → swp ports have
Ethernet MAC addresses and IPs. `platform-mgrd` service running.

**Remaining blocker:** BCM56846 ASIC RX = 0 packets (physical forwarding not working).
Root causes identified via reverse engineering:
1. **S-Channel broken** — `nos_kernel_bde.c` uses DMA channel 0 for S-Channel (wrong). SCHAN_MSG
   registers confirmed at BAR0+0x3300c–0x33060 (from Broadcom binary strings). Fix: use SCHAN_MSG.
2. **`init all` not implemented** — `sdk/src/soc.c` silently skips `init all` in Cumulus rc.soc.
3. **Named register setreg not handled** — rc.soc uses `setreg xmac_tx_ctrl 0xc802` etc.

---

## Checklist

### Boot & Kernel

- [x] ONIE installer boots on physical hardware
- [x] Linux 5.10.0-dirty kernel starts (`ppc`, P2020, verified via `uname -a`)
- [x] systemd init completes, reaches multi-user.target
- [x] Root filesystem mounted (squashfs overlay, 3.3 GB, 1% used)
- [x] Persist partition mounted (`/dev/sda1` → `/mnt/persist`, 124 MB)
- [x] Memory healthy (753 MB total, 70 MB used at idle)
- [ ] System clock — **BROKEN**: stuck at 1970-01-01, no RTC accessible, no NTP running
- [ ] `proc-sys-fs-binfmt_misc.automount` — **FAILED**: `autofs4` kernel module missing

---

### Hardware / ASIC

- [x] BCM56846 detected on PCIe bus (`a000:01:00.0`, device ID `14e4:b846`)
- [x] BCM56846 BAR0 mapped (`0xa0000000–0xa003ffff`, 64-bit)
- [x] DMA pool allocated (4 MB at `0x03000000`)
- [x] CPLD present (`ea000000.cpld` in platform bus)
- [x] I2C bus drivers loaded (`at24`, `lm75`, `lm90`, `dummy`)
- [ ] I2C devices enumerated — **MISSING**: no devices visible in `/sys/bus/i2c/devices/`
- [ ] hwmon sensors — **MISSING**: no `/sys/class/hwmon/` entries; no temp or fan readings
- [ ] Hardware clock / RTC — **NO ACCESS**: `hwclock` reports no method found
- [x] CPLD kernel driver — `accton_as5610_cpld.ko` loaded, sysfs at `/sys/devices/platform/ff705000.localbus/ea000000.cpld/`
- [x] Fan control — **WORKING**: `platform-mgrd` running, fans at 70% via CPLD `pwm1`
- [ ] Temperature monitoring — **PARTIAL**: CPLD accessible, but I2C temp sensors not enumerated (no hwmon); fans set to safe 70% as default
- [ ] LED control — **PARTIAL**: CPLD `led_diag` exposed via sysfs; platform-mgrd sets green at startup

---

### NOS Kernel Modules (BDE)

- [x] `nos_kernel_bde.ko` — loaded successfully
- [x] `nos_user_bde.ko` — loaded successfully
- [x] `/dev/nos-bde` character device created (major 253, minor 0)
- [x] BDE log: `BCM56846 at [mem 0xa0000000-0xa003ffff 64bit], BAR0 d3a12c7c, DMA 0x03000000 size 4194304`

---

### NOS Switch Daemon (`nos-switchd`)

- [x] `nos-switchd` binary present (`/usr/sbin/nos-switchd`, 931 KB, statically linked)
- [x] Service starts and stays running (running since boot)
- [x] 52 **TAP** interfaces created (`swp1..swp52`) — `BROADCAST,MULTICAST`, Ethernet MAC addresses (IFF_TAP fix)
- [x] netlink, link-state, and TX threads started
- [x] Port enable called for all 52 ports (but S-Channel is broken so writes don't reach ASIC)
- [ ] BCM56846 ASIC initialization — **NOT IMPLEMENTED**: S-Channel transport broken in kernel BDE (uses DMA ch0 instead of SCHAN_MSG at 0x3300c); `init all` no-op in soc.c
- [ ] Physical port link state — **NOT IMPLEMENTED**: all ports show DOWN (ASIC port enable failing silently)
- [ ] Packet forwarding — **NOT WORKING**: ASIC RX = 0 packets; S-Channel must be fixed first
- [ ] Port statistics from ASIC — **NOT IMPLEMENTED**: all counters zero

---

### Switch Port Interfaces

- [x] `swp1` — UP, `10.101.101.1/29`, MTU 1600, TAP, MAC address assigned
- [x] `swp2` — UP, `10.101.101.10/29`, MTU 1600, TAP, MAC address assigned
- [x] `swp3`–`swp52` — TAP interfaces exist (DOWN, unconfigured, expected)
- [ ] Physical link detection — **NOT IMPLEMENTED** (all ports show DOWN; ASIC port enable writes failing)
- [ ] ARP / ping through swp ports — **NOT WORKING** (ASIC forwarding plane not initialized)

---

### Management Plane / Networking

- [x] `eth0` — UP, DHCP, `10.1.1.233/24`, MAC `80:a2:35:81:ca:ae`
- [x] `lo` — UP, `127.0.0.1/8` + `10.101.101.241/32`
- [x] Default route via `10.1.1.1` installed
- [x] Ping to gateway (`10.1.1.1`) — 0% loss, <1 ms RTT
- [x] SSH daemon — running, key auth working
- [ ] NTP / time sync — **NOT CONFIGURED**: no NTP client running; time shows 1970
- [ ] `/etc/default/locale` — **MISSING**: minor PAM error on every SSH login

---

### System Services

| Service | Status | Notes |
|---------|--------|-------|
| `nos-bde-modules.service` | ✅ active (exited) | Both .ko modules loaded |
| `nos-switchd.service` | ✅ active (running) | 52 swp TUN interfaces up |
| `nos-boot-success.service` | ✅ active (exited) | u-boot boot_count reset |
| `ssh.service` | ✅ active (running) | Key auth confirmed |
| `lldpd.service` | ✅ active (running) | Minor warnings (no lldpd.conf, no system name) |
| `rsyslog.service` | ✅ active (running) | — |
| `cron.service` | ✅ active (running) | — |
| `platform-mgrd.service` | ✅ active (running) | C daemon running; fans at 70%; CPLD sysfs |
| `proc-sys-fs-binfmt_misc.automount` | ❌ failed | `autofs4` kernel module not built |

---

## Issues to Fix (Prioritized)

### High Priority (Session 3)

1. **S-Channel broken in kernel BDE** — `nos_kernel_bde.c::nos_bde_schan_op()` uses
   DMA channel 0 as transport (wrong). Correct protocol: write cmd to SCHAN_MSG registers
   at BAR0+0x3300c–0x33060 (confirmed from Broadcom binary strings), write 1 to
   CMIC_CMC0_SCHAN_CTRL (BAR0+0x32800), poll for DONE bit 1. Fix requires kernel module
   rebuild + redeploy.

2. **`init all` not implemented** — rc.soc begins with `init all` which our `soc.c` parser
   ignores. Need to implement it as a call to `bcm56846_chip_init()` that clears stale
   SCHAN state and performs minimal CMIC bringup (the ASIC ring maps may be intact
   from previous Cumulus boot since we do warm reboots).

3. **`rcload` not implemented** — rc.soc calls `rcload /etc/bcm.d/rc.ports_0` and
   `rcload /var/lib/cumulus/rc.datapath_0` which are silently ignored. Need to
   implement recursive script loading in `soc.c`.

4. **Named register `setreg`** — rc.soc uses `setreg xmac_tx_ctrl 0xc802`,
   `setreg IFP_METER_PARITY_CONTROL 0`, etc. Our parser only handles `setreg 0xHEX val`.
   Need a register name lookup table or soft-fail with a warning.

### Medium Priority

5. **System time / NTP** — Time stuck at 1970. Add `ntpdate` or `systemd-timesyncd`
   to rootfs and configure NTP servers.

6. **I2C devices not enumerated** — I2C buses are present but no slaves registered.
   platform-mgrd is using fixed 70% fan speed (safe) until hwmon sensors are accessible.

7. **`/etc/default/locale` missing** — Creates a PAM warning on every SSH session.
   Fix: create the file in `rootfs/overlay/etc/default/locale`.

### Low Priority

7. **`autofs4` / binfmt_misc** — `proc-sys-fs-binfmt_misc.automount` fails. Add
   `CONFIG_AUTOFS4_FS=y` to kernel config or mask the unit.

8. **`lldpd.conf` missing** — lldpd logs config warnings. Add a minimal
   `/etc/lldpd.conf` to the rootfs overlay.

9. **No swap** — Not critical (682 MB free RAM) but worth noting.

---

## Raw Data Snapshot

```
Kernel:    Linux as5610 5.10.0-dirty #4 Sun Mar  1 22:22:45 EST 2026 ppc GNU/Linux
Memory:    753 MB total, ~80 MB used
Disk:      3.3 GB overlay (1% used), 124 MB /mnt/persist (0% used)
eth0 MAC:  80:a2:35:81:ca:ae
eth0 IP:   10.1.1.233/24 (DHCP)
Gateway:   10.1.1.1 (reachable, <1ms)
PCI ASIC:  a000:01:00.0 [14e4:b846] BCM56846
BDE DMA:   0x03000000, 4 MB
CPLD:      ea000000.cpld, sysfs at /sys/devices/platform/ff705000.localbus/ea000000.cpld/
swp count: 52 (swp1–swp52, all TAP with MAC addresses — session 2 fix)
Fan speed: 70% via CPLD pwm1 (platform-mgrd C binary running)
S-Channel: BROKEN — kernel BDE uses DMA ch0 instead of SCHAN_MSG at 0x3300c (session 3 to fix)
```
