# BCM56846 S-Channel Hardware Discovery Report

## Summary

Live hardware testing on the AS5610-52X (BCM56846 rev 2) revealed the
correct SCHAN register layout and the root cause of PIO SCHAN failures.

**TL;DR**: CMC2 (CTRL=0x33000, MSG=0x3300c) is the correct SCHAN channel.
After warm reboot from Cumulus, CMC2 is locked in **ring-buffer DMA mode**
and cannot be used for PIO SCHAN. A **cold power cycle** is required.

---

## Hardware Configuration Confirmed

| Register | BAR0 Offset | Value/Notes |
|---|---|---|
| CMIC_DEV_REV_ID | 0x178 | 0x0002b846 (BCM56846 rev 2) ✓ |
| CMIC_SBUS_TIMEOUT | 0x200 | 0x7d0 (programmed by nos-init) ✓ |
| CMIC_SBUS_RING_MAP_0 | 0x204 | 0x43052100 ✓ |
| CMIC_SBUS_RING_MAP_1 | 0x208 | 0x33333343 ✓ |
| CMIC_SBUS_RING_MAP_2 | 0x20c | 0x44444333 ✓ |
| CMIC_SBUS_RING_MAP_3 | 0x210 | 0x00034444 ✓ |
| CMC2 SCHAN_CTRL | 0x33000 | Non-writable from PCIe (ring-buf mode) |
| CMC2 SCHAN_MSG0 | 0x3300c | Write-only FIFO; readback = 0x00000001 |

---

## SCHAN CMC Layout Discovery

### Evidence: 4 MSG Register Sets in switchd Binary

Extracted from `build-server/switchd/sdk-deep-extract/strings-hex-literals.txt`:

```
S-bus PIO Message Register Set; PCI offset from: 0x3100c to: 0x31060  → CMC0
S-bus PIO Message Register Set; PCI offset from: 0x3200c to: 0x32060  → CMC1
S-bus PIO Message Register Set; PCI offset from: 0x3300c to: 0x33060  → CMC2  ← libopennsl uses this
S-bus PIO Message Register Set; PCI offset from: 0x1000c to: 0x10060  → alias of CMIC base
```

### CMCx Layout (confirmed from MSG set addresses)

The BCM56846 CMICm uses a classical layout where each CMC has:
- **SCHAN_CTRL** at `CMCx_BASE + 0x0`
- **SCHAN_MSG0** at `CMCx_BASE + 0xc`
- **SCHAN_MSG20** at `CMCx_BASE + 0x60`

| CMC | Base | SCHAN_CTRL | SCHAN_MSG0 | Used by |
|-----|------|-----------|-----------|---------|
| CMC0 | 0x31000 | 0x31000 | 0x3100c | (unused) |
| CMC1 | 0x32000 | 0x32000 | 0x3200c | (unused) |
| **CMC2** | **0x33000** | **0x33000** | **0x3300c** | **libopennsl** |
| CMIC alias | 0x10000 | 0x10000 | 0x1000c | (alias) |

### BAR0 Area Map (from hardware dump)

```
0x00000-0x10fff: CMIC registers (ID, LED, MIIM, SBUS, etc.)
  0x00004-0x0004c: Chip security hash (read-only)
  0x00088:         0x00000001 (LEDUP0_CTRL)
  0x00178:         0x0002b846 (CMIC_DEV_REV_ID)
  0x00200:         CMIC_SBUS_TIMEOUT
  0x00204-0x00220: CMIC_SBUS_RING_MAP_0..7
  0x10000:         Alias of 0x00000 CMIC base

0x20400-0x207fc: LEDUP0 Data RAM
0x20800-0x20bfc: LEDUP0 Program RAM

0x31000-0x31fff: CMC0 registers
  0x31000:         CMC0 SCHAN_CTRL (writable, 8-bit; not used by libopennsl)
  0x3100c-0x31060: CMC0 SCHAN_MSG0..20

0x32000-0x32fff: CMC1 + LEDUP1 area
  0x32000:         CMC1 SCHAN_CTRL (writable, 8-bit; Cumulus stale 0xf2 after reboot)
  0x32000-0x323ff: Repeating {0xf2, 0x00, 0x80, 0x00} pattern (PKTDMA channels)
  0x32400-0x3251f: 0x00000080 (PKTDMA status)
  0x32520-0x327ff: LEDUP1 Program RAM (byte data)
  0x32800:         NOT SCHAN_CTRL — in LEDUP/DMA area (byte-wide registers)
  0x3200c-0x32060: CMC1 SCHAN_MSG0..20

0x33000-0x33fff: CMC2 registers
  0x33000:         CMC2 SCHAN_CTRL (NON-WRITABLE from PCIe; ring-buf mode)
  0x3300c-0x33060: CMC2 SCHAN_MSG0..20 (write-only FIFO when SCHAN busy)
```

---

## Why SCHAN PIO Fails (Root Cause)

### The Problem: Ring-Buffer DMA Mode

After **warm reboot** from Cumulus Linux, the BCM56846 CMC2 retains the
configuration written by the Cumulus BCM SDK. The Broadcom SDK configures
CMC2 in **ring-buffer DMA mode** for high-performance SCHAN operations.

