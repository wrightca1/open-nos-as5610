# Root Filesystem

PPC32 big-endian Linux root filesystem built with Buildroot or Yocto.

## Target Architecture

- Architecture: `powerpc` (PPC32 big-endian)
- CPU: Freescale P2020 (e500v2 core)
- ABI: soft-float or hardfp

## Package List

### Required
- `nos-switchd` — our control plane daemon (built from `../switchd/`)
- `libbcm56846` — our custom SDK (built from `../sdk/`)
- `frr` — FRRouting (BGP, OSPF, ISIS, BFD)
- `iproute2` — ip, bridge, tc
- `ifupdown2` — interface configuration
- `lldpd` — LLDP
- `busybox` — standard Unix utilities
- `openssh` — SSH server
- `python3` — scripting

### Kernel Modules (loaded at boot)
- `linux-kernel-bde.ko`
- `linux-user-bde.ko`
- `tun.ko` (or built-in)
- `accton_as5610_52x_cpld.ko`
- I2C mux drivers for SFP/QSFP buses

### Optional
- `onlp` — platform management library + daemon
- `collectd` — metrics collection
- `tcpdump` — packet capture
- `ethtool` — port diagnostics

## Init System

**systemd** (or OpenRC for lighter footprint):

Boot order:
1. Load BDE modules (`linux-kernel-bde.ko`, `linux-user-bde.ko`)
2. Load platform drivers (CPLD, I2C mux)
3. Start `platform-mgrd` (thermal/fan/PSU)
4. Start `nos-switchd` (ASIC init + TUN creation + netlink)
5. Start `frr` (routing daemons: zebra, bgpd, ospfd, ...)

## Filesystem Layout

```
squashfs (read-only):
  /usr/sbin/nos-switchd
  /usr/lib/libbcm56846.so
  /etc/nos/                # default config files
  /usr/sbin/frr/           # FRR daemons
  ...

overlay (rw, /dev/sda3):
  /etc/frr/                # FRR config
  /etc/nos/ports.conf      # port configuration
  /etc/nos/config.bcm      # ASIC config
  /var/log/                # logs

persist (/dev/sda1):
  /mnt/persist/            # startup config, licenses, SSH host keys
```
