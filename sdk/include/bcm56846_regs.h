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
 * NOTE: After warm reboot from Cumulus, CMC2 is locked in SCHAN DMA ring-buffer
 *   mode.  In this mode, 0x33000 (SCHAN_CTRL) reads 0x92 and MSG registers
 *   (0x3300c+) are not directly writable.  All PIO SCHAN operations silently
 *   fail.  A cold VDD power cycle (unplug cables 30+ sec) restores PIO mode.
 *   After cold boot, SCHAN_CTRL reads 0x00 and PIO SCHAN works normally.
 *   (See SCHAN_DISCOVERY_REPORT.md for definitive hardware analysis.)
 */
#define CMIC_CMC0_SCHAN_CTRL      0x33000u   /* CMC2 SCHAN_CTRL (= CMC2_BASE + 0x0) */
#define CMIC_CMC0_SCHAN_MSG(n)    (0x3300cu + (n) * 4u)  /* CMC2 MSG0..MSG20 */

/*
 * CMIC packet DMA — old-style CMIC (BCM56840/56846 Trident+).
 * BCM56840 uses CMIC_DMA_* at 0x100-0x11C, NOT CMICm at 0x31xxx.
 * Source: OpenMDK/cdk/PKG/chip/bcm56840/bcm56840_a0_defs.h
 *
 * 4 DMA channels: CH0=TX, CH1=RX typically.
 * CMIC_DMA_CTRL fields per channel (8 bits apart):
 *   CHx_DIRECTION, CHx_ABORT_DMA, CHx_SEL_INTR_ON_DESC_OR_PKT
 * CMIC_DMA_STAT fields:
 *   DMA_EN bits[3:0], CHAIN_DONE bits[7:4], DESC_DONE bits[11:8]
 */
#define CMIC_DMA_CTRL             0x00000100u
#define CMIC_DMA_STAT             0x00000104u
#define CMIC_DMA_DESC(ch)         (0x00000110u + 4u * (ch))  /* CH0=0x110, CH1=0x114 */

/* Legacy aliases for compatibility */
#define CMICM_DMA_CTRL(ch)        CMIC_DMA_CTRL
#define CMICM_DMA_DESC0(ch)       CMIC_DMA_DESC(ch)
#define CMICM_DMA_STAT            CMIC_DMA_STAT

/*
 * CMIC diagnostic and boot-detection registers.
 *
 * BCM56846 register values after cold VDD power cycle:
 *   BAR0+0x10c  = 0x00000000  SCHAN ring cfg (0 = PIO mode)
 *   BAR0+0x148  = 0x80000000  CMIC_DMA_CFG (DO NOT WRITE — writing 0 breaks SCHAN)
 *   BAR0+0x400  = 0x505b8d80  SCHAN DMA ring head pointer (HW default)
 *   BAR0+0x33000 = 0x00000000  SCHAN_CTRL — PIO mode idle
 *
 * After warm reboot from Cumulus (DMA ring mode persists):
 *   BAR0+0x10c  = 0x32000043  SCHAN DMA ring config (non-zero = DMA ring mode)
 *   BAR0+0x33000 = 0x00000092  SCHAN_CTRL — stuck in ring-buffer mode
 *   MSG registers (0x3300c+) are not directly writable, PIO SCHAN fails.
 *   Software reboot does NOT exit DMA ring mode — cold power cycle required.
 *
 * CMIC_DMA_RING_ADDR (BAR0+0x158):
 *   Cleared by P2020 PCIe PERST_N on every reboot (unreliable for detection).
 *
 * Definitive PIO mode check: write 0x5A5A0000 to 0x3300c, read back.
 *   Match = PIO mode; mismatch = DMA ring mode.
 */
#define CMIC_DMA_CFG              0x0148u   /* DO NOT WRITE (HW default 0x80000000) */
#define CMIC_DMA_CFG_ENABLE       (1u << 31) /* tentative; writing 0 breaks SCHAN */
#define CMIC_DMA_RING_ADDR        0x0158u   /* Non-zero = warm boot (Cumulus ring PA) */
#define CMIC_SCHAN_RING_CFG       0x010cu   /* HW default 0x32000043 */
#define CMIC_SCHAN_RING_HEAD      0x0400u   /* HW default 0x505b8d80 */

