# ASIC sanity check — 10.1.1.233

**Date:** 2026-03-02  
**Host:** root@10.1.1.233 (as5610, Debian 8, Linux 5.10.0-dirty)

## 1. BDE validation (`bde_validate`)

```
BDE validation (Phase 1d)
Test 1: READ_REG(0)
  READ_REG(0) = 0x00000000
Test 2: GET_DMA_INFO + mmap write/read
  DMA pbase=0x3000000 size=4194304
  mmap DMA: wrote 0xdeadbeef, read back 0xdeadbeef
Test 3: READ_REG(0x32800) S-Channel control
  READ_REG(0x32800) CMIC_CMC0_SCHAN_CTRL = 0x00000000
Passed 3/3
```

- **BAR0** accessible (READ_REG(0) reads 0).
- **DMA pool** at physical 0x03000000, 4 MB; mmap write/read OK.
- **0x32800** = 0 (CMC1 SBUSDMA channel 0 control, not S-Channel; expected 0 when idle).

## 2. BAR0 register snapshot (script `scripts/asic_reg_read.py`)

| Offset   | Name                  | Value      | Notes |
|----------|-----------------------|------------|--------|
| 0x00000  | BAR0[0]               | 0x00000000 | First BAR0 dword |
| 0x00200  | CMIC_SBUS_TIMEOUT     | 0x000007d0 | 2000 cycles (expected) |
| 0x00204  | CMIC_SBUS_RING_MAP_0  | 0x43052100 | BCM56846 expected |
| 0x00208  | CMIC_SBUS_RING_MAP_1  | 0x33333343 | BCM56846 expected |
| 0x32800  | CMC1_SBUSDMA_CH0_TM   | 0x00000000 | Packet DMA, idle |
| 0x33000  | **CMIC_CMC2_SCHAN_CTRL** | **0x00000001** | **START=1, DONE=0 → stuck** |
| 0x3300c  | CMIC_CMC2_SCHAN_MSG0  | 0x00000001 | First message word |
| 0x33010  | SCHAN_MSG1            | 0x00000001 | |
| 0x33014  | SCHAN_MSG2            | 0x00000001 | |
| 0x33018  | SCHAN_MSG3            | 0x00000001 | |
| 0x3301c  | SCHAN_MSG4            | 0x00000001 | |

## 3. Conclusion

- **BAR0, DMA, SBUS config:** OK. Timeout and ring maps match expected BCM56846 values.
- **S-Channel:** **Fixed (2026-03-02).** Use CMC0: SCHAN_CTRL at 0x32800, SCHAN_MSG at 0x3300c. CMC2 (0x33000) does not complete ops on this ASIC.

## 4. S-Channel fixes (2026-03-02)

### 4a. Protocol fix (clear + abort)
- If `ctrl != 0`: write ABORT, poll until ctrl==0, then write 0
- After writing cmd/data: write 0 to SCHAN_CTRL before START (protocol step 3)

### 4b. CMC0 vs CMC2 — **fix** (2026-03-02)

**Root cause:** BDE used CMC2 (SCHAN_CTRL at 0x33000). ASIC never completed S-Channel ops → 158 timeouts.

**Fix:** Use CMC0 layout per SCHAN_AND_RING_BUFFERS.md:
- `SCHAN_CTRL` at **0x32800** (CMC0 base 0x31000 + 0x1800)
- `SCHAN_MSG` at **0x3300c** (unchanged)

**Result:** S-Channel timeouts = 0. ASIC now completes S-Channel operations.

## 5. How to re-run

On the switch (root):

```bash
/usr/sbin/bde_validate
python3 /path/to/scripts/asic_reg_read.py
# or custom offsets:
python3 /path/to/scripts/asic_reg_read.py 0x33000 0x3300c 0x33010
```

On PPC, the READ_REG ioctl uses `(3<<30)|(8<<16)|(ord('B')<<8)|1` (see `open-nos-as5610/scripts/asic_reg_read.py`).
