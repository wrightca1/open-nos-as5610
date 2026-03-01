# Platform: accton_as5610_52x (USB flash boot; single-slot: kernel=5, root=6)
cl.platform	accton_as5610_52x
loadaddr	0x02000000
consoledev	ttyS0
baudrate	115200
# Prevent U-Boot from relocating FDT/initrd so Linux can access them (avoids hang after "Loading Device Tree ... OK")
fdt_high	0xffffffff
initrd_high	0xffffffff
usbdev	0
usb_part1	5
usb_part2	5
hw_boot		usb start; usbboot $loadaddr ${usbdev}:$active_part && bootm ${loadaddr}#${cl.platform}
hw_active1	usb start && usbiddev && setenv active_part $usb_part1; run hw_boot
hw_active2	usb start && usbiddev && setenv active_part $usb_part2; run hw_boot
