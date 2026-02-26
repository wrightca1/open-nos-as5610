# Building open-nos-as5610

## Build server (recommended)

Builds run on the same Debian build hosts used by ONL (see `../build-config.sh`).

```bash
# From open-nos-as5610 repo root
./scripts/build-on-build-server.sh
```

This syncs the repo to the server and runs the remote build. **By default the full stack is built** (kernel + BDE + SDK + nos-switchd) so the switch can run our kernel and modules. It will:

- Install PPC32 cross-toolchain on the server if missing (`gcc-powerpc-linux-gnu`)
- Install cmake if missing (for SDK/switchd)
- **Kernel + BDE**: built by default (`BUILD_KERNEL=1`). Set `BUILD_KERNEL=0` to skip (SDK + switchd only, for quick iteration).
- **SDK (libbcm56846) + nos-switchd**: always built (PPC32 userspace)

### Providing a kernel tree for BDE

The BDE modules build against a Linux 5.10 (or compatible) kernel source tree. Options:

1. **Use an existing tree on the server**  
   Run with `KERNEL_SRC` set on the server, or clone Linux 5.10 yourself and pass it:
   ```bash
   ssh smiley@10.22.1.5 'cd open-nos-as5610-build-YYYYMMDD-HHMMSS && KERNEL_SRC=/path/to/linux ./scripts/remote-build.sh'
   ```

2. **Let the script clone and build the kernel** (default; slow first time only):
   ```bash
   ./scripts/build-on-build-server.sh
   ```
   Or explicitly: `BUILD_KERNEL=1 ./scripts/build-on-build-server.sh`. This clones mainline v5.10, runs a PPC 85xx defconfig, builds `uImage` and modules, then builds the BDE. Use `BUILD_KERNEL=0` to skip kernel+BDE and build only SDK + switchd.

3. **Use modern Debian build host** (if toolchain on Debian 8 is too old):
   ```bash
   USE_BUILD_SERVER=modern ./scripts/build-on-build-server.sh
   ```

### After the build

Artifacts on server (in `~/open-nos-as5610-build-YYYYMMDD-HHMMSS/`):

| Component | Path |
|-----------|------|
| BDE modules | `bde/nos_kernel_bde.ko`, `bde/nos_user_bde.ko` |
| SDK | `build/sdk/libbcm56846.so`, `build/sdk/libbcm56846.a` |
| nos-switchd | `build/switchd/nos-switchd` |
| BDE validation test | `build/tests/bde_validate` (run on target, or under QEMU: `./scripts/run-bde-validate.sh`) |
| Kernel (if BUILD_KERNEL=1) | `linux-5.10/arch/powerpc/boot/uImage` |

Copy back (adjust host/path):

```bash
scp smiley@10.22.1.4:open-nos-as5610-build-*/bde/*.ko .
scp smiley@10.22.1.4:open-nos-as5610-build-*/build/sdk/libbcm56846.so* .
scp smiley@10.22.1.4:open-nos-as5610-build-*/build/switchd/nos-switchd .
```

## Local build (same host as development)

Prereqs:

```bash
sudo apt-get install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu make
```

### BDE only

```bash
# Need a kernel tree (e.g. linux-5.10) with modules_prepare already run
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
  -C /path/to/linux-5.10 M=$(pwd)/bde modules
```

### SDK / switchd (later)

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=tools/ppc32-toolchain.cmake -B build sdk/
cmake --build build --target package
```

See [PLAN.md](PLAN.md) Phase 0 and Phase 1 for full build and boot sequence.

---

## Building the ONIE-installable image

To produce an image that can be installed via ONIE on the switch (`onie-nos-install http://.../open-nos-as5610-YYYYMMDD.bin`), build in order:

### 1. Kernel + BDE + SDK + switchd (build server)

```bash
./scripts/build-on-build-server.sh
```

Copy back from server: `uImage`, BDE `.ko`, `libbcm56846.so`, `nos-switchd` (see "After the build" above). Optionally copy `linux-5.10/arch/powerpc/boot/uImage` and any built modules.

### 2. DTB (Device Tree Blob)

Required for FIT and boot. Obtain once:

- **From ONL**: Build from ONL tree `as5610_52x.dts` → `as5610_52x.dtb`
- **From Cumulus**: `dumpimage -l /path/to/CumulusLinux-*.bin` then `dumpimage -i ... -p <fdt_index> -o as5610_52x.dtb`

Place `as5610_52x.dtb` in `boot/` or set `DTB_IMAGE` when building FIT.

### 3. Initramfs

```bash
./initramfs/build.sh
```

Produces `initramfs/initramfs.cpio.gz`. Requires busybox (or set `BUSYBOX_STATIC` to a PPC32 static busybox path).

### 4. FIT image (kernel + DTB + initramfs)

```bash
# From repo root; kernel uImage and DTB in boot/ or set KERNEL_IMAGE, DTB_IMAGE
./boot/build-fit.sh [path/to/uImage] [path/to/as5610_52x.dtb] [path/to/initramfs.cpio.gz]
```

Produces `boot/nos-powerpc.itb`.

### 5. Rootfs (Debian 12 PPC32 squashfs)

On an x86 host with `debootstrap`, `qemu-user-static`, `squashfs-tools`:

```bash
# Copy build artifacts into repo (bde/*.ko, build/sdk/libbcm56846.so, build/switchd/nos-switchd)
# or set BUILD_DIR to build server path if staging is on NFS.
./rootfs/build.sh
```

Produces `onie-installer/sysroot.squash.xz`. Set `BUILD_ARTIFACTS=0` to build rootfs without our binaries (for testing). Set `KERNEL_VERSION=5.10.0` to match the kernel you use.

### 6. ONIE installer .bin

```bash
./onie-installer/build.sh
```

Expects `boot/nos-powerpc.itb` (or `KERNEL_FIT`) and `onie-installer/sysroot.squash.xz` (or `ROOTFS_SQUASH`). Produces `onie-installer/open-nos-as5610-YYYYMMDD.bin`.

### 7. Install on switch

From ONIE (install or rescue mode):

```bash
onie-nos-install http://<your-server>/open-nos-as5610-YYYYMMDD.bin
```

Or copy the `.bin` to a USB stick and run `onie-nos-install /mnt/usb/open-nos-as5610-YYYYMMDD.bin`.

---

## One-command ONIE image (Edgecore AS5610-52X)

To produce a **single loadable .bin** with all partitions and info for the Edgecore AS5610 to boot:

```bash
# Requires: DTB (or Cumulus image to extract from), and build server access for kernel/SDK
DTB_IMAGE=/path/to/as5610_52x.dtb ./scripts/build-onie-image.sh
# or
CUMULUS_BIN=/path/to/CumulusLinux-*.bin ./scripts/build-onie-image.sh
```

This script: builds kernel+BDE+SDK+switchd on server and copies back; builds initramfs and FIT; builds rootfs (debootstrap); packages **onie-installer/open-nos-as5610-YYYYMMDD.bin** with the correct partition layout and platform strings for Edgecore/Accton AS5610-52X.

See **docs/EDGECORE_AS5610_ONIE_PARTITIONS.md** for the full partition table and boot flow.
