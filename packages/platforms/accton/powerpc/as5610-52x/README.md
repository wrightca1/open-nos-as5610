# Accton AS5610-52X (PowerPC) — ONL-style platform

This tree follows the **Open Network Linux (ONL)** platform layout so we stay aligned with the open-source ONL project.

- **ONL reference**: [packages/platforms/accton/powerpc/as5610-52x](https://github.com/opencomputeproject/OpenNetworkLinux/tree/master/packages/platforms/accton/powerpc/as5610-52x)
- **Platform config**: `platform-config/r0/src/lib/powerpc-accton-as5610-52x-r0.yml` is the single source of truth for loader, FIT, U-Boot env, and installer partitions.

## Implementation

| YAML / concept        | open-nos-as5610 implementation |
|-----------------------|---------------------------------|
| `flat_image_tree`     | `boot/nos.its` + `build-fit.sh` → `nos-powerpc.itb` (kernel + initramfs + DTB) |
| `loader.device`       | `/dev/sda` (or first sdX on USB 1-1.3) |
| `loader.nos_bootcmds` | `onie-installer/uboot_env/` → `hw_boot`, `flashboot` |
| `environment`         | NOR `/dev/mtd1`; installer uses `fw_setenv -f -s` with script from `uboot_env/*.inc` |
| `installer` partitions | `onie-installer/cumulus/init/accton_as5610_52x/platform.fdisk` + `platform.conf` |
| `console`             | `consoledev` / `baudrate` in `as5610_52x.platform.inc` |

## Differences from ONL

- **Partition layout**: ONL uses ONL-BOOT (128MiB), ONL-CONFIG (128MiB), ONL-IMAGES (768MiB), ONL-DATA (rest). We use a **simplified single-slot** layout (kernel partition 5, rootfs partition 6) to work with ONIE’s kernel on this device (limited logical partitions).
- **DTB**: ONL builds DTB from their kernel tree. We use the **Cumulus DTB** (extracted from their FIT) or a padded minimal DTB for build; see `scripts/extract-dtb.sh` and `boot/README.md`.
- **Installer**: We use a single self-extracting script + data.tar (Cumulus-style); ONL uses initrd + chroot + `onl-install`. Same goal: partition, write FIT + rootfs, set U-Boot env.

See also: `docs/reverse-engineering/ONL_DEB8_PPC_AS5610_REFERENCE.md`.
