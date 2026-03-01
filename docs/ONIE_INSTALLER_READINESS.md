# ONIE Installer Readiness — Do We Have Everything for Our Own Debian Installer?

**Short answer: Yes.** We have all the pieces to build and run our own ONIE-installable Debian-based NOS image for the Edgecore AS5610-52X. The one-command build produces a loadable `.bin`; what remains is hardware validation (install on switch and confirm boot).

---

## Checklist: What We Have

| Component | Status | Where |
|-----------|--------|--------|
| **Installer format** | ✅ | Self-extracting script + `__ARCHIVE__` + `data.tar` (same idea as Cumulus; we use data.tar-only payload). [ONIE_AND_INSTALLER_FORMATS_DEEP_DIVE.md](../../docs/reverse-engineering/ONIE_AND_INSTALLER_FORMATS_DEEP_DIVE.md) |
| **install.sh** | ✅ | Extracts payload, detects platform (`onie-sysinfo -p` / accton_as5610_52x), partitions (fdisk), formats (ext2), dd’s kernel + rootfs to both slots, sets U-Boot env, reboot. `onie-installer/install.sh` |
| **build.sh** | ✅ | Builds `data.tar` (control, platform.conf, platform.fdisk, uboot_env, FIT, squashfs), then `cat install.sh data.tar > .bin`. `onie-installer/build.sh` |
| **Partition layout** | ✅ | platform.conf + platform.fdisk for sda1 (persist), sda5/6 (slot A), sda7/8 (slot B), sda3 (rw-overlay). Matches Cumulus/ONIE layout. `onie-installer/cumulus/init/accton_as5610_52x/` |
| **U-Boot env** | ✅ | common_env.inc, as5610_52x.platform.inc; install.sh runs `fw_setenv bootsource flashboot`, `cl.active 1`, `cl.platform accton_as5610_52x`. ONIE on AS5610 provides `fw_setenv` (U-Boot tools). `onie-installer/uboot_env/` |
| **Debian rootfs** | ✅ | Debian 8 (Jessie) PPC32 — last Debian with powerpc; `debootstrap --arch powerpc` + qemu-user-static + squashfs. Build locally (Linux + debootstrap) or on build server in Docker. `rootfs/build.sh` |
| **Initramfs** | ✅ | Mounts squashfs (sda6/sda8) + overlay (sda3), then `switch_root`. Packed in FIT. `initramfs/build.sh`, `initramfs/init` |
| **FIT image** | ✅ | Kernel (uImage) + DTB + initramfs → `nos-powerpc.itb`. `boot/build-fit.sh`, `boot/nos.its` |
| **Kernel** | ✅ | Linux 5.10 PPC32 (build server or local). uImage in FIT. |
| **DTB** | ⚠️ Obtain once | Required for FIT and boot. Options: (1) Extract from Cumulus `.bin` (`dumpimage` / `scripts/extract-dtb.sh`), (2) Build from ONL `as5610_52x.dts`, (3) Minimal `boot/as5610_52x_minimal.dts` (may lack full eTSEC/PCIe). Build script uses `DTB_IMAGE` or `CUMULUS_BIN`. |
| **One-command build** | ✅ | `./scripts/build-onie-image.sh` — optional server build (kernel+BDE+SDK+switchd), DTB resolution, initramfs, FIT, rootfs (local or Docker), then `.bin`. See [BUILD.md](../BUILD.md). |

---

## Optional / Nice-to-Have

| Item | Notes |
|------|--------|
| **onie-nos-mode -s** | ONIE spec suggests installer call this on success. Our install.sh doesn’t; add after reboot path if you want “user friendly” next boot. |
| **Full DTB** | For production, use DTB from ONL or Cumulus so eTSEC (eth0), PCIe, I2C, serial are correct. Minimal DTB is enough to build the FIT; boot on real hardware may need full DTB. |

---

## What You Need to Run the Build

1. **DTB**: Either  
   - `DTB_IMAGE=/path/to/as5610_52x.dtb`, or  
   - `CUMULUS_BIN=/path/to/CumulusLinux-2.5.1-powerpc.bin` (script extracts DTB), or  
   - Build from `boot/as5610_52x_minimal.dts` (script can do this on build server or if `dtc` is installed).

2. **Rootfs build**: One of  
   - Linux host with `debootstrap`, `qemu-user-static`, `squashfs-tools` (rootfs built locally), or  
   - Reachable build server with Docker (script builds rootfs in Docker and copies back).  
   - Or `SKIP_ROOTFS=1` for a kernel-only `.bin` (for testing installer flow without full rootfs).

3. **Kernel**: Either  
   - `BUILD_SERVER=1` (default) so script builds kernel on server and copies back, or  
   - `BUILD_SERVER=0` and provide `boot/uImage` (and optionally BDE/SDK/switchd in `build/`).

---

## Conclusion

We have everything to recreate our own ONIE installer for Debian (Jessie PPC32): installer script and format, partition layout, U-Boot env, Debian rootfs build, initramfs, FIT, and one-command image build. The only external requirement is a DTB (from Cumulus/ONL or minimal .dts). After building, install on the switch with:

```bash
onie-nos-install http://<server>/open-nos-as5610-YYYYMMDD.bin
```

Hardware validation (install + boot on AS5610) is the remaining step to confirm end-to-end.

---

## Serial console after NOS boot

If the installer completes and the switch reboots but the **serial output is garbage** (random characters), the host you’re connected with (e.g. the machine with the serial cable) is not the switch — the switch is outputting at a different baud rate than your terminal.

- **Default is 115200** (8N1, no flow control). Set your serial client to 115200 to match. If you get garbage, try **9600**: edit `onie-installer/uboot_env/as5610_52x.platform.inc` (`baudrate 9600`), rebuild the installer, reinstall the `.bin`, and set your terminal to 9600.
- To change baud: edit `onie-installer/uboot_env/as5610_52x.platform.inc` and the platform YAML (`packages/.../powerpc-accton-as5610-52x-r0.yml`), then re-run the installer build. Reinstall the new `.bin` and set your terminal to the same baud.

---

## Stopped at LOADER=> (U-Boot prompt)

**LOADER=>** is this board’s U-Boot prompt (ONIE build). The switch stopped in the bootloader instead of booting the NOS.

**Try this first:** at the prompt type:

```text
run flashboot
```

That runs the same sequence as a normal boot: set bootargs, load the FIT from USB partition 5, and run `bootm`. If the NOS was installed correctly, the kernel should start.

**If that fails or you want to see why:**

1. **Don’t press any key during the first few seconds after power-on** — a keypress can interrupt the countdown and drop you to LOADER=>.
2. **Check environment:**  
   `printenv bootsource cl.platform loadaddr`  
   You should see `bootsource=flashboot`, `cl.platform=accton_as5610_52x`, `loadaddr=0x02000000`. If `bootsource` is empty, the installer’s `fw_setenv` may not have persisted; try `setenv bootsource flashboot` then `run flashboot`.
3. **Manual boot:**  
   `usb start`  
   then  
   `usbboot 0x02000000 0:5`  
   then  
   `run initargs`  
   then  
   `bootm 0x02000000#accton_as5610_52x`  
   If `usbboot` fails (e.g. "cannot find device"), the USB disk or partition 5 may not be present or the FIT wasn’t written correctly; re-run the installer.
