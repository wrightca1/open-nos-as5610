/* BCM56846 / Trident+ register offsets from BAR0 (RE docs) */
#ifndef BCM56846_REGS_H
#define BCM56846_REGS_H

/*
 * CMICm S-Channel — CMC2 registers (confirmed from libopennsl RE + hardware tests).
 *
 * libopennsl binary string: "S-bus PIO Message Register Set; PCI offset from:
 *   0x3300c to: 0x33060" → CMC2 is the SCHAN CMC used for BCM56846.
 * Hardware test confirmed 4 MSG sets: 0x3100c/CMC0, 0x3200c/CMC1,
 *   0x3300c/CMC2 (active), 0x1000c (alias of CMIC base).
 *
 * CMC2 layout: CTRL = CMC2_BASE + 0x0 = 0x33000
 *              MSG0 = CMC2_BASE + 0xc = 0x3300c (through MSG20=0x33060)
 *
 * NOTE: After warm reboot from Cumulus, CMC2 is in ring-buffer DMA mode
 *   and 0x33000 is non-writable from PCIe.  PIO SCHAN requires a cold
 *   power-cycle of the switch to reset the BCM56846 to PIO mode.
 */
#define CMIC_CMC0_SCHAN_CTRL      0x33000u   /* CMC2 SCHAN_CTRL (= CMC2_BASE + 0x0) */
#define CMIC_CMC0_SCHAN_MSG(n)    (0x3300cu + (n) * 4u)  /* CMC2 MSG0..MSG20 */

/* CMICm packet DMA channels */
#define CMICM_CMC_BASE            0x31000u
#define CMICM_DMA_CTRL(ch)        (0x31140u + 4u * (ch))
#define CMICM_DMA_DESC0(ch)       (0x31158u + 4u * (ch))
#define CMICM_DMA_HALT_ADDR(ch)   (0x31120u + 4u * (ch))
#define CMICM_DMA_STAT            0x31150u

/*
 * Cold-boot detection registers.
 *
 * CMIC_DMA_CFG (BAR0+0x148):
 *   Reads 0x80000000 as the BCM56846 hardware power-on default.
 *   Function of bit31 is not fully decoded, but hardware tests (2026-03-04)
 *   confirm: writing 0 to this register DISABLES SCHAN PIO entirely.
 *   DO NOT write to this register.  Read for diagnostic display only.
 *   The name "DMA_CFG" is tentative; it may be a SCHAN PIO enable register.
 *
 * CMIC_DMA_RING_ADDR (BAR0+0x158):
 *   Cumulus writes the host physical address of the CMC2 SCHAN DMA ring
 *   descriptor buffer here when DMA ring mode is activated (warm reboot).
 *   Non-zero = warm boot (Cumulus DMA mode active); PIO SCHAN via BDE ioctl
 *   is unavailable until cold power cycle.
 *   Zero = cold boot; SCHAN PIO via nos_kernel_bde.ko BDE ioctl is available.
 *
 * NOTE: A previous version of this header incorrectly named DMA_RING_ADDR as
 * CMIC_MIIM_PARAM.  MIIM_PARAM is at a different offset (0x0230).
 */
#define CMIC_DMA_CFG              0x0148u   /* DO NOT WRITE (HW default 0x80000000) */
#define CMIC_DMA_CFG_ENABLE       (1u << 31) /* tentative; writing 0 breaks SCHAN */
#define CMIC_DMA_RING_ADDR        0x0158u   /* Non-zero = warm boot, DMA mode */

/*
 * CMIC_MISC_CONTROL (BAR0+0x1c).
 * Bit 0 = LINK40G_ENABLE: gates SBUS access to 40G (XLMAC) port blocks.
 * Cumulus rc.soc sets this via 'm cmic_misc_control LINK40G_ENABLE=1'.
 * Without it, all SCHAN ops to XLPORT/XLMAC addresses return ERROR_ABORT.
 */
#define CMIC_MISC_CONTROL         0x001cu
#define CMIC_MISC_CONTROL_LINK40G_ENABLE  (1u << 0)

/* MIIM (MDIO) for SerDes */
#define CMIC_MIIM_PARAM           0x00000230u   /* MDIO data/command parameter */
#define CMIC_MIIM_ADDRESS         0x000004a0u

/*
 * CMIC_SBUS registers — direct BAR0 access (NOT S-Channel).
 * These configure the CMIC's knowledge of which ASIC sub-block is on which
 * S-bus ring.  They must be written before any S-Channel operation.
 *
 * Offsets confirmed from OpenBCM SDK-6.5.16 src/soc/mcm/allregs_c.i,
 * entry SOC_REG_INT_CMIC_SBUS_RING_MAP_0_BCM56840_A0r (soc_cpureg type,
 * offset 0x204) and consecutive entries for maps 1-7 at 0x208..0x220.
 * CMIC_SBUS_TIMEOUT is the register immediately before map 0, at 0x200.
 * BCM56840_B0 (and BCM56846) share these offsets.
 *
 * Values confirmed from Cumulus switchd binary strings (libopennsl.so.1
 * RE extract, strings-hex-literals.txt) built for AS5610-52X/BCM56846:
 *   "Map of S-bus agents (0 to 7) ... User should write: 0x43052100"
 *   "Map of S-bus agents (8 to 15) ... User should write: 0x33333343"
 *   "Map of S-bus agents (16 to 23) ... User should write: 0x44444333"
 *   "Map of S-bus agents (24 to 31) ... User should write: 0x00034444"
 *   (agents 32-63: all zeros — no S-bus agents in that range for BCM56846)
 */
