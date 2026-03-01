# Kernel Patches for Linux 5.10 / AS5610-52X

## Root cause of "No suitable machine description found"

Linux 5.10 mainline has no machine descriptor for the AS5610-52X. The DTB
(extracted from Cumulus) has `compatible = "accton,as5610_52x"`. The kernel
iterates all compiled-in `machine_desc` entries, finds no match, and panics.

**Fix**: Add `accton_as5610_52x.c` to `arch/powerpc/platforms/85xx/`.

## Files in this directory

- `arch/powerpc/platforms/85xx/accton_as5610_52x.c` — AS5610 machine description

## How to apply to a Linux 5.10 tree

```bash
LINUX=/path/to/linux-5.10

# 1. Copy platform file
cp arch/powerpc/platforms/85xx/accton_as5610_52x.c \
   $LINUX/arch/powerpc/platforms/85xx/

# 2. Add to Kconfig (before "endif # FSL_SOC_BOOKE"):
# Insert these lines at the end of the "if PPC32" block in Kconfig:
cat >> $LINUX/arch/powerpc/platforms/85xx/Kconfig << 'EOF'

config ACCTON_AS5610_52X
	bool "Accton/Edgecore AS5610-52X"
	select DEFAULT_UIMAGE
	help
	  This option enables support for the Accton/Edgecore AS5610-52X
	  open networking switch (Freescale P2020 SoC, BCM56846 ASIC).
	  Matches DTB compatible strings "accton,as5610_52x" and "accton,5652".
EOF

# 3. Add to Makefile (after the MPC85XX_RDB line):
echo 'obj-$(CONFIG_ACCTON_AS5610_52X) += accton_as5610_52x.o' \
  >> $LINUX/arch/powerpc/platforms/85xx/Makefile

# 4. Enable in .config:
echo 'CONFIG_ACCTON_AS5610_52X=y' >> $LINUX/.config

# 5. Rebuild:
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- olddefconfig
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- uImage -j$(nproc)
```

## build-on-build-server.sh integration

The `scripts/remote-build.sh` already handles this via Docker on the Debian 8
build server (10.22.1.5). The files in this directory are applied automatically
during the `BUILD_KERNEL=1` build pass.

## Compatible strings matched

| DTB source | compatible string |
|------------|------------------|
| Cumulus Linux 2.5.x | `"accton,as5610_52x"` |
| ONIE kernel 3.2.69 | `"accton,5652"` |
