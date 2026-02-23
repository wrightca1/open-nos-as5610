# BDE Kernel Modules — Written by Us

We write our own BDE (Board Device Environment) kernel modules from scratch.
We do NOT use the OpenNSL BDE — it has significant kernel version compatibility issues
and carries licensing complications. We have all the hardware data needed from RE.

## Why Write Our Own

- OpenNSL BDE requires patching for every major kernel version change
- We have complete register offsets, DMA layout, and ioctl interface from RE
- The BDE itself contains zero ASIC-specific logic — it is a thin PCI + DMA abstraction
- Writing our own gives us full control and zero compatibility debt

## Module Overview

### nos-kernel-bde.ko

Kernel-side PCI driver for the BCM56846.

Responsibilities:
- PCI probe: match `vendor=0x14e4, device=0xb846` (BCM56846)
- BAR0 map: `ioremap(pci_resource_start(dev, 0), 256*1024)` at phys `0xa0000000`
- DMA pool: `dma_alloc_coherent(8MB)` with simple slab sub-allocator
- Interrupt: `request_irq(pdev->irq, ...)` for S-Channel completion and RX notification
- S-Channel DMA: write command buffer to DMA memory; program `CMICM_DMA_DESC0` + `CMICM_DMA_CTRL`; wait for completion
- Export kernel symbols for `nos-user-bde.ko` to use

### nos-user-bde.ko

Userspace character device driver: `/dev/nos-bde`

Responsibilities:
- Expose BAR0 register read/write to userspace via ioctl
- Expose DMA pool info (`dma_pbase`, `size`) via ioctl
- Allow userspace to mmap DMA pool (via `remap_pfn_range`)
- Proxy S-Channel operations to kernel BDE

## ioctl Interface

```c
#define NOS_BDE_MAGIC       'B'
#define NOS_BDE_READ_REG    _IOWR(NOS_BDE_MAGIC, 1, nos_bde_reg_t)
#define NOS_BDE_WRITE_REG   _IOW (NOS_BDE_MAGIC, 2, nos_bde_reg_t)
#define NOS_BDE_GET_DMA_INFO _IOR(NOS_BDE_MAGIC, 3, nos_bde_dma_info_t)
#define NOS_BDE_SCHAN_OP    _IOWR(NOS_BDE_MAGIC, 4, nos_bde_schan_t)

typedef struct {
    uint32_t offset;   /* BAR0 byte offset */
    uint32_t value;    /* read: filled by driver; write: value to write */
} nos_bde_reg_t;

typedef struct {
    uint64_t pbase;    /* DMA pool physical/bus address */
    uint32_t size;     /* pool size in bytes */
} nos_bde_dma_info_t;

typedef struct {
    uint32_t cmd[8];   /* S-Channel command words (cmd[0] = opcode + length) */
    uint32_t data[16]; /* data words in/out */
    int      len;      /* number of words */
    int      status;   /* 0 = success */
} nos_bde_schan_t;
```

## Key Register Offsets (from RE)

```c
/* All offsets are from BAR0 base (phys 0xa0000000) */
#define CMIC_CMC0_SCHAN_CTRL        0x32800  /* S-Channel control register */
#define CMICM_CMC_BASE              0x31000
#define CMICM_DMA_CTRL(ch)          (0x31140 + 4*(ch))
#define CMICM_DMA_DESC0(ch)         (0x31158 + 4*(ch))
#define CMICM_DMA_HALT_ADDR(ch)     (0x31120 + 4*(ch))
#define CMICM_DMA_STAT              0x31150
#define CMIC_CMCx_IRQ_STAT0(cmc)    (0x31400 + 0x1000*(cmc))
#define CMIC_MIIM_PARAM             0x00000158  /* SerDes MDIO param */
#define CMIC_MIIM_ADDRESS           0x000004a0  /* SerDes MDIO address */
```

## S-Channel DMA Operation

From RE (`WRITE_MECHANISM_ANALYSIS.md`, `SCHAN_FORMAT_ANALYSIS.md`):

```
S-Channel command word format: 0x2800XXXX
  bits [31:24] = 0x28  (S-Channel magic)
  bits [23:16] = opcode
  bits [15:0]  = table address / length

DMA flow (confirmed from live GDB trace on Cumulus):
  1. Build command word in DMA buffer
  2. Write DMA_DESC0 = physical address of command buffer
  3. Write DMA_CTRL |= DMA_EN (start bit)
  4. Wait for interrupt (IRQ) or poll DMA_STAT for completion
  5. Read result from DMA buffer
```

## Directory Structure

```
bde/
├── README.md                    (this file)
├── Makefile
├── nos-kernel-bde/
│   ├── Kbuild
│   ├── nos_kernel_bde.c         # PCI probe, BAR, DMA, IRQ, S-Chan
│   ├── nos_kernel_bde.h         # Internal definitions
│   └── nos_dma.c                # DMA pool sub-allocator
└── nos-user-bde/
    ├── Kbuild
    ├── nos_user_bde.c           # Char device, ioctl, mmap
    └── nos_user_bde.h           # ioctl definitions (shared with userspace)
```

## Build

```bash
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
     KERNEL_SRC=/path/to/linux-5.10-ppc32
# produces: nos-kernel-bde/nos-kernel-bde.ko
#           nos-user-bde/nos-user-bde.ko
```

## Loading on Target

```bash
insmod nos-kernel-bde.ko dmasize=8M
insmod nos-user-bde.ko
ls -la /dev/nos-bde   # should exist, owned by switchd user
```

## RE References

- `../edgecore-5610-re/ASIC_INIT_AND_DMA_MAP.md` — BAR0 layout, DMA pool, register R/W
- `../edgecore-5610-re/BDE_CMIC_REGISTERS.md` — CMIC register offsets
- `../edgecore-5610-re/WRITE_MECHANISM_ANALYSIS.md` — S-Channel DMA path (GDB confirmed)
- `../edgecore-5610-re/SCHAN_FORMAT_ANALYSIS.md` — S-Channel command word format
- `../edgecore-5610-re/SCHAN_AND_RING_BUFFERS.md` — DMA ring registers
- `../edgecore-5610-re/DMA_DCB_LAYOUT_FROM_KNET.md` — DCB descriptor format
