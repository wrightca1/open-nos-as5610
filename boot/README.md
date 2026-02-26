# Boot image (FIT)

- **nos.its** — FIT description for `mkimage`. References kernel (`uImage`), DTB (`as5610_52x.dtb`), and optionally initramfs.
- **DTB**: Obtain or build `as5610_52x.dtb` for P2020/AS5610-52X. ONL has `as5610_52x.dts`; or extract from Cumulus FIT with `dumpimage -l <Cumulus.itb>` then `dumpimage -i <Cumulus.itb> -p N -o as5610.dtb`.
- **Build**: Copy `uImage` (from kernel build), DTB, and optionally `initramfs.cpio.gz` (from `initramfs/build.sh`) into this dir or adjust paths in nos.its, then:
  `mkimage -f boot/nos.its boot/nos-powerpc.itb`
