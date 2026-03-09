# ONIE Installer Build Guide

**Platform**: Edgecore/Accton AS5610-52X (PowerPC, U-Boot)
**Build server**: 10.22.1.5 (Debian 8 host, Docker debian:bookworm for cross-compile)
**HTTP server**: 10.22.1.4 (python3 -m http.server 8000 in ~/http-server)

---

## Build Order (MUST follow this sequence)

The ONIE installer is composed of multiple artifacts that depend on each other.
Build them **in this order** — skipping a step or using stale artifacts causes silent failures.

```
1. Kernel + BDE modules    (if kernel/config changed)
2. SDK + nos-switchd       (if any sdk/src/* or switchd/src/* changed)
3. Initramfs               (if initramfs/nos-init.c changed)
4. FIT image               (if kernel, DTB, OR initramfs changed)
5. Rootfs squashfs          (if SDK, switchd, overlay, OR packages changed)
6. ONIE installer .bin      (if FIT OR rootfs changed)
```

**Common pitfall**: Rebuilding rootfs or FIT without rebuilding initramfs when nos-init.c changed.
The FIT embeds the initramfs — if nos-init.c is modified, you MUST rebuild steps 3+4+5+6.

---

## Step-by-step

All commands run on the build server (10.22.1.5) unless noted.

### 1. Kernel + BDE (only if kernel config changed)

```bash
cd ~/open-nos-as5610
BUILD_KERNEL=1 ./scripts/remote-build.sh
# Docker fallback runs automatically on Debian 8
# Output: linux-5.10/arch/powerpc/boot/uImage, bde/*.ko
# IMPORTANT: chown -R smiley:smiley afterwards (Docker builds as root)
```

### 2. SDK + nos-switchd

```bash
cd ~/open-nos-as5610
./scripts/remote-build.sh
# Output: build/sdk/libbcm56846.so, build/switchd/nos-switchd, build/tests/bde_validate
```

### 3. Initramfs

```bash
cd ~/open-nos-as5610/initramfs
./build.sh
# Output: initramfs/initramfs.cpio.gz
# Uses Docker to cross-compile nos-init.c for PPC32
```

### 4. FIT image (kernel + DTB + initramfs)

```bash
cd ~/open-nos-as5610
docker run --rm -v $(pwd):/work -w /work/boot debian:bookworm bash -c \
  "apt-get update -qq && apt-get install -y -qq u-boot-tools device-tree-compiler >/dev/null 2>&1 && ./build-fit.sh"
# Output: boot/nos-powerpc.itb
# Requires: boot/uImage, boot/as5610_52x.dtb, initramfs/initramfs.cpio.gz
```

### 5. Rootfs squashfs

```bash
cd ~/open-nos-as5610
KERNEL_VERSION=5.10.0-nos \
KERNEL_SRC=$HOME/open-nos-as5610/linux-5.10 \
BUILD_DIR=$HOME/open-nos-as5610/build \
rootfs/build.sh
# Output: onie-installer/sysroot.squash.xz
# Docker --privileged runs automatically (needs debootstrap + qemu-user-static)
# Installs: libbcm56846.so, nos-switchd, BDE .ko, platform-mgrd, overlay files
```

### 6. ONIE installer

```bash
cd ~/open-nos-as5610/onie-installer
./build.sh
# Output: onie-installer/open-nos-as5610-YYYYMMDD.bin
```

### Deploy to HTTP server

```bash
# From local machine (build servers can't SSH to each other):
scp smiley@10.22.1.5:~/open-nos-as5610/onie-installer/open-nos-as5610-YYYYMMDD.bin /tmp/
scp /tmp/open-nos-as5610-YYYYMMDD.bin smiley@10.22.1.4:~/http-server/

# Ensure HTTP server is running on 10.22.1.4:
ssh smiley@10.22.1.4 'cd ~/http-server && nohup python3 -m http.server 8000 > http-server.log 2>&1 &'
```

---

## Installing on the Switch

### From ONIE (preferred)

```bash
ONIE:/ # onie-nos-install http://10.22.1.4:8000/open-nos-as5610-YYYYMMDD.bin
```

