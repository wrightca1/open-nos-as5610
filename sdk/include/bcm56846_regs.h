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

/* MIIM (MDIO) for SerDes */
#define CMIC_MIIM_PARAM           0x00000158u
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

#endif