/*
 * CMIC_MISC_CONTROL (BAR0+0x1c).
 * Bit 0 = LINK40G_ENABLE: gates SBUS access to 40G (XLMAC) port blocks.
 * Cumulus rc.soc sets this via 'm cmic_misc_control LINK40G_ENABLE=1'.
 * Without it, all SCHAN ops to XLPORT/XLMAC addresses return ERROR_ABORT.
 */
#define CMIC_MISC_CONTROL         0x001cu
#define CMIC_MISC_CONTROL_LINK40G_ENABLE  (1u << 0)

/*
 * MIIM (MDIO) for SerDes — CORRECTED register offsets.
 *
 * The old CMIC_MIIM_PARAM at 0x230 was wrong (from an older BCM chip variant).
 * Correct offsets confirmed by hardware test on BCM56846_A1:
 *
 * MIIM read sequence:
 *   1. Write 16 to MIIM_CTRL (reset read state)
 *   2. Write 18 to MIIM_CTRL (clear done status)
 *   3. Write (reg & 0x1F) to MIIM_ADDRESS
 *   4. Write param to MIIM_PARAM (PHY | BUS_ID | INTERNAL_SEL)
 *   5. Write 0x90 to MIIM_CTRL (trigger read)
 *   6. Poll MIIM_CTRL bit 18 (done)
 *   7. Read 16-bit data from MIIM_READ_DATA
 *   8. Write 18 to MIIM_CTRL (clear done)
 *
 * MIIM write sequence:
 *   1. Write 17 to MIIM_CTRL (reset write state)
 *   2. Write 18 to MIIM_CTRL (clear done status)
 *   3. Write (reg & 0x1F) to MIIM_ADDRESS
 *   4. Write param to MIIM_PARAM (PHY | BUS_ID | INTERNAL_SEL | DATA[15:0])
 *   5. Write 0x91 to MIIM_CTRL (trigger write)
 *   6. Poll MIIM_CTRL bit 18 (done)
 *   7. Write 18 to MIIM_CTRL (clear done)
 */
#define CMIC_MIIM_CTRL            0x00000050u   /* MIIM control/status */
#define CMIC_MIIM_PARAM           0x00000158u   /* MDIO parameter (PHY/BUS/data) */
#define CMIC_MIIM_ADDRESS         0x000004a0u   /* MDIO register address */
#define CMIC_MIIM_READ_DATA       0x0000015cu   /* MDIO read data (16-bit) */
#define CMIC_MIIM_CTRL_DONE       (1u << 18)    /* MIIM operation complete */

/* MIIM clock divider — optional, sets MDIO bus clock speed */
#define CMIC_MIIM_INT_SEL_MAP     0x000001bcu   /* internal MDIO divider */
#define CMIC_MIIM_EXT_SEL_MAP     0x000001b8u   /* external MDIO divider */

/*
 * CMIC_SBUS registers — direct BAR0 access (NOT S-Channel).
 * These configure the CMIC's knowledge of which ASIC sub-block is on which
 * S-bus ring.  They must be written before any S-Channel operation.
 *
 * CORRECTED 2026-03-06: Ring maps start at 0x0204 (NOT 0x0200).
 * 0x0200 is CMIC_SBUS_TIMEOUT — definitively verified by experiment:
 *   - Writing ring map value (0x43052100) to 0x200 causes ALL blocks to
 *     HANG (value interpreted as ~1 billion cycle SBUS timeout).
 *   - Writing 0x7D0 (2000 cycles) to 0x200 and ring maps at 0x204+ works.
 *   - Cumulus works because SDK `init all` overwrites maps at correct offset.
 *
 * Values from Cumulus switchd RE strings:
 *   "Map of S-bus agents (0 to 7) ... User should write: 0x43052100"
 *   "Map of S-bus agents (8 to 15) ... User should write: 0x33333343"
 *   "Map of S-bus agents (16 to 23) ... User should write: 0x44444333"
 *   "Map of S-bus agents (24 to 31) ... User should write: 0x00034444"
 */
