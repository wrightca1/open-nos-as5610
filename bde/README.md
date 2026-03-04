# BDE Kernel Modules

Custom BDE (Board Device Environment) kernel modules for BCM56846 (Trident+).
Written entirely from RE docs — no OpenNSL/Broadcom SDK code.

## Module Overview

### nos_kernel_bde.ko

Kernel-side PCI driver for BCM56846.

- PCI probe: matches `vendor=0x14e4, device=0xb846`
- BAR0 map: 256KB at phys `0xa0000000`
- DMA pool: `dma_alloc_coherent(4MB)` at phys `0x03000000` (PPC32 DMA window)
- Interrupt: `request_irq` (currently stub; future RX/completion)
- **S-Channel PIO**: CMD written to SCHAN_MSG (0x3300c+), START asserted at SCHAN_CTRL (0x33000),
  polls for DONE(bit1) or ERROR_ABORT(bit2). Protected by `schan_mutex`.
- Exports: `nos_bde_get_bar0()`, `nos_bde_get_dma_pbase()`, `nos_bde_schan_op()`

### nos_user_bde.ko

Character device `/dev/nos-bde` exposing BDE to userspace.

- `READ_REG` / `WRITE_REG`: direct BAR0 ioread32/iowrite32
- `GET_DMA_INFO`: returns DMA pool phys base + size
- `SCHAN_OP`: proxies through `nos_bde_schan_op()` in kernel BDE

## ioctl Interface

```c
#define NOS_BDE_MAGIC       'B'
#define NOS_BDE_READ_REG    _IOWR(NOS_BDE_MAGIC, 1, struct nos_bde_reg)      /* 0xC0084201 */
#define NOS_BDE_WRITE_REG   _IOW (NOS_BDE_MAGIC, 2, struct nos_bde_reg)      /* 0x40084202 */
#define NOS_BDE_GET_DMA_INFO _IOR(NOS_BDE_MAGIC, 3, struct nos_bde_dma_info) /* 0x400C4203 */
#define NOS_BDE_SCHAN_OP    _IOWR(NOS_BDE_MAGIC, 4, struct nos_bde_schan)    /* 0xC0684204 */

struct nos_bde_reg { uint32_t offset; uint32_t value; };
struct nos_bde_dma_info { uint64_t pbase; uint32_t size; };
struct nos_bde_schan {
    uint32_t cmd[8];   /* SCHAN command words */
    uint32_t data[16]; /* data in/out */
    int32_t  len;      /* cmd_words */
    int32_t  status;   /* 0=success, -EIO=SBUS err, -ETIMEDOUT=no completion */
};
```

> **BUG in installed binary**: As of 2026-03-03, the switch has `nos_user_bde.ko` compiled
> with WRITE_REG as `_IOR` (0x80084202) instead of the source's `_IOW` (0x40084202).
> Python scripts targeting the current binary must use `WW = 0x80084202`. Rebuild fixes this.

## Key Register Offsets (confirmed from hardware, 2026-03-03)

```
BAR0 base: 0xa0000000

0x200   CMIC_SBUS_TIMEOUT    set to 0x7d0 (2000 cycles) during chip_init
0x204   CMIC_SBUS_RING_MAP_0 agents  0- 7 → 0x43052100
0x208   CMIC_SBUS_RING_MAP_1 agents  8-15 → 0x33333343
0x20c   CMIC_SBUS_RING_MAP_2 agents 16-23 → 0x44444333
0x210   CMIC_SBUS_RING_MAP_3 agents 24-31 → 0x00034444
0x148   CMIC_DMA_CFG         0=cold boot, 0x80000000=warm-boot from Cumulus
0x158   CMIC_DMA_RING_ADDR   0=cold boot, 0x0294ffd0=warm-boot from Cumulus
0x33000 CMC2_SCHAN_CTRL      SCHAN PIO control (CMC2 = libopennsl-confirmed CMC)
0x3300c CMC2_SCHAN_MSG0      SCHAN cmd/response word 0 (through MSG20 at 0x33060)
```

## S-Channel PIO Protocol

```
Confirmed operational on BCM56846 after cold power cycle + chip_init():

  1. If SCHAN_CTRL (0x33000) != 0: write CTRL|ABORT(4), poll until 0, write 0.
  2. Write cmd[0..n-1] to SCHAN_MSG(0..n-1)   [0x3300c, 0x33010, ...]
  3. For write ops: write data[0..m-1] to SCHAN_MSG(n..n+m-1)
  4. Write 0 to SCHAN_CTRL.
  5. Write SCHAN_CTRL_START (1) to SCHAN_CTRL.
  6. Poll SCHAN_CTRL for completion (timeout 50ms):
       bit 1 (DONE)         → success; read result from SCHAN_MSG(0..)
       bit 2 (ERROR_ABORT)  → SBUS timeout (block not responding / in reset)
       bit 3 (NACK)         → SBUS NACK from target agent
  7. Write 0 to SCHAN_CTRL.

Expected SBUS errors:
  ctrl=0x4 (ERROR_ABORT) is normal for uninitialized port registers (XLMAC, SerDes).
  This is NOT a SCHAN protocol failure — the SCHAN completed, SBUS target returned error.

SCHAN opcode format (confirmed from nos-switchd dmesg):
  cmd[0] bits [31:24] = opcode: 0x2a=SOC_MEM_READ, 0x28=SOC_MEM_WRITE
  cmd[0] bits [7:0]   = word count
  cmd[1]              = SOC memory/register address
```

## Cold-Boot vs Warm-Boot

| State                   | BAR0+0x158 | BAR0+0x148 | SCHAN writes |
|-------------------------|-----------|-----------|-------------|
| Cold boot (power cycle) | 0x00000000 | 0x00000000 | Work ✓      |
| Warm reboot from Cumulus| 0x0294ffd0 | 0x80000000 | Silently ignored ✗ |

Warm-boot SCHAN failure: BCM56846 CMIC stays in ring-buffer DMA mode across warm reset.
**Fix**: hard power cycle only. No PCI reset method works (FLR unsupported; bus reset
clears PCIe link but NOT CMIC internal state).

## Build

```bash
# Cross-compile for PPC32 BE, targeting kernel 5.10.0-dirty
make ARCH=powerpc CROSS_COMPILE=powerpc-linux-gnu- \
     -C /path/to/linux-5.10-ppc32 \
     M=$(pwd) modules
# produces: nos_kernel_bde.ko, nos_user_bde.ko
```

Build servers: 10.22.1.5 (main, Docker), 10.22.1.4 (powerpc-linux-gnu-gcc).

## Loading on Target

```bash
insmod nos_kernel_bde.ko
insmod nos_user_bde.ko
ls -la /dev/nos-bde   # should exist
```
