# Common U-Boot env (matches ONIE/Cumulus so NOS boots after install)
# See: https://opencomputeproject.github.io/onie/design-spec/uboot_nos_interface.html
env_version	1
# Boot flow (mirrors Cumulus/ONL ONIE spec):
#   1. check_boot_reason: obey onie_boot_reason if set (fw_setenv onie_boot_reason install triggers reinstall)
#   2. nos_bootcmd: try our NOS, incrementing boot_count first
#   3. If boot_count exceeds threshold → automatic fallback to ONIE installer
#   4. onie_bootcmd: last resort ONIE fallback
bootcmd	run check_boot_reason; run nos_bootcmd; run onie_bootcmd
# check_boot_reason: if onie_boot_reason is set to a known ONIE trigger, hand off to ONIE.
# Running NOS can trigger reinstall via: fw_setenv onie_boot_reason install && reboot
check_boot_reason	if test $onie_boot_reason = install; then run onie_bootcmd; fi; if test $onie_boot_reason = rescue; then run onie_bootcmd; fi; if test $onie_boot_reason = uninstall; then run onie_bootcmd; fi
# nos_bootcmd: increment boot_count before each NOS boot attempt using setexpr (U-Boot arithmetic).
# Fall back to ONIE installer after 3 consecutive failures.
# The running NOS MUST reset this on successful startup: fw_setenv boot_count 0
nos_bootcmd	if test -z ${boot_count}; then setenv boot_count 0; fi; setexpr boot_count ${boot_count} + 1; saveenv; if test ${boot_count} -gt 3; then echo "NOS failed 3 times - entering ONIE installer"; setenv onie_boot_reason install; saveenv; run onie_bootcmd; fi; if test -n ${bootsource}; then run lbootcmd; else run bootorder1; fi
lbootcmd	echo ** local boot **;if test $bootsource = flashboot; then run flashboot; else dhcp; tftpboot $bootsource && source $loadaddr; fi; sleep 1 && run bootorder1
bootorder1	echo ** trying ${ethaddr}.img **; dhcp; tftpboot $loadaddr ${ethaddr}.img; if imi $loadaddr; then source $loadaddr || bootm ${loadaddr}#${cl.platform}; fi; sleep 1 && run bootorder2
bootorder2	echo ** trying DHCP bootfile **; dhcp; tftpboot $loadaddr $bootfile; if imi $loadaddr; then source $loadaddr || bootm ${loadaddr}#${cl.platform}; fi; sleep 1 && run bootorder3
bootorder3	echo ** trying ${cl.platform}.img **; tftpboot ${cl.platform}.img; if imi $loadaddr; then source $loadaddr || bootm ${loadaddr}#${cl.platform}; fi; sleep 1 && run bootorder4
bootorder4	echo ** trying primary image **; run flashboot; sleep 1 && run bootorder5
bootorder5	echo ** trying alternate image **; run flashboot_alt; sleep 1 && run bootorder1
bootdelay	3
autoload	no
lbootargs	root=/dev/sda6 rootfstype=squashfs ro rootwait
initargs	setenv bootargs quiet console=$consoledev,$baudrate earlycon $lbootargs $debugargs
bootsource	flashboot
cl.active	1
boot_count	0
# Override ONIE's sticky onie_boot_reason=install so check_boot_reason falls through to nos_bootcmd.
# 'nos' is not a recognized ONIE trigger value; check_boot_reason ignores it.
# To reinstall: fw_setenv onie_boot_reason install && reboot  (from running NOS)
onie_boot_reason	nos
set_active1	setenv bootargs ${bootargs} active=1; run hw_active1
set_active2	setenv bootargs ${bootargs} active=2; run hw_active2
boot_active	if test ${cl.active} = 1; then run set_active1; else run set_active2; fi
boot_alt	if test ${cl.active} = 1; then run set_active2; else run set_active1; fi
flashboot	echo "Booting primary image..." && run initargs boot_active
flashboot_alt	echo "Booting alternate image..." && run initargs boot_alt
stdin		serial
stdout		serial
stderr		serial
