---
layout: default
title: Home
---

# open-nos-as5610

An open-source Network Operating System for the **Edgecore AS5610-52X** whitebox switch, built entirely from reverse-engineered hardware data.

No Broadcom SDK. No Cumulus Linux code.

## Quick Links

- [Project README](https://github.com/wrightca1/open-nos-as5610) -- full architecture, build instructions, status
- [Reverse Engineering Documentation](https://wrightca1.github.io/edgecore-5610-reverse-engineering/) -- BCM56846 register maps, table formats, init sequences

## Documentation

- [SCHAN Discovery Report](SCHAN_DISCOVERY_REPORT.html) -- S-Channel register access mechanism
- [SCHAN Investigation Status](SCHAN_INVESTIGATION_STATUS.html) -- current SCHAN debugging state
- [ONIE Partitions](EDGECORE_AS5610_ONIE_PARTITIONS.html) -- AS5610 partition layout
- [ONIE Installer Readiness](ONIE_INSTALLER_READINESS.html) -- installer build status
- [ASIC Check](ASIC_CHECK_10.1.1.233.html) -- live ASIC register dump
- [FRR on PPC32](FRR-PPC32.html) -- FRRouting cross-compilation notes

## Hardware

| | |
|--|--|
| **Platform** | Edgecore AS5610-52X |
| **ASIC** | Broadcom BCM56846 (Trident+) -- 52x 10GbE + 4x 40GbE |
| **CPU** | Freescale P2020 (PowerPC e500v2, PPC32 big-endian) |
| **RAM** | 2 GB DDR3 |
| **Bootloader** | ONIE + U-Boot |

## Architecture

```
User Space:
  FRRouting (BGP/OSPF/ISIS)
       | netlink
  nos-switchd --- libbcm56846 (our SDK)
       |              |
  TUN devices      ioctl/mmap
  swp1..swp52

Kernel:
  nos-kernel-bde.ko  (PCI, BAR0, DMA, S-Channel)
  nos-user-bde.ko    (/dev/nos-bde character device)
       |
  BCM56846 ASIC
```

## Status

Active development. See the [GitHub repo](https://github.com/wrightca1/open-nos-as5610) for current status.
