# ONIE Installer

Self-extracting shell script that installs open-nos-as5610 via ONIE on the AS5610-52X.

## ONIE Boot Flow

```
U-Boot (PowerPC P2020)
  → ONIE (install mode)
    → onie-nos-install http://server/open-nos.bin
      → install.sh runs (under ONIE busybox)
        → detect platform: accton_as5610_52x
        → format_disk (partition /dev/sda per platform.fdisk)
        → install kernel to sda5, rootfs to sda6 (single-slot)
        → set U-Boot env: cl.active=1, bootsource=flashboot
        → reboot
```

## Partition Layout

```
/dev/sda (USB flash):
  sda1: persist    (ext2, sectors 8192–270273)
  sda2: extended   (container, 2 logicals only)
    sda5: kernel   (raw uImage FIT, 16 MiB)
    sda6: rootfs   (squashfs, ~289 MiB)
  sda3: rw-overlay (ext2, 895840–end)
```

## Build

```bash
./build.sh   # produces open-nos-as5610-YYYYMMDD.bin
```

## Files

| File | Purpose |
|------|---------|
| `install.sh` | Main installer script |
| `platform.conf` | Partition layout, USB path, slot names |
| `platform.fdisk` | fdisk MBR partition script |
| `uboot_env/as5610_52x.platform.inc` | U-Boot platform variable |
| `uboot_env/common_env.inc` | nos_bootcmd, flashboot sequence |
| `build.sh` | Assembles control.tar.xz + data.tar → installer binary |

## RE References

- `../../docs/reverse-engineering/ONIE_BOOT_AND_PARTITION_LAYOUT.md` — complete installer format