After install completes, power-cycle the switch (ONIE's `reboot` command may not be found — known bug).

### Getting into ONIE from a running NOS

`fw_setenv` does not work from the NOS (no MTD devices — NOR flash driver not loading).
Use the **boot_count mechanism** instead:

1. Reboot the switch repeatedly (up to 4 times)
2. U-Boot increments `boot_count` on each boot
3. NOS cannot reset `boot_count` (fw_setenv broken), so it accumulates
4. After `boot_count > 3`, U-Boot enters ONIE installer mode automatically

**Switch IPs**:
- NOS: 10.1.1.233 (SSH root/as5610)
- ONIE: 10.1.1.222 (SSH needs `-o HostKeyAlgorithms=+ssh-rsa`)

Poll both IPs after each reboot to know which mode booted.

---

## Known Issues and Fixes

### squashfs xattr error
**Symptom**: `SQUASHFS error: Xattrs in filesystem, these will be ignored`
**Cause**: mksquashfs includes xattrs by default; kernel has `CONFIG_SQUASHFS_XATTR=n`
**Fix**: rootfs/build.sh uses `-no-xattrs` flag on mksquashfs

### overlayfs mount failed / devtmpfs No such device
**Symptom**: `nos-init: WARNING: overlayfs mount failed; falling back to read-only root` then `Failed to mount devtmpfs at /dev: No such device`
**Causes** (check both):
1. **Stale kernel in `boot/uImage`**: The FIT builder uses `boot/uImage`, but `remote-build.sh` outputs to `linux-5.10/arch/powerpc/boot/uImage`. If `boot/uImage` predates kernel config changes (e.g. adding `CONFIG_OVERLAY_FS`), the FIT will contain a kernel without overlayfs. `remote-build.sh` now auto-copies the kernel to `boot/` after build, but if you rebuild the kernel manually, you must copy it yourself: `cp linux-5.10/arch/powerpc/boot/uImage boot/uImage`.
2. **Stale initramfs**: Initramfs not rebuilt after nos-init.c was modified. The FIT contained a stale nos-init binary.

**Fix**: After kernel or nos-init.c changes, ensure `boot/uImage` and `initramfs/initramfs.cpio.gz` are fresh, then rebuild FIT (step 4) + ONIE installer (step 6).

### BDE modules fail to load (Invalid module format)
**Symptom**: `insmod: ERROR: could not load module .../nos_kernel_bde.ko: Invalid module format`
**Cause**: Kernel version string mismatch. The kernel tree appends `+` to the version when the git tree has uncommitted changes (e.g. our AS5610 machine patch). BDE modules are compiled with vermagic `5.10.0-nos` but running kernel is `5.10.0-nos+`.
**Fix**: `remote-build.sh` now creates `.scmversion` in the kernel tree to suppress the `+` suffix. If you rebuild the kernel manually, run `touch linux-5.10/.scmversion` before `make`.

### Packages not installed (frr blocking apt-get)
**Symptom**: openssh-server, iproute2, and other packages missing from rootfs despite being listed in build.sh
**Cause**: `frr` is not available in Debian jessie repos. When mixed into a single `apt-get install` command with `|| true`, the entire install fails silently.
**Fix**: `rootfs/build.sh` now installs frr in a separate apt-get call so its failure doesn't block other packages.

### ONIE reboot not found
**Symptom**: `/bin/exec_installer: line 469: reboot: not found` after install
**Cause**: ONIE BusyBox missing reboot in PATH
**Workaround**: Power-cycle the switch manually; install data is already written to disk.

### fw_setenv not working from NOS
**Symptom**: Cannot set U-Boot env from running NOS; no /proc/mtd, no /dev/mtd*
**Cause**: MTD/CFI kernel modules load but no flash partitions detected (DT doesn't describe NOR flash mapping)
**Workaround**: Use boot_count mechanism (reboot 4 times) to enter ONIE.

---

## Full Rebuild (all steps)

For a complete clean build from scratch:

```bash
# On build server 10.22.1.5:
cd ~/open-nos-as5610

# 1+2. Kernel + BDE + SDK + switchd
BUILD_KERNEL=1 ./scripts/remote-build.sh
sudo chown -R smiley:smiley linux-5.10 build bde

# 3. Initramfs
initramfs/build.sh

# 4. FIT
docker run --rm -v $(pwd):/work -w /work/boot debian:bookworm bash -c \
  "apt-get update -qq && apt-get install -y -qq u-boot-tools device-tree-compiler >/dev/null 2>&1 && ./build-fit.sh"

# 5. Rootfs
KERNEL_VERSION=5.10.0-nos KERNEL_SRC=$HOME/open-nos-as5610/linux-5.10 BUILD_DIR=$HOME/open-nos-as5610/build rootfs/build.sh

# 6. ONIE installer
onie-installer/build.sh
```
