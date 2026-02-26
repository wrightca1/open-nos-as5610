# Boot image (FIT)

- **nos.its** — FIT description for `mkimage`. References kernel (`uImage`), DTB (`as5610_52x.dtb`), and optionally initramfs.
- **build-fit.sh** — Script to build `nos-powerpc.itb` when uImage and DTB are present. Run from `boot/` or pass paths.

## DTB

Obtain or build `as5610_52x.dtb` for P2020/AS5610-52X:

- **ONL**: Use `as5610_52x.dts` from the ONL tree; compile with `dtc -I dts -O dtb -o as5610_52x.dtb as5610_52x.dts`.
- **Cumulus**: Extract from an existing Cumulus FIT:  
  `dumpimage -l <Cumulus.itb>` (note the FDT subimage index), then  
  `dumpimage -i <Cumulus.itb> -p <index> -o as5610_52x.dtb`.

## Build steps

1. **Kernel uImage**: From build server: `linux-5.10/arch/powerpc/boot/uImage`.
2. **Initramfs** (optional): `initramfs/build.sh` → `initramfs/initramfs.cpio.gz`. Init script mounts squashfs + overlay and `switch_root` (see PLAN.md Phase 1a).
3. **FIT**: Copy uImage and DTB into `boot/`, then:
   ```bash
   cd boot && chmod +x build-fit.sh && ./build-fit.sh
   ```
   Or: `./build-fit.sh /path/to/uImage /path/to/as5610_52x.dtb [initramfs.cpio.gz]`
   Output: `boot/nos-powerpc.itb`.
