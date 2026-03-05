# BCM56846 SCHAN PIO Investigation Status
## Date: 2026-03-04

## Summary

S-Channel (SCHAN) PIO operations fail on ALL CMC channels after boot.
Writing SC_GO=1 (bit0) to CMC0/CMC1 SCHAN_CTRL never produces DONE or any error —
the START bit stays set indefinitely. CMC2 SCHAN_CTRL is completely non-writable
and returns 0x77 for all reads.

---

## What We Know — Confirmed Hardware State

### BAR0 Accessible Registers (Working)
| Register | Offset | Value | Notes |
|----------|--------|-------|-------|
| CMIC_MISC_CONTROL | 0x001c | 0x7db4db2f | LINK40G_ENABLE=1 ✓ |
| CMIC_DMA_CFG | 0x0148 | 0x80000000 | DO NOT WRITE; HW default |
| CMIC_DMA_RING_ADDR | 0x0158 | 0x00000000 | 0 = cold boot indicator |
| CMIC_SBUS_TIMEOUT | 0x0200 | 0x00002700 | 9984 cycles (generous) |
| CMIC_SBUS_RING_MAP_0 | 0x0204 | 0x43052100 | Agents 0-7 rings ✓ |
| CMIC_SBUS_RING_MAP_1 | 0x0208 | 0x33333343 | Agents 8-15 rings ✓ |
| CMIC_SBUS_RING_MAP_2 | 0x020c | 0x44444333 | Agents 16-23 ✓ |
| CMIC_SBUS_RING_MAP_3 | 0x0210 | 0x00034444 | Agents 24-31 ✓ (fixed) |
| CMIC_SBUS_RING_MAP_4-7 | 0x0214-0x0220 | 0x00000000 | Agents 32-63 ✓ |
| CHIP_REV/ID (?) | 0x0178 | 0x0002b846 | BCM56846 confirmed |

### CMC Register State
| CMC | Base | CTRL (0x_000) | MSG0 (0x_00c) | CTRL Writable | Notes |
|-----|------|---------------|---------------|---------------|-------|
| CMC0 | 0x31000 | 0→0 (write+readback) | always reads 0 | YES | SC_GO stays set, no completion |
| CMC1 | 0x32000 | 0xf2 initially | 0x00000031 | needs test | SC_GO stays set |
| CMC2 | 0x33000 | **0x77 (non-writable)** | **0x77 (non-writable)** | **NO** | Locked — all CMC2 regs = 0x77 |

### Register Address Aliases (All Map to Same CMIC Base)
- 0x00000: CMIC global registers
- 0x10000: alias of 0x00000 (confirmed, same values)
- 0x30000: alias of 0x00000 (confirmed, same values)

### CMC0 Non-Zero Registers (Partial Scan 0x31000-0x310fc)
Registers at offsets +0x008, +0x018, +0x028, ..., +0xf8 from CMC0 base all = 0x80.
Pattern: 0x31008, 0x31018, ..., 0x310f8 — every 0x10 bytes at position +8.
Interpretation unknown (could be DMA channel status, parity enable, or other).

---

## What We've Tried (Chronological)

### Phase 1: SCHAN Format Bug (Fixed)
- **Bug**: schan.c and probe.py put 0x2a000001/0x28000001 in MSG[0] instead of addr.
  This routes to invalid SBUS agent 512 → ctrl=0x77 for all ops.
- **Fix**: Put `addr` (full SCHAN command word) directly in cmd[0]/MSG[0].
- **Result**: dmesg now shows correct addresses in cmd[0], but ctrl=0x77 continues.
  Root cause was NOT the routing bug alone — there's an additional issue.

### Phase 2: CMC2 Lock Investigation
- **Observation**: CMC2 (0x33000) reads 0x77 everywhere, all writes silently drop.
- **First theory**: CMC2 in Cumulus SCHAN DMA ring mode (warm boot).
- **Disproved**: Cumulus 02sdk.bcm does NOT use SCHAN ring buffer mode.
- **Current theory**: CMC2 locked for unknown hardware reason.