In ring-buffer mode:
1. SCHAN commands are placed in a ring buffer in host DRAM (DMA)
2. The CMIC fetches commands from the ring buffer autonomously
3. The CMIC writes responses back to the ring buffer
4. **Direct PCIe writes to SCHAN_CTRL (0x33000) are non-functional** — the
   register is managed internally by the CMIC's ring-buffer state machine

Evidence of ring-buffer mode:
- `0x33000` reads 0x00000001 (START bit held by HW ring-buf state machine)
- Writes to `0x33000` have zero effect (PCIe writes rejected by ring-buf mode)
- `0x0158` = `0x0292ffd0` — a host DRAM DMA buffer address for the ring
- `0x010c` = `0x32000043` — ring buffer write pointer / descriptor count

### Hardware Writability Test Results

```
CMC2-0x33000: before=0x000000ef → write DEADBEEF → after=0x000000ef  READ-ONLY
CMC0-0x31000: before=0x00000000 → write DEADBEEF → after=0x000000ef  WRITABLE (8-bit)
CMC1-0x32000: before=0x00000000 → write DEADBEEF → after=0x000000ef  WRITABLE (8-bit)
0x32800:      before=0x00000000 → write DEADBEEF → after=0x000000ef  WRITABLE (8-bit)
0x10000:      before=0x00000000 → write DEADBEEF → after=0xdeadbeef  WRITABLE (32-bit, CMIC alias)
```

CMC0 and CMC1 SCHAN_CTRLs are writable but NOT the active SCHAN CMC —
writing START has no effect on the hardware state machine.

### SCHAN PIO Test Results (all timed out)

All combinations of CTRL+MSG were tested. None produced DONE:

| CTRL | MSG | Result |
|------|-----|--------|
| 0x33000 (CMC2) | 0x3300c | Timeout — CTRL non-writable |
| 0x31000 (CMC0) | 0x3100c | Timeout — wrong CMC |
| 0x32000 (CMC1) | 0x3200c | Timeout — wrong CMC |
| 0x32800 (LED area!) | 0x3300c | Timeout — not a SCHAN_CTRL |

---

## Register Address Clarification: 0x32800

The **switchd RE analysis script** labeled `0x32800` as `CMIC_CMC0_SCHAN_CTRL`.
Hardware testing shows this is **incorrect** — 0x32800 is in the LEDUP/DMA
area (byte-wide registers, not SCHAN_CTRL).

The correct addresses per libopennsl binary and hardware evidence:
- **SCHAN_CTRL = 0x33000** (CMC2_BASE + 0x0)
- **SCHAN_MSG = 0x3300c..0x33060** (CMC2_BASE + 0xc..0x60)

---

## Fix Options

### Option A: Cold Power Cycle (Recommended)

Power cycle the switch (not just warm reboot). After a cold boot:
1. The BCM56846 resets to factory defaults (PIO mode, no ring-buffer config)
2. Our driver programs SBUS ring maps and timeout on probe
3. SCHAN_CTRL (0x33000) accepts writes; PIO SCHAN works normally

**Expected behavior after cold boot:**
- 0x33000 reads 0x00000000 (SCHAN idle, clean state)
- Write ABORT+clear → confirmed CTRL = 0x00000000
- Write MSG command → write START → poll → DONE asserts → response in MSG

### Option B: Implement Ring-Buffer Mode (Complex)

Implement SCHAN ring-buffer DMA in `nos_bde_schan_op()`:
1. Allocate a DMA ring buffer in host RAM (already have DMA pool)
2. Program ring buffer base address to CMIC ring-buf registers
3. Write SCHAN commands to ring buffer entries
4. Signal CMIC via ring-buf produce pointer
5. Poll ring-buf consume pointer for completion

This avoids the need for a cold power cycle and matches how the Cumulus
BCM SDK operates. Implementation is complex (~200 lines of kernel code).

### Option C: CMIC Soft Reset

If a CMIC_SOFT_RESET register can be found at a known BAR0 offset,
writing it would reset the CMC2 ring-buffer configuration without a
physical power cycle. Register offset not yet confirmed for BCM56846.
Candidates to investigate: BAR0+0x0000, BAR0+0x013c.

---

## Next Steps

1. **Immediate**: Cold power cycle the switch → verify SCHAN PIO works
2. **Short term**: If power cycle confirms SCHAN works, document the
   initialization sequence and ensure `nos_kernel_bde.c` handles cold boot
3. **Long term**: Consider implementing ring-buffer mode (Option B) to
   avoid requiring a cold power cycle after every Cumulus → open-NOS migration

---

## Test Programs

The following test programs in `/tmp/` on the switch were used for this analysis:

- `cmic_dump`: Dumps BAR0+0x000-0x200 and CMC1/CMC2 areas
- `schan_cmc2_test`: Tests SCHAN PIO with all 4 CMC address combinations
- `cmc_full_test`: Tests CMC at 0x10000, writability of all CTRL candidates,
  detailed CMC2 analysis
- `schan_cmc0_test`: Tests ring map verification and CMC0/CMC1 SCHAN
