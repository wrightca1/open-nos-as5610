# Capturing rc.datapath_0 and LED programs

The SOC runner in the SDK runs `rc.soc` and `rc.datapath_0` from the config directory (e.g. `/etc/nos/`). These files contain `setreg <addr> <val>` and `getreg <addr>` lines that configure the ASIC datapath pipeline and LED microcode.

## rc.datapath_0

- **Purpose**: Programs the BCM56846 datapath pipeline (ingress/egress stages). Without it, L2/L3 forwarding may not work correctly.
- **How to capture**: On a running Cumulus switch with the same ASIC, copy the file from the NOS:
  ```bash
  scp cumulus@switch:/etc/cumulus/rc.datapath_0 ./rc.datapath_0
  ```
  Or extract from a Cumulus rootfs/squashfs and place in `etc/nos/rc.datapath_0` before building rootfs.
- **Format**: One line per register: `setreg 0x<hex_addr> 0x<hex_val>` or `getreg 0x<hex_addr>`.

## LED programs (led0.hex, led1.hex)

- **Purpose**: Port and system LED behavior (link, activity, speed). Optional; switch forwards without them but LEDs may be dark.
- **How to capture**: From a Cumulus switch, copy from `/usr/share/cumulus/` or equivalent, or extract from rootfs. Place in `/etc/nos/led0.hex` and `/etc/nos/led1.hex` on the target (or in rootfs overlay).

## Placeholder

If `rc.datapath_0` is missing, the SOC runner skips it. You can leave the file absent until you have a captured copy.