#define CMIC_SBUS_TIMEOUT         0x0200u   /* SBUS timeout (cycles)       */
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
 * CMIC_SOFT_RESET_REG (BAR0+0x580) — direct PCI BAR0 register.
 *
 * BCM56840/56846 (Trident+) uses CMIC_SOFT_RESET_REGr for all block resets.
 * This is NOT accessed via SCHAN — it's a direct BAR0 read/write.
 *
 * BCM56850 (Trident2) and later use TOP_SOFT_RESET_REGr via SCHAN instead.
 * BCM56840/56846 does NOT have TOP_SOFT_RESET_REGr at all — confirmed by
 * grep of OpenMDK/cdk/PKG/chip/bcm56840/ (no TOP_SOFT_RESET_REG definition).
 *
 * Source: OpenMDK/cdk/PKG/chip/bcm56840/bcm56840_a0_defs.h:
 *   #define BCM56840_A0_CMIC_SOFT_RESET_REGr 0x00000580
 *
 * All fields are active-LOW (_RST_L): bit=1 = OUT of reset, bit=0 = IN reset.
 * After full reset sequence, the kernel BDE writes 0x0000FFFF (all out of reset).
 */
#define CMIC_SOFT_RESET               0x0580u
#define CMIC_SOFT_RESET_2             0x0584u

/* CMIC_SOFT_RESET bit fields */
#define CMIC_PG0_RST_L                (1u << 0)   /* Port group 0: xlport0-4  (blocks 10-14) */
#define CMIC_PG1_RST_L                (1u << 1)   /* Port group 1: xlport5-8  (blocks 15-18) */
#define CMIC_PG2_RST_L                (1u << 2)   /* Port group 2: xlport9-13 (blocks 19-23) */
#define CMIC_PG3_RST_L                (1u << 3)   /* Port group 3: xlport14-17 (blocks 24-27) */
#define CMIC_MMU_RST_L                (1u << 4)   /* MMU block */
#define CMIC_IP_RST_L                 (1u << 5)   /* Ingress Pipeline */
#define CMIC_EP_RST_L                 (1u << 6)   /* Egress Pipeline */
#define CMIC_XG_PLL0_RST_L            (1u << 7)   /* LCPLL 0 */
#define CMIC_XG_PLL1_RST_L            (1u << 8)   /* LCPLL 1 */
#define CMIC_XG_PLL2_RST_L            (1u << 9)   /* LCPLL 2 */
#define CMIC_XG_PLL3_RST_L            (1u << 10)  /* LCPLL 3 */
#define CMIC_XG_PLL0_POST_RST_L       (1u << 11)  /* PLL0 post-divider */
#define CMIC_XG_PLL1_POST_RST_L       (1u << 12)  /* PLL1 post-divider */
#define CMIC_XG_PLL2_POST_RST_L       (1u << 13)  /* PLL2 post-divider */
#define CMIC_XG_PLL3_POST_RST_L       (1u << 14)  /* PLL3 post-divider */
#define CMIC_TEMP_MON_RST_L           (1u << 15)  /* Temperature monitor */
#define CMIC_SOFT_RESET_ALL_OUT       0x0000FFFFu  /* All blocks out of reset */

/* BCM56846 port group assignments (from OpenMDK bcm56840_a0_chip.c):
 *   PG0 = XLPORT blocks 10-14  (ring 3)
 *   PG1 = XLPORT blocks 15-18  (ring 3)
 *   PG2 = XLPORT blocks 19-23  (ring 4)
 *   PG3 = XLPORT blocks 24-27  (ring 4)
 */

/* XLPORT_MODE_REGr — CDK offset 0x80229 within an XLPORT block.
 * Used for SCHAN probe/diagnostic to verify block accessibility. */
#define XLPORT_MODE_REG_OFF           0x80229u

#endif