#define CMIC_SBUS_TIMEOUT         0x0200u   /* S-bus operation timeout     */
#define CMIC_SBUS_TIMEOUT_VAL     0x7d0u    /* 2000 S-bus cycles           */

#define CMIC_SBUS_RING_MAP_0      0x0204u   /* Agents  0- 7 ring mapping   */
#define CMIC_SBUS_RING_MAP_1      0x0208u   /* Agents  8-15 ring mapping   */
#define CMIC_SBUS_RING_MAP_2      0x020cu   /* Agents 16-23 ring mapping   */
#define CMIC_SBUS_RING_MAP_3      0x0210u   /* Agents 24-31 ring mapping   */
#define CMIC_SBUS_RING_MAP_4      0x0214u   /* Agents 32-39 ring mapping   */
#define CMIC_SBUS_RING_MAP_5      0x0218u   /* Agents 40-47 ring mapping   */
#define CMIC_SBUS_RING_MAP_6      0x021cu   /* Agents 48-55 ring mapping   */
#define CMIC_SBUS_RING_MAP_7      0x0220u   /* Agents 56-63 ring mapping   */

/* BCM56846 (AS5610-52X) ring map values — from Cumulus switchd RE strings */
#define BCM56846_RING_MAP_0       0x43052100u
#define BCM56846_RING_MAP_1       0x33333343u
#define BCM56846_RING_MAP_2       0x44444333u
#define BCM56846_RING_MAP_3       0x00034444u
#define BCM56846_RING_MAP_4       0x00000000u
#define BCM56846_RING_MAP_5       0x00000000u
#define BCM56846_RING_MAP_6       0x00000000u
#define BCM56846_RING_MAP_7       0x00000000u

/*
 * TOP block SBUS addresses for BCM56846 (Trident+).
 *
 * The TOP block (SBUS agent 3 on ring 2) contains chip-level registers
 * including TOP_SOFT_RESET_REG which controls XLPORT soft-reset state.
 * After a cold power cycle, all XLP_RESET bits are SET (XLPORT in reset).
 * They must be cleared before any SCHAN access to XLPORT/XLMAC registers.
 *
 * SBUS address RE analysis (Cumulus switchd, BCM SDK 6.3.8, BCM56846_A1):
 *
 * FUN_10325fa0 and FUN_103260d4 in the switchd binary are SDK register-cache
 * hash table insert/lookup functions.  Their hash keys ARE the SCHAN command
 * words (opcode + SBUS routing + register offset in one 32-bit value).
 * The assembly constructs the key as:
 *   key = (block_sel & 0x7FFF) << 11 | (reg_off & 0x7FF) | 0x28000000
 * where 0x28000000 encodes SCHAN opcode 0x0A (READ_REGISTER) in bits[31:26].
 *
 * For TOP_SOFT_RESET_REG:
 *   block_sel = data constant at 0x11436428 → lower 15 bits = 0x0066
 *   reg_off   = 0x200 (known BCM56840 TOP_SOFT_RESET_REG offset)
 *   SCHAN key = (0x0066 << 11) | 0x200 | 0x28000000 = 0x28033200
 *
 * In this SCHAN word format (CMICm BCM56840/56846):
 *   bits[31:26] = 0x0A = READ_REGISTER opcode
 *   bits[25:16] = 0x003 = SBUS destination 3 = TOP block (confirmed: RING_MAP_0
 *                 nibble 3 = ring 2 = TOP block ring for BCM56846)
 *   bits[15:0]  = 0x3200 = (block subaddr 0x32 from high bits of block_sel << 11)
 *                          | reg offset 0x200 in bits[10:8]
 *
 * This SCHAN word (0x28033200) goes in the BDE schan_op addr argument (cmd[1]).
 * If the BDE strips the opcode, the addr-only variant 0x00033200 may be needed.
 *
 * BCM56846 has 13 XLPORT blocks (XLP0–XLP12) for 52 front-panel ports.
 * TOP_SOFT_RESET_REG bits [12:0] = XLP0_RESET..XLP12_RESET.
 * Write 0x00000000 to de-assert all XLPORT resets.
 */
#define BCM56846_TOP_SBUS_AGENT          3u      /* TOP block SBUS agent from ring map */
#define BCM56846_TOP_REG_SOFT_RESET      0x200u  /* TOP_SOFT_RESET_REG offset */
#define BCM56846_TOP_REG_SOFT_RESET_2    0x204u  /* TOP_SOFT_RESET_REG_2 offset */
#define BCM56846_XLPORT_COUNT            13u     /* 13 XLP blocks = 52 ports */
#define BCM56846_XLPORT_RESET_MASK       0x1FFFu /* bits [12:0] = XLP0-12 reset */

/*
 * Candidate SBUS addresses (probed in priority order).
 * CAND_0 and CAND_1 are highest-confidence based on RE analysis.
 */
#define BCM56846_TOP_SOFT_RESET_CAND_0   0x28033200u  /* RE-confirmed SCHAN word */
#define BCM56846_TOP_SOFT_RESET_CAND_1   0x00033200u  /* SCHAN addr without opcode */
#define BCM56846_TOP_SOFT_RESET_CAND_2   0x00030200u  /* agent<<16 | 0x200 */
#define BCM56846_TOP_SOFT_RESET_CAND_3   0x00300200u  /* agent<<20 | 0x200 */
#define BCM56846_TOP_SOFT_RESET_CAND_4   0x02030200u  /* ring2<<24|agent<<16|0x200 */
#define BCM56846_TOP_SOFT_RESET_CAND_5   0x40030200u  /* 0x40 prefix variant */
#define BCM56846_TOP_SOFT_RESET_CAND_6   0x00000200u  /* bare offset */

#endif
