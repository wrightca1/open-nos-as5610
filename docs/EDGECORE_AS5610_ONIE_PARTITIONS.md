# Edgecore AS5610-52X — ONIE partition layout and boot

This document describes the partition layout and information required for the **Edgecore AS5610-52X** (same hardware as Accton AS5610-52X) to boot from an ONIE-installed image.

## Storage

- **Device**: Internal USB flash (typically `/dev/sda`; ONIE may use `sdb` depending on enumeration)
- **Layout**: MBR with primary + extended + primary partitions

## Partition table (sector units)

| Partition | Type    | Start sector | End sector | Purpose |
|-----------|---------|--------------|------------|---------|
| **sda1**  | Primary | 8192         | 270273     | **persist** — ext2; config, licenses, persistent data |
| **sda2**  | Extended| 270274       | 860097     | Container for logical partitions |
| **sda5**  | Logical | 270336       | 303041     | **Kernel slot A** — raw FIT image (uImage .itb) |
| **sda6**  | Logical | 303104       | 565185     | **Root FS slot A** — squashfs (read-only root) |
| **sda7**  | Logical | 565248       | 597953     | **Kernel slot B** — raw FIT image |
| **sda8**  | Logical | 598016       | 860097     | **Root FS slot B** — squashfs |
| **sda3**  | Primary | 860160       | (end)      | **rw-overlay** — ext2; overlay upper/work for rootfs |

## Boot flow

1. **U-Boot** reads `cl.active` (1 = slot A, 2 = slot B).
2. **Slot A**: kernel from sda5, root from sda6. **Slot B**: kernel from sda7, root from sda8.
3. Kernel boots with `root=/dev/sda6` (or sda8 for slot B). **Initramfs** (inside FIT) mounts sda6 squashfs and sda3 overlay, then `switch_root` to real root.
4. **U-Boot env** set by installer: `bootsource=flashboot`, `cl.active=1`, `cl.platform=accton_as5610_52x`.

## ONIE installer image contents

The loadable file (e.g. `open-nos-as5610-YYYYMMDD.bin`) is a **self-extracting script** plus **data.tar** containing:

| Item | Purpose |
|------|---------|
| **control** | `Architecture: powerpc`, `Platforms: accton_as5610_52x edgecore_as5610_52x` |
| **cumulus/init/accton_as5610_52x/platform.conf** | Partition variable names (persist_part, kernel_part1, ro_part1, …) |
| **cumulus/init/accton_as5610_52x/platform.fdisk** | fdisk script to create the MBR layout above |
| **nos-powerpc.itb** | FIT image: kernel (uImage) + DTB (as5610_52x.dtb) + initramfs |
| **sysroot.squash.xz** | SquashFS root filesystem (Debian + nos-switchd, libbcm56846, BDE modules, FRR, etc.) |

## Producing the loadable .bin

From repo root:

```bash
# One-shot (build on server, then FIT + rootfs + .bin locally; requires DTB and rootfs deps)
DTB_IMAGE=/path/to/as5610_52x.dtb ./scripts/build-onie-image.sh

# Or extract DTB from Cumulus and build
CUMULUS_BIN=/path/to/CumulusLinux-2.5.1-powerpc.bin ./scripts/build-onie-image.sh
```

Output: **onie-installer/open-nos-as5610-YYYYMMDD.bin**

Install on the switch (ONIE rescue or install mode):

```bash
onie-nos-install http://<your-server>/open-nos-as5610-YYYYMMDD.bin
```

## Platform names

ONIE may report the platform as **accton_as5610_52x** or **edgecore_as5610_52x**. The installer accepts both and uses the same partition layout (accton_as5610_52x) for both.

## References

- `../docs/reverse-engineering/ONIE_BOOT_AND_PARTITION_LAYOUT.md`
- `../onie-installer/cumulus/init/accton_as5610_52x/platform.fdisk`
- `../boot/README.md` (DTB and FIT)
