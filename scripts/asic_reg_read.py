#!/usr/bin/env python3
"""
Read BCM56846 BAR0 registers via /dev/nos-bde NOS_BDE_READ_REG ioctl.
Usage: run on switch (root): python3 asic_reg_read.py [offset_hex ...]
Default: read 0x0, 0x200, 0x204, 0x32800, 0x33000, 0x3300c (BAR0, SBUS timeout, RING_MAP_0, DMA ctrl, SCHAN_CTRL, SCHAN_MSG0).
"""
import array
import fcntl
import os
import struct
import sys

NOS_BDE_DEVICE = "/dev/nos-bde"
NOS_BDE_MAGIC = ord("B")
# _IOWR(NOS_BDE_MAGIC, 1, struct nos_bde_reg) with size 8 (PPC uses 3<<30 for R|W)
NOS_BDE_READ_REG = (3 << 30) | (8 << 16) | (NOS_BDE_MAGIC << 8) | 1

REG_NAMES = {
    0x0: "BAR0[0]",
    0x200: "CMIC_SBUS_TIMEOUT",
    0x204: "CMIC_SBUS_RING_MAP_0",
    0x208: "CMIC_SBUS_RING_MAP_1",
    0x32800: "CMC1_SBUSDMA_CH0_TM (0x32800)",
    0x33000: "CMIC_CMC2_SCHAN_CTRL",
    0x3300c: "CMIC_CMC2_SCHAN_MSG0",
}


def read_reg(fd, offset):
    buf = array.array("I", [offset, 0])
    fcntl.ioctl(fd, NOS_BDE_READ_REG, buf, True)
    return buf[1]


def main():
    if len(sys.argv) > 1:
        offsets = [int(x, 16) for x in sys.argv[1:]]
    else:
        offsets = [0x0, 0x200, 0x204, 0x208, 0x32800, 0x33000, 0x3300c]

    fd = os.open(NOS_BDE_DEVICE, os.O_RDWR)
    try:
        for off in offsets:
            val = read_reg(fd, off)
            name = REG_NAMES.get(off, "0x%x" % off)
            print("  %s (0x%05x) = 0x%08x" % (name, off, val))
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