### Phase 3: CMC0/CMC1 Direct SCHAN PIO Test
- Wrote SCHAN command to CMC0 MSG0 (0x3100c), then SC_GO=1 to CTRL (0x31000).
- Result: CTRL=0x00000001 indefinitely (START stuck, no DONE/ABORT/NACK/TIMEOUT).
- Same result for CMC1.
- **Conclusion**: The CMIC SCHAN state machine does not process our START signal.

### Phase 4: SCHAN_CTRL Value Variants Tested
Tried: 0x01, 0x81, 0x101, 0x03, 0x02000001, 0xFE → all either stuck or false DONE.
0x03 shows "DONE" but that's because bit1=DONE is set by us, not hardware.

---

## Current Hypotheses (Ranked by Likelihood)

### H1: SCHAN_CTRL bit0 is NOT the SC_GO trigger (HIGH confidence)
The CMICm SCHAN_CTRL format might be different from what we assume.
- Possible: SC_GO is at a different bit (bit1? bit7? multi-bit field?)
- Evidence: bit0 stays set forever (doesn't auto-clear); hardware normally auto-clears SC_GO
- Action: Systematically test single-bit writes (0x02, 0x04, 0x08, 0x10, ..., 0x80)
  to find the bit that auto-clears (= SC_GO recognized and processed)

### H2: SCHAN requires additional global enable register (MEDIUM confidence)
Some CMIC implementations require a global "SCHAN enable" bit beyond just SC_GO.
- Register at 0x0088 = 0x00000001 (bit0 set) might be CMIC_SCHAN_ENABLE
- Or a register in 0x0000-0x004f range that we haven't written
- Action: Read OpenBCM SDK soc/cmic*.c for init sequence details

### H3: CMC2 lock is from CMIC "claimed by DMA" state (MEDIUM confidence)
Even without explicit ring-buffer mode, the CMIC might put CMC2 in a "DMA-owned" state
during Cumulus init. This would lock CMC2 and require cold power cycle to reset.
- 0x0158=0 supports cold boot hypothesis BUT may not be accurate after nos-switchd init
- Action: Full cold power cycle, then run probe BEFORE nos-switchd starts

### H4: SCHAN requires BCM SDK soc_init() first (MEDIUM confidence)
The BCM SDK may write additional CMIC registers during soc_init() that we're missing.
Known BCM SDK init steps we're NOT doing:
- CMIC_CMCx_PKTDMA_COSQsettings
- CMIC_TOP_INTR_ENABLe registers
- CMIC_MISCCONFIG or CMIC_SOFT_RESET_REG
- Some SBUS ring unlock register

### H5: SCHAN needs SBUS RING ENABLE written (LOW-MEDIUM confidence)
Beyond the ring MAP (0x204+), there may be a SBUS ring ENABLE register that must be
set before any SBUS operation can complete.

---

## Resources Available

### Files on Switch (10.1.1.233)
- `/usr/lib/libbcm56846.so` — Our own NOS BCM library (75KB PowerPC ELF)
- `/usr/lib/libopennsl.so.1` — Check if this exists (might be Cumulus version!)
- `/dev/nos-bde` — Our kernel BDE device (BAR0 read/write + SCHAN ioctl)

### Files in build-server/
- `build-server/opennsl/libopennsl.so.1` — Full 33MB Cumulus BCM SDK binary (PowerPC)
  RE analysis files in build-server/opennsl/sdk-deep-extract/
- `build-server/switchd/switchd` — Cumulus switchd binary

### RE Analysis Available
- `sdk-deep-extract/libopennsl-exported-symbols.txt` — All exported functions
- `sdk-deep-extract/libopennsl-schan-usage.txt` — SCHAN usage in libopennsl
- `sdk-deep-extract/switchd-schan-data-refs.txt` — SCHAN data references in switchd
- `sdk-deep-extract/strings-hex-literals.txt` — All hex strings (includes CMC addresses)
- Ghidra analysis of switchd binary (register constants, function dumps)

### Our Code
- `bde/nos_kernel_bde.c` — Kernel BDE (CMC2 hardcoded at 0x33000/0x3300c)
- `sdk/src/schan.c` — SCHAN helper functions (routing bug fixed 2026-03-04)
- `tools/post-coldboot-probe.py` — Hardware probe tool (v5, routing fix applied)

---

## Next Steps (Prioritized)

### STEP 1 (Quick, ~30min): Find SC_GO bit by systematic test
Write only one bit at a time to CMC0 SCHAN_CTRL (0x31000) and watch if it auto-clears.
The bit that auto-clears AND triggers some response is SC_GO.
Test: 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800.

### STEP 2 (Quick, ~1hr): Mine RE data for SCHAN_CTRL init sequence
Look at `sdk-deep-extract/libopennsl-schan-usage.txt` and Ghidra function dumps
for the exact sequence libopennsl uses to trigger SCHAN PIO:
- What value does it write to 0x31000?
- Does it write to any other registers first?
- What does it do with the DMA_CTRL registers?

### STEP 3 (Medium, ~2hr): Cold power cycle + immediate probe
Power cycle switch completely (unplug both PSU cables, wait 60 sec).
Boot NOS, run probe BEFORE nos-switchd starts.
Goal: get a completely clean state and see if SCHAN works on the virgin CMC state.
Compare CMC register values at clean boot vs after nos-switchd runs init.

### STEP 4 (Harder): Extract SCHAN init from libopennsl binary on switch
The Cumulus `/usr/lib/libopennsl.so.1` (if it exists on switch) IS the actual
working library. Cross-reference with Ghidra analysis to extract exact CMIC init
sequence and replicate it in nos_kernel_bde.c.

### STEP 5 (Hard): Check for /sys/bus/pci kernel PCIe reset path
BCM56846 might support PCIe link-level reset (not FLR). This could reset the CMC
to a known state. The BCM56846 might need PCIe reset after warm boot to clear CMC2.

---

## Key Unknown: Why CMC2 (0x33000) is Non-Writable

Possible explanations:
1. **CMC2 is in hardware-DMA mode** regardless of 02sdk.bcm setting (SDK init enabled it)
2. **CMC2 is in SBUS ring-buffer SCHAN mode** that the SDK enabled at runtime
3. **CMC2 offset is wrong** and 0x33000 maps to some other read-only hardware
4. **CMC2 needs to be "claimed" by software** before it becomes writable (some token/ownership)

The 0x77 value is suspicious: 0b01110111 = bit0,1,2,4,5,6 set. In SCHAN_CTRL:
- bit0=START: set (operation in progress?)
- bit1=DONE: set
- bit2=ABORT/ERR_ABORT: set
- bit4: unknown
- bit5: unknown (SBUS timeout in some CMICm?)
- bit6: unknown (SER check fail in some CMICm?)

If CMC2 SCHAN_CTRL is stuck at 0x77 = "last op completed with ABORT+DONE+other_err",
this is the final state left by Cumulus after its last SCHAN op. In DMA ring mode,
the SCHAN_CTRL would still reflect the last SCHAN completion status, and the hardware
might protect it from being written while in DMA mode.

---

## Critical Test Pending

**Does CMC0 SCHAN work if SC_GO is at a different bit than bit0?**

From the BCM SDK for BCM56840 (same CMICm family), SCHAN_CTRL format:
```
bit0: SC_GO (write 1 to start SCHAN operation; auto-cleared when done)
bit1: SC_DONE (set by hardware; write 1 to clear)
bit2: SC_ERROR_ABORT (set by hardware on SBUS error; write 1 to clear)
bit3: SC_NACK_ERROR (set by hardware on SBUS NACK)
bit5: SC_TIMEOUT_ERROR (set when SBUS_TIMEOUT expires)
bit6: SC_SER_CHK_FAIL (set when SER parity check fails on result)
```

However: bit0 = SC_GO AND bit5 = SC_TIMEOUT_ERROR creates ambiguity.
If SBUS_TIMEOUT fires, bit5 should set. We see bit0 only → timeout is NOT firing.
This means either:
a) SC_GO at bit0 IS correct, but the CMIC's SBUS controller is in reset (not running)
b) SC_GO is at a different bit
c) The SBUS master clock is gated and not running

**The SBUS master clock gating** is a real possibility — there may be a register
that enables the SBUS clock, and without it no SBUS timeout fires.
