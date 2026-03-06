# Common U-Boot env for open-nos-as5610 (AS5610-52X, U-Boot 2013.01)
#
# IMPORTANT: This U-Boot does NOT have 'usbboot' or 'usbiddev' commands.
# Use 'usb read' with raw sector offsets to load the kernel FIT from USB.
#
# Partition layout (from platform.fdisk):
#   sda5 = kernel FIT  — starts at sector 270336 (0x42000), size 32706 sectors (0x7FC2)
#   sda6 = rootfs squashfs — starts at sector 303104 (0x49F00)
#
# Boot flow:
#   1. check_boot_reason: if onie_boot_reason=install/rescue/uninstall → boot ONIE
#   2. nos_bootcmd: boot_count safety, then load kernel via 'usb read' and boot
#   3. onie_bootcmd: fallback to ONIE if NOS fails
env_version	1
bootcmd	run check_boot_reason; run nos_bootcmd; run onie_bootcmd
check_boot_reason	if test $onie_boot_reason = install; then run onie_bootcmd; fi; if test $onie_boot_reason = rescue; then run onie_bootcmd; fi; if test $onie_boot_reason = uninstall; then run onie_bootcmd; fi
# nos_bootcmd: increment boot_count, fall back to ONIE after 3 failures.
# NOS must run 'fw_setenv boot_count 0' on successful startup.
# Uses 'usb read' (proven working on this U-Boot) to load FIT from sda5.
# 'noinitrd' tells the kernel to ignore the stub ramdisk in the FIT image.
# Without it, the kernel takes the initrd_load() path which doesn't honor rootwait,
# causing immediate panic before USB storage has time to enumerate.
nos_bootcmd	if test -z ${boot_count}; then setenv boot_count 0; fi; setexpr boot_count ${boot_count} + 1; saveenv; if test ${boot_count} -gt 3; then echo "NOS failed 3 times - entering ONIE installer"; setenv onie_boot_reason install; saveenv; run onie_bootcmd; fi; usb start; setenv bootargs quiet noinitrd console=$consoledev,$baudrate earlycon root=/dev/sda6 rootfstype=squashfs ro rootwait active=1; usb read $loadaddr 0x42000 0x7fc2 && bootm ${loadaddr}#${cl.platform}
bootdelay	3
autoload	no
boot_count	0
# Override ONIE's sticky onie_boot_reason=install so check_boot_reason falls through.
# 'nos' is not a recognized ONIE trigger; check_boot_reason ignores it.
# To reinstall from running NOS: fw_setenv onie_boot_reason install && reboot
onie_boot_reason	nos
stdin	serial
stdout	serial
stderr	serial
