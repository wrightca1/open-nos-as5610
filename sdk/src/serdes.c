/*
 * Warpcore WC-B0 SerDes init for BCM56846 (Trident+).
 *
 * Implements the full OpenMDK WARPcore initialization sequence:
 *   1. Stop PLL sequencer
 *   2. Download uC firmware (wc40_ucode_b0_bin, ~38KB via XLPORT UCMEM)
 *   3. Start PLL sequencer, wait for lock
 *   4. Per-lane 10G SFI speed configuration:
 *      - Hold Tx/Rx ASIC reset (MISC6)
 *      - Set FIRMWARE_MODE = SFP_DAC (2)
 *      - Clear CL49 HiGig2 bits
 *      - Set forced speed FV_fdr_10G_SFI = 0x29
 *        (MISC1[4:0]=0x09, MISC3.FORCE_SPEED_B5=1)
 *      - Set IND_40BITIF, FIBER_MODE
 *      - Disable auto-negotiation
 *      - Release Tx/Rx ASIC reset
 *
 * AER register addressing (CRITICAL):
 *   WARPcore registers are addressed via AER (Address Extension Register).
 *   A 16-bit register address is split into:
 *     - Block = addr & 0xFFF0 (written to CL22 reg 0x1F)
 *     - Offset = addr & 0x000F (CL22 reg 0x10 + offset)
 *   WRONG: passing full address as "block" and 0 as "offset" — this
 *   always accesses offset 0 within the block, ignoring the low 4 bits.
 *
 * Key registers (MDIO via AER):
 *  - MIIM: CTRL=0x50, PARAM=0x158, ADDR=0x4A0, READ_DATA=0x15C
 *  - XGXSCONTROL (0x8000): bit13=START_SEQUENCER
 *  - XGXSSTATUS  (0x8001): bit11=TXPLL_LOCK
 *  - MISC1 (0x8308): [4:0]=FORCE_SPEED, [11:8]=PLL_MODE_AFE
 *  - MISC2 (0x830D): bit0=FIBER_MODE
 *  - MISC3 (0x833C): bit15=IND_40BITIF, bit7=FORCE_SPEED_B5
 *  - MISC6 (0x8345): bit15=RESET_RX_ASIC, bit14=RESET_TX_ASIC
 *  - FIRMWARE_MODE (0x81F2): per-lane firmware mode [3:0]=ln0...[15:12]=ln3
 *  - UC COMMAND (0xFFC2), WRDATA (0xFFC3), STATUS (0xFFC5)
 *
 * Port-to-PHY mapping (empirical, 10G SFP+ ports):
 *  - SFP I2C buses 10-13 -> WARPcore PHY 13,17,31 on MDIO bus 1
 */
#include "bcm56846.h"
#include "bcm56846_regs.h"
#include "sbus.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int bde_read_reg(uint32_t offset, uint32_t *value);
extern int bde_write_reg(uint32_t offset, uint32_t value);

/* Firmware binary (from OpenMDK wc40_ucode_b0) */
extern unsigned char wc40_ucode_b0_bin[];
extern unsigned int wc40_ucode_b0_bin_len;

/* MIIM parameter encoding */
#define MIIM_PARAM_INTERNAL (1u << 25)
#define MIIM_PARAM_BUS_ID(b) (((uint32_t)(b) & 7u) << 22)
#define MIIM_PARAM_PHY(pa)   (((uint32_t)(pa) & 31u) << 16)
#define MIIM_PARAM_DATA(d)   ((uint32_t)(d) & 0xffffu)

/* WARPcore AER addressing */
#define WC_REG_AER_BLK    0x1f   /* block select register */
#define WC_REG_AER_LANE   0x1e   /* lane/devad select register */
#define WC_AER_BROADCAST  0xffd0 /* AER broadcast value for lane select init */

/*
 * WARPcore register addresses (full 16-bit).
 * AER splits these: block = addr & 0xFFF0, offset = addr & 0x000F.
 */
#define WC_XGXSCONTROL    0x8000u  /* bit13=START_SEQUENCER */
#define WC_XGXSSTATUS     0x8001u  /* bit11=TXPLL_LOCK */
#define WC_LANEPRBS       0x8015u
#define WC_MISC1          0x8308u  /* [4:0]=FORCE_SPEED */
#define WC_MISC2          0x830Du  /* bit0=FIBER_MODE */
#define WC_SERDESID0      0x8310u
#define WC_MISC3          0x833Cu  /* bit15=IND_40BITIF, bit7=FORCE_SPEED_B5 */
#define WC_MISC6          0x8345u  /* bit15=RESET_RX_ASIC, bit14=RESET_TX_ASIC */
#define WC_CL49_CTRL1     0x8360u
#define WC_FW_VERSION     0x81F0u
#define WC_FW_MODE        0x81F2u  /* per-lane firmware mode */
#define WC_FW_CRC         0x81FEu
#define WC_UC_RAMWORD     0xFFC0u
#define WC_UC_ADDRESS     0xFFC1u
#define WC_UC_COMMAND     0xFFC2u  /* bit15=INIT_CMD, bit4=UC_RESET_N */
#define WC_UC_WRDATA      0xFFC3u
#define WC_UC_DL_STATUS   0xFFC5u  /* bit2=INIT_DONE, bit1=ERR1, bit0=ERR0 */
#define WC_UC_COMMAND2    0xFFCAu  /* bit5=TMR_EN */
#define WC_UC_COMMAND3    0xFFCCu  /* bit0=EXT_MEM_ENABLE, bit1=EXT_CLK_ENABLE */
#define WC_COMBO_MII_CTRL 0xFFE0u  /* bit12=AN_ENABLE */
#define WC_ANARXSTATUS    0x80B0u  /* SIGDET status (bit5=RX_SIGDET, bit0=ADC) */

/* init_stage_2 registers — critical for 64B/66B block lock */
#define WC_CONTROL1000X1  0x8300u  /* bit6=DISABLE_PLL_PWRDWN, bit4=AUTODET_EN */
#define WC_CONTROL1000X3  0x8302u  /* bit13=DISABLE_TX_CRS, [2:1]=FIFO_ELASICITY */
#define WC_MISC4          0x833Du  /* bit15=AUTO_PCS_TYPE_SEL_EN */
#define WC_RX66_CTRL      0x83C0u  /* bit14=CC_DATA_SEL, bit13=CC_EN */
#define WC_RX66_SCW0      0x83C2u  /* 64/66 sync word 0 */
#define WC_RX66_SCW1      0x83C3u  /* 64/66 sync word 1 */
#define WC_RX66_SCW2      0x83C4u  /* 64/66 sync word 2 */
#define WC_RX66_SCW3      0x83C5u  /* 64/66 sync word 3 */
#define WC_RX66_SCW0_MASK 0x83C6u  /* sync word 0 mask */
#define WC_RX66_SCW1_MASK 0x83C7u  /* sync word 1 mask */
#define WC_RX66_SCW2_MASK 0x83C8u  /* sync word 2 mask */
#define WC_RX66_SCW3_MASK 0x83C9u  /* sync word 3 mask */

/* CL49_CONTROL1r HiGig2 bits to clear for standard 10G */
#define WC_CL49_HG2_MASK  0x077cu

/*
 * XLPORT UCMEM CDK addresses (from bcm56840_a0_defs.h).
 *
 * These are full CDK SCHAN addresses — bits [23:20] select the sub-block
 * within XLPORT (0x5 = UCMEM data, 0x8 = UCMEM control).  The SBUS block
 * number is passed separately via sbus_mem_write_blk/sbus_mem_read_blk.
 *
 * UCMEM_CTRLr (0x00580241): Bit 0 = ACCESS_MODE (1=CPU, 0=WC).
 * UCMEM_DATAm (0x00500000): 128-bit (4 x u32) entries, 4096 max.
 */
#define XLPORT_WC_UCMEM_CTRL  0x00580241u
#define XLPORT_WC_UCMEM_DATA  0x00500000u

/*
 * Map 10G SFP+ port to XLPORT block base address.
 * These bases encode the physical block number in bits [31:20]
 * using extended block format: ((block & 0xF) | 0x400) << 20.
 *
 * Port 49 → xe48 → physical block 22 → 0x40600000
 * Port 50 → xe49 → physical block 21 → 0x40500000
 * Port 51 → xe50 → physical block 25 → 0x40900000
 * Port 52 → xe51 → physical block 24 → 0x40800000
 */
static uint32_t port_to_xlport_base(int port)
{
	switch (port) {
	case 49: return 0x40600000u;
	case 50: return 0x40500000u;
	case 51: return 0x40900000u;
	case 52: return 0x40800000u;
	default: return 0;
	}
}

/* Max poll iterations for MIIM done */
#define MIIM_POLL_MAX 2000

/*--- MIIM read/write using correct CMIC registers ---*/

static int miim_write(int phy_addr, int bus_id, uint32_t reg, uint16_t data)
{
	uint32_t param, ctrl;
	int i;

	/* Reset write state, clear done */
	bde_write_reg(CMIC_MIIM_CTRL, 17);
	bde_write_reg(CMIC_MIIM_CTRL, 18);

	/* Set register address */
	bde_write_reg(CMIC_MIIM_ADDRESS, reg & 0x1fu);

	/* Set parameter: internal PHY, bus ID, PHY addr, data */
	param = MIIM_PARAM_INTERNAL | MIIM_PARAM_BUS_ID(bus_id)
		| MIIM_PARAM_PHY(phy_addr) | MIIM_PARAM_DATA(data);
	bde_write_reg(CMIC_MIIM_PARAM, param);

	/* Trigger write */
	bde_write_reg(CMIC_MIIM_CTRL, 0x91);

	/* Poll for done */
	for (i = 0; i < MIIM_POLL_MAX; i++) {
		bde_read_reg(CMIC_MIIM_CTRL, &ctrl);
		if (ctrl & CMIC_MIIM_CTRL_DONE) {
			bde_write_reg(CMIC_MIIM_CTRL, 18);
			return 0;
		}
	}

	bde_write_reg(CMIC_MIIM_CTRL, 17);
	return -EIO;
}

static int miim_read(int phy_addr, int bus_id, uint32_t reg, uint16_t *data)
{
	uint32_t param, ctrl, val;
	int i;

	/* Reset read state, clear done */
	bde_write_reg(CMIC_MIIM_CTRL, 16);
	bde_write_reg(CMIC_MIIM_CTRL, 18);

	/* Set register address */
	bde_write_reg(CMIC_MIIM_ADDRESS, reg & 0x1fu);

	/* Set parameter: internal PHY, bus ID, PHY addr */
	param = MIIM_PARAM_INTERNAL | MIIM_PARAM_BUS_ID(bus_id)
		| MIIM_PARAM_PHY(phy_addr);
	bde_write_reg(CMIC_MIIM_PARAM, param);

	/* Trigger read */
	bde_write_reg(CMIC_MIIM_CTRL, 0x90);

	/* Poll for done */
	for (i = 0; i < MIIM_POLL_MAX; i++) {
		bde_read_reg(CMIC_MIIM_CTRL, &ctrl);
		if (ctrl & CMIC_MIIM_CTRL_DONE) {
			bde_read_reg(CMIC_MIIM_READ_DATA, &val);
			bde_write_reg(CMIC_MIIM_CTRL, 18);
			if (data)
				*data = (uint16_t)(val & 0xffffu);
			return 0;
		}
	}

	bde_write_reg(CMIC_MIIM_CTRL, 16);
	return -EIO;
}

/*--- WARPcore register access via AER ---*/

/*
 * Read a WARPcore register with lane selection.
 * addr: full 16-bit register address (e.g. 0x8308 for MISC1).
 *   Block = addr & 0xFFF0 -> written to AER block register (CL22 reg 0x1F)
 *   Offset = addr & 0x000F -> CL22 register = 0x10 + offset
 */
static int wc_read(int phy, int lane, int bus, uint16_t addr, uint16_t *val)
{
	uint16_t block = addr & 0xFFF0u;
	int offset = addr & 0xFu;

	if (miim_write(phy, bus, WC_REG_AER_BLK, WC_AER_BROADCAST) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_LANE, lane & 0x3) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_BLK, block) != 0)
		return -EIO;
	return miim_read(phy, bus, 0x10 + offset, val);
}

/*
 * Write a WARPcore register with lane selection.
 * addr: full 16-bit register address.
 */
static int wc_write(int phy, int lane, int bus, uint16_t addr, uint16_t val)
{
	uint16_t block = addr & 0xFFF0u;
	int offset = addr & 0xFu;

	if (miim_write(phy, bus, WC_REG_AER_BLK, WC_AER_BROADCAST) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_LANE, lane & 0x3) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_BLK, block) != 0)
		return -EIO;
	return miim_write(phy, bus, 0x10 + offset, val);
}

/*
 * CL45 indirect register read via CL22 registers 13/14.
 * This is the correct method for reading PCS_IEEESTATUS1r (devad 3, reg 1)
 * which reports 10G link status. GP0 SYNC_STATUS is NOT valid for 10G SFI.
 */
static int wc_cl45_read(int phy, int bus, int devad, uint16_t reg,
			 uint16_t *val)
{
	/* Reset AER to lane 0, block 0 */
	miim_write(phy, bus, WC_REG_AER_BLK, WC_AER_BROADCAST);
	miim_write(phy, bus, WC_REG_AER_LANE, 0);
	miim_write(phy, bus, WC_REG_AER_BLK, 0x0000);

	/* CL45 indirect: address frame */
	miim_write(phy, bus, 13, (uint16_t)(devad & 0x1f));
	miim_write(phy, bus, 14, reg);

	/* CL45 indirect: data frame */
	miim_write(phy, bus, 13, (uint16_t)(0x4000 | (devad & 0x1f)));
	return miim_read(phy, bus, 14, val);
}

/*--- Port-to-PHY mapping ---*/

static int port_to_phy_bus(int port, int *phy_addr, int *bus_id, int *lane)
{
	/* 10G SFP+ ports (49-52) -- confirmed MDIO bus 1 */
	if (port == 49) {
		*phy_addr = 13; *bus_id = 1; *lane = 0;
		return 0;
	}
	if (port == 50) {
		*phy_addr = 17; *bus_id = 1; *lane = 0;
		return 0;
	}
	if (port == 51) {
		*phy_addr = 31; *bus_id = 1; *lane = 0;
		return 0;
	}
	if (port == 52) {
		*phy_addr = 31; *bus_id = 1; *lane = 1;
		return 0;
	}

	/* 1G ports: 4 ports per WARPcore, grouped into blocks of 4.
	 * MDIO0: PHY 01,05,09,13,17,21,31 (7 WCs = 28 lanes)
	 * MDIO2: PHY 01,09,13,17,21,31    (6 WCs = 24 lanes) */
	if (port >= 1 && port <= 48) {
		int xe = port - 1;
		int block = xe / 4;
		*lane = xe % 4;
		if (block < 7) {
			static const int phy_map[] = {1, 5, 9, 13, 17, 21, 31};
			*phy_addr = phy_map[block];
			*bus_id = 0;
		} else {
			static const int phy_map[] = {1, 9, 13, 17, 21, 31};
			int idx = block - 7;
			if (idx >= 6)
				return -EINVAL;
			*phy_addr = phy_map[idx];
			*bus_id = 2;
		}
		return 0;
	}

	return -EINVAL;
}

/*--- MIIM clock divider setup ---*/

static void miim_divider_init(void)
{
	bde_write_reg(CMIC_MIIM_INT_SEL_MAP, (1u << 16) | 99u);
	bde_write_reg(CMIC_MIIM_EXT_SEL_MAP, (1u << 16) | 396u);
}

/*--- Firmware download via XLPORT UCMEM (SCHAN WRITE_MEM) ---*/

/*
 * Download WARPcore uC firmware via XLPORT UCMEM parallel bus.
 *
 * The WARPcore has an internal 8051 microcontroller that handles CDR,
 * DFE, and signal conditioning. Without firmware, CDR cannot lock to
 * incoming 10G signals.
 *
 * On Trident+ (BCM56840/56846), the CDK always uses the UCMEM path
 * (not MDIO WRDATA) via a registered firmware_helper callback.
 * MDIO download returns ERR1 on this chip — the 8051 requires the
 * parallel UCMEM bus for firmware loading.
 *
 * Sequence (from OpenMDK bcm56840_a0_bmd_init.c _firmware_helper):
 *   1. Set XLPORT_WC_UCMEM_CTRL.ACCESS_MODE = 1 (CPU access)
 *   2. Write firmware in 16-byte chunks to XLPORT_WC_UCMEM_DATA
 *      via SCHAN WRITE_MEM (4 dwords per entry, word-order reversed)
 *   3. Set XLPORT_WC_UCMEM_CTRL.ACCESS_MODE = 0 (WC access)
 *
 * Byte ordering (PPC32 big-endian):
 *   - Read firmware bytes as big-endian uint32_t (no byte swap)
 *   - Reverse word order within each 128-bit entry (wdx ^ 3)
 *   - iowrite32 in kernel handles CPU→PCI byte swap
 *   Matches CDK behavior on big-endian hosts exactly.
 *
 * Must be called with PLL sequencer STOPPED.
 */
/*
 * Download WARPcore uC firmware via UCMEM with full MDIO state machine.
 * Must be called with PLL STOPPED — the MDIO state machine (EXT_MEM_ENABLE)
 * routes UCMEM data to the 8051's internal program RAM during the write.
 *
 * After this function returns, the 8051 is running (UC_RESET_N set) and
 * firmware version is verified. The caller can then start the PLL sequencer,
 * which will reset MICROBLK registers (clearing UC_RESET_N). The caller
 * must re-enable TMR_EN and UC_RESET_N after PLL lock.
 */
static int wc_ucmem_download(int phy, int bus, int port)
{
	uint32_t xlport_base, xlport_blk;
	uint32_t ucmem_entry[4];
	uint32_t fw_size_aligned, idx, wdx, chunk, cnt;
	uint16_t val;
	int rc;

	xlport_base = port_to_xlport_base(port);
	if (xlport_base == 0) {
		fprintf(stderr, "[serdes] PHY %d: no XLPORT base for port %d\n",
			phy, port);
		return -EINVAL;
	}

	/* Extract SBUS block number from CDK extended address format.
	 * Block is used in SCHAN header for routing; CDK addresses
	 * (XLPORT_WC_UCMEM_DATA/CTRL) go in SCHAN msg[1]. */
	xlport_blk = ((xlport_base >> 20) & 0xfu)
		   | ((xlport_base >> 26) & 0x30u);

	fw_size_aligned = (wc40_ucode_b0_bin_len + 15u) & ~15u;
	fprintf(stderr, "[serdes] PHY %d bus %d port %d: downloading %u bytes "
		"firmware via UCMEM (block=%u)\n",
		phy, bus, port, wc40_ucode_b0_bin_len, xlport_blk);

	/* Step 1: Initialize 8051 download state machine via MDIO */

	/* Reset 8051 (critical for warm-boot from Cumulus) */
	wc_write(phy, 0, bus, WC_UC_COMMAND, 0x0000);
	usleep(1000);

	/* Clear download status */
	wc_write(phy, 0, bus, WC_UC_DL_STATUS, 0x0000);

	/* INIT_CMD */
	wc_write(phy, 0, bus, WC_UC_COMMAND, 0x8000);

	/* Poll INIT_DONE (bit 2 only — bit 15 is INIT_CMD echo, always set) */
	val = 0;
	for (cnt = 0; cnt < 500; cnt++) {
		wc_read(phy, 0, bus, WC_UC_DL_STATUS, &val);
		if (val & 0x0004)
			break;
		usleep(1000);
	}
	fprintf(stderr, "[serdes] PHY %d: INIT_DONE poll %s (%u ms, "
		"DL_STATUS=0x%04x)\n", phy,
		(val & 0x0004) ? "OK" : "TIMEOUT", cnt, val);
	/* Continue even without INIT_DONE — on Trident+ the UCMEM
	 * hardware may route firmware to 8051 RAM without MDIO init. */

	/* TMR_EN */
	wc_read(phy, 0, bus, WC_UC_COMMAND2, &val);
	wc_write(phy, 0, bus, WC_UC_COMMAND2, val | 0x0020);

	/* RAMWORD = firmware size - 1 */
	wc_write(phy, 0, bus, WC_UC_RAMWORD,
		 (uint16_t)(wc40_ucode_b0_bin_len - 1));

	/* ADDRESS = 0 */
	wc_write(phy, 0, bus, WC_UC_ADDRESS, 0x0000);

	/* EXT_MEM_ENABLE + EXT_CLK_ENABLE */
	wc_read(phy, 0, bus, WC_UC_COMMAND3, &val);
	wc_write(phy, 0, bus, WC_UC_COMMAND3, val | 0x0001u);
	usleep(1000);
	wc_read(phy, 0, bus, WC_UC_COMMAND3, &val);
	wc_write(phy, 0, bus, WC_UC_COMMAND3, val | 0x0002u);
	usleep(1000);

	/* Step 2: Write firmware to UCMEM via SCHAN WRITE_MEM */

	rc = sbus_reg_write_blk(xlport_blk, XLPORT_WC_UCMEM_CTRL, 1u);
	if (rc != 0) {
		fprintf(stderr, "[serdes] PHY %d: UCMEM_CTRL write failed\n",
			phy);
		return -EIO;
	}

	/* Clear UCMEM first (OpenMDK clears all 4096 entries).
	 * Stale data from previous boot's firmware may confuse 8051. */
	{
		uint32_t zero[4] = {0, 0, 0, 0};
		for (idx = 0; idx < 4096; idx++) {
			rc = sbus_mem_write_blk(xlport_blk,
						XLPORT_WC_UCMEM_DATA,
						(int)idx, zero, 4);
			if (rc != 0) {
				fprintf(stderr, "[serdes] PHY %d: UCMEM clear "
					"failed at entry %u\n", phy, idx);
				sbus_reg_write_blk(xlport_blk,
						   XLPORT_WC_UCMEM_CTRL,
						   0u);
				return -EIO;
			}
		}
		fprintf(stderr, "[serdes] PHY %d: cleared 4096 UCMEM entries\n",
			phy);
	}

	/* Write firmware data */
	for (idx = 0; idx < fw_size_aligned; idx += 16) {
		for (wdx = 0; wdx < 4; wdx++) {
			uint32_t off = idx + wdx * 4;
			uint32_t w = 0;
			if (off < wc40_ucode_b0_bin_len)
				w |= (uint32_t)wc40_ucode_b0_bin[off] << 24;
			if (off + 1 < wc40_ucode_b0_bin_len)
				w |= (uint32_t)wc40_ucode_b0_bin[off + 1] << 16;
			if (off + 2 < wc40_ucode_b0_bin_len)
				w |= (uint32_t)wc40_ucode_b0_bin[off + 2] << 8;
			if (off + 3 < wc40_ucode_b0_bin_len)
				w |= (uint32_t)wc40_ucode_b0_bin[off + 3];
			ucmem_entry[wdx ^ 3] = w;
		}

		chunk = idx >> 4;
		rc = sbus_mem_write_blk(xlport_blk, XLPORT_WC_UCMEM_DATA,
					(int)chunk, ucmem_entry, 4);
		if (rc != 0) {
			fprintf(stderr, "[serdes] PHY %d: UCMEM write failed "
				"at chunk %u\n", phy, chunk);
			sbus_reg_write_blk(xlport_blk,
					   XLPORT_WC_UCMEM_CTRL, 0u);
			return -EIO;
		}
	}

	/* Verify first UCMEM entry by reading it back */
	{
		uint32_t readback[4] = {0, 0, 0, 0};
		if (sbus_mem_read_blk(xlport_blk, XLPORT_WC_UCMEM_DATA,
				      0, readback, 4) == 0) {
			fprintf(stderr, "[serdes] PHY %d: UCMEM[0] readback: "
				"%08x %08x %08x %08x\n", phy,
				readback[0], readback[1],
				readback[2], readback[3]);
		}
	}

	sbus_reg_write_blk(xlport_blk, XLPORT_WC_UCMEM_CTRL, 0u);
	fprintf(stderr, "[serdes] PHY %d: wrote %u UCMEM entries\n",
		phy, fw_size_aligned >> 4);

	/* Step 3: Start 8051 */

	/* Disable EXT_MEM/CLK */
	wc_read(phy, 0, bus, WC_UC_COMMAND3, &val);
	wc_write(phy, 0, bus, WC_UC_COMMAND3, val & ~0x0003u);
	usleep(1000);

	/* Check for errors */
	wc_read(phy, 0, bus, WC_UC_DL_STATUS, &val);
	fprintf(stderr, "[serdes] PHY %d: post-download DL_STATUS=0x%04x\n",
		phy, val);
	if (val & 0x0003) {
		fprintf(stderr, "[serdes] PHY %d: firmware download error\n",
			phy);
		return -EIO;
	}

	/* Disable CRC check */
	wc_write(phy, 0, bus, WC_FW_CRC, 0x0001);

	/* UC_RESET_N: release 8051 from reset.
	 * MUST keep INIT_CMD (bit 15) set alongside UC_RESET_N (bit 4).
	 * Empirically, writing 0x0010 (without INIT_CMD) causes the 8051
	 * to not start. Writing 0x8010 works on this WC-B0 variant. */
	wc_write(phy, 0, bus, WC_UC_COMMAND, 0x8010);

	/* Wait for firmware version */
	val = 0;
	for (cnt = 0; cnt < 500; cnt++) {
		wc_read(phy, 0, bus, WC_FW_VERSION, &val);
		if (val != 0)
			break;
		usleep(1000);
	}
	fprintf(stderr, "[serdes] PHY %d: firmware version=0x%04x (%ums)\n",
		phy, val, cnt);

	return 0;
}

/*--- init_stage_2: post-firmware, post-PLL-lock configuration ---*/

/*
 * OpenMDK _warpcore_init_stage_2 equivalent.
 * Called after firmware download and PLL lock.
 *
 * Critical steps for 10G 64B/66B:
 *   1. Rx clock compensation (CC_EN + CC_DATA_SEL for 1-lane ports)
 *   2. 64/66 sync word configuration (SCW0-3 + masks)
 *   3. Disable PLL powerdown + SGMII auto-detect
 *   4. FIFO elasticity + disable Tx CRS
 *   5. Auto PCS type selection
 *   6. Disable PMA/PMD forced speed encoding
 *
 * Without the sync word configuration (#2), CL49 PCS cannot detect
 * 64B/66B block boundaries and BLOCK_LOCK will never be achieved.
 */
static void wc_init_stage2(int phy, int bus)
{
	uint16_t val;

	/* Rx clock compensation for 1-lane ports (10G SFI).
	 * CC_EN (bit 13) = 1, CC_DATA_SEL (bit 14) = 1 */
	wc_read(phy, 0, bus, WC_RX66_CTRL, &val);
	wc_write(phy, 0, bus, WC_RX66_CTRL, val | (1u << 13) | (1u << 14));

	/* Configure 64/66 sync words and masks.
	 * These define the block sync patterns that CL49 PCS uses to
	 * find 64B/66B block boundaries in the incoming bit stream. */
	wc_write(phy, 0, bus, WC_RX66_SCW0, 0xe070);
	wc_write(phy, 0, bus, WC_RX66_SCW1, 0xc0d0);
	wc_write(phy, 0, bus, WC_RX66_SCW2, 0xa0b0);
	wc_write(phy, 0, bus, WC_RX66_SCW3, 0x8090);
	wc_write(phy, 0, bus, WC_RX66_SCW0_MASK, 0xf0f0);
	wc_write(phy, 0, bus, WC_RX66_SCW1_MASK, 0xf0f0);
	wc_write(phy, 0, bus, WC_RX66_SCW2_MASK, 0xf0f0);
	wc_write(phy, 0, bus, WC_RX66_SCW3_MASK, 0xf0f0);

	/* Auto PCS type selection: MISC4 bit 15 = 1 */
	wc_read(phy, 0, bus, WC_MISC4, &val);
	wc_write(phy, 0, bus, WC_MISC4, val | (1u << 15));

	/* Disable PMA/PMD forced speed encoding: MISC2 bit 5 = 0 */
	wc_read(phy, 0, bus, WC_MISC2, &val);
	wc_write(phy, 0, bus, WC_MISC2, val & ~(1u << 5));

	/* Disable PLL powerdown + disable SGMII/fiber auto-detect:
	 * CONTROL1000X1: DISABLE_PLL_PWRDWN (bit 6) = 1, AUTODET_EN (bit 4) = 0 */
	wc_read(phy, 0, bus, WC_CONTROL1000X1, &val);
	val |= (1u << 6);   /* disable PLL powerdown */
	val &= ~(1u << 4);  /* disable auto-detect */
	wc_write(phy, 0, bus, WC_CONTROL1000X1, val);

	/* FIFO elasticity 13.5k + disable Tx CRS:
	 * CONTROL1000X3: FIFO_ELASICITY_TX[2:1] = 2, DISABLE_TX_CRS (bit 13) = 1 */
	wc_read(phy, 0, bus, WC_CONTROL1000X3, &val);
	val = (val & ~(0x3u << 1)) | (2u << 1);  /* FIFO elasticity = 2 */
	val |= (1u << 13);                        /* disable TX CRS */
	wc_write(phy, 0, bus, WC_CONTROL1000X3, val);

	fprintf(stderr, "[serdes] PHY %d bus %d: init_stage2 complete "
		"(64/66 sync words + clock comp configured)\n", phy, bus);
}

/*--- Per-PHY initialization (once per WARPcore, not per lane) ---*/

/*
 * Initialize a WARPcore PHY: download firmware and start PLL.
 * Follows OpenMDK init_stage0 + firmware_set + init_stage2.
 * port: needed to identify XLPORT block for UCMEM firmware download.
 */
static int wc_phy_init(int phy, int bus, int port)
{
	uint16_t val;
	int cnt, locked = 0;

	fprintf(stderr, "[serdes] PHY %d bus %d: init\n", phy, bus);

	/* Read SERDESID0 to identify WARPcore variant */
	wc_read(phy, 0, bus, WC_SERDESID0, &val);
	fprintf(stderr, "[serdes] PHY %d: SERDESID0=0x%04x\n", phy, val);

	/* Stage 0: Stop PLL sequencer before firmware download */
	wc_read(phy, 0, bus, WC_XGXSCONTROL, &val);
	if (val & (1u << 13)) {
		wc_write(phy, 0, bus, WC_XGXSCONTROL, val & ~(1u << 13));
		usleep(1000);
	}

	/* Disable PRBS on all lanes */
	wc_write(phy, 0, bus, WC_LANEPRBS, 0x0000);

	/* Set PLL divider for 10G (10.3125 Gbps = 66x from 156.25 MHz refclk).
	 * MISC1[11:8] = PLL_MODE_AFE = 2 (66x divider)
	 * MISC1[12]   = PLL_MODE_AFE_SEL = 1 (use explicit PLL mode)
	 * Must be set BEFORE starting PLL sequencer (OpenMDK init_stage0). */
	wc_read(phy, 0, bus, WC_MISC1, &val);
	val &= ~0x1F00u;  /* clear PLL_MODE_AFE[11:8] and PLL_MODE_AFE_SEL[12] */
	val |= (2u << 8) | (1u << 12);  /* AFE=2, SEL=1 */
	wc_write(phy, 0, bus, WC_MISC1, val);

	/*
	 * Download firmware with PLL stopped (monolithic).
	 * The MDIO state machine (EXT_MEM_ENABLE etc.) must be active
	 * during UCMEM write — it routes UCMEM data to the 8051's
	 * internal program RAM in real-time.
	 */
	if (wc_ucmem_download(phy, bus, port) < 0)
		fprintf(stderr, "[serdes] WARNING: firmware download failed "
			"PHY %d bus %d port %d\n", phy, bus, port);

	/* Start PLL sequencer: XGXSCONTROL bit 13 = 1
	 * WARNING: This resets all MICROBLK registers (UC_COMMAND,
	 * UC_COMMAND2, etc.), clearing UC_RESET_N and TMR_EN.
	 * The 8051 stops but its internal program RAM is preserved. */
	wc_read(phy, 0, bus, WC_XGXSCONTROL, &val);
	wc_write(phy, 0, bus, WC_XGXSCONTROL, val | (1u << 13));

	/* Wait for PLL lock (XGXSSTATUS bit 11), up to 200ms */
	for (cnt = 0; cnt < 200; cnt++) {
		wc_read(phy, 0, bus, WC_XGXSSTATUS, &val);
		if (val & (1u << 11)) {
			locked = 1;
			break;
		}
		usleep(1000);
	}
	fprintf(stderr, "[serdes] PHY %d: PLL %s (%dms, XGXSSTATUS=0x%04x)\n",
		phy, locked ? "LOCKED" : "TIMEOUT", cnt, val);

	/*
	 * Restart 8051 after PLL lock.
	 * PLL start cleared MICROBLK registers (UC_COMMAND, UC_COMMAND2),
	 * stopping the 8051. But its program RAM still has the firmware.
	 * Re-enable TMR_EN and set UC_RESET_N to restart it.
	 */
	/* Re-enable timers: COMMAND2 bit 5 = TMR_EN */
	wc_read(phy, 0, bus, WC_UC_COMMAND2, &val);
	fprintf(stderr, "[serdes] PHY %d: UC_COMMAND2 after PLL=0x%04x\n",
		phy, val);
	wc_write(phy, 0, bus, WC_UC_COMMAND2, val | 0x0020);

	/* Release 8051 from reset: INIT_CMD (bit 15) + UC_RESET_N (bit 4) */
	wc_write(phy, 0, bus, WC_UC_COMMAND, 0x8010);

	/* Wait for 8051 to boot — poll firmware version, up to 500ms */
	val = 0;
	for (cnt = 0; cnt < 500; cnt++) {
		wc_read(phy, 0, bus, WC_FW_VERSION, &val);
		if (val != 0)
			break;
		usleep(1000);
	}
	fprintf(stderr, "[serdes] PHY %d: firmware version=0x%04x (%dms)\n",
		phy, val, cnt);

	if (val == 0)
		fprintf(stderr, "[serdes] WARNING: PHY %d firmware did not "
			"start (version=0)\n", phy);

	/* Verify UC_COMMAND state */
	wc_read(phy, 0, bus, WC_UC_COMMAND, &val);
	fprintf(stderr, "[serdes] PHY %d: UC_COMMAND=0x%04x\n", phy, val);

	/* NOTE: init_stage_2 register config (64/66 sync words, clock comp,
	 * CONTROL1000X, etc.) is now done per-lane in bcm56846_serdes_init_10g()
	 * instead of here.  These are per-lane registers in WARPcore — writing
	 * them with lane=0 only configures lane 0, leaving other lanes (e.g.
	 * PHY 31 lane 1 for port 52) without sync word configuration. */

	/* Reset AER to safe state */
	miim_write(phy, bus, WC_REG_AER_BLK, 0);

	return 0;
}

/*--- Public API ---*/

/*
 * Track which PHYs have been initialized.
 * Indexed by bus (0-2), bitmask by PHY address (0-31).
 */
static uint32_t phy_inited[3];

int bcm56846_serdes_init_10g(int unit, int port)
{
	int phy, bus, lane;
	uint16_t val;

	(void)unit;

	if (port_to_phy_bus(port, &phy, &bus, &lane) != 0)
		return -EINVAL;

	miim_divider_init();

	/* One-time per-PHY init: firmware download + PLL.
	 * Keyed by (bus, phy) to avoid skipping bus 1 PHYs
	 * that share the same address as bus 0 PHYs.
	 * Port is passed for XLPORT UCMEM block identification. */
	if (bus < 3 && !(phy_inited[bus] & (1u << phy))) {
		wc_phy_init(phy, bus, port);
		phy_inited[bus] |= (1u << phy);
	}

	fprintf(stderr, "[serdes] port %d -> PHY %d lane %d bus %d: "
		"configuring 10G SFI\n", port, phy, lane, bus);

	/*
	 * OpenMDK speed_set sequence for 1-lane 10G SFI:
	 *   1. Hold Tx/Rx ASIC reset (MISC6)
	 *   2. Set firmware mode (SFP_DAC=2)
	 *   3. Disable FX100 mode
	 *   4. Set forced speed (MISC1 + MISC3)
	 *   5. Release Tx/Rx ASIC reset (MISC6)
	 *
	 * init_stage_2 per-lane registers (sync words, clock comp, etc.)
	 * are written after the speed change.
	 */

	/* 1. Hold Tx/Rx ASIC reset: MISC6 bits 15,14 = 1 */
	wc_read(phy, lane, bus, WC_MISC6, &val);
	wc_write(phy, lane, bus, WC_MISC6, val | 0xC000u);

	/* 2. Set FIRMWARE_MODE for this lane = SFP_DAC (2) */
	wc_read(phy, 0, bus, WC_FW_MODE, &val);
	val &= (uint16_t)~(0xFu << (4 * lane));
	val |= (uint16_t)(2u << (4 * lane));
	wc_write(phy, 0, bus, WC_FW_MODE, val);

	/* 3. Disable FX100 mode (required by OpenMDK for all speed changes):
	 *    FX100_CONTROL1r (0x8400): ENABLE=bit0→0, AUTO_DETECT_FX_MODE=bit2→0
	 *    FX100_CONTROL3r (0x8402): CORRELATOR_DISABLE=bit7→1 */
	if (wc_read(phy, lane, bus, 0x8400u, &val) == 0)
		wc_write(phy, lane, bus, 0x8400u, val & ~0x0005u);
	if (wc_read(phy, lane, bus, 0x8402u, &val) == 0)
		wc_write(phy, lane, bus, 0x8402u, val | (1u << 7));

	/* 3b. Clear CL49 HiGig2 bits for standard 10GBASE-R:
	 * CL49_CONTROL1r defaults to 0x0078 (HiGig2 mode), which uses
	 * different encoding than standard 10G. MUST clear for BLOCK_LOCK. */
	if (wc_read(phy, lane, bus, WC_CL49_CTRL1, &val) == 0) {
		if (val & WC_CL49_HG2_MASK)
			wc_write(phy, lane, bus, WC_CL49_CTRL1,
				 val & ~WC_CL49_HG2_MASK);
	}

	/* 4a. Set forced speed: FV_fdr_10G_SFI = 0x29
	 *     MISC1[4:0] = 0x09, MISC3.FORCE_SPEED_B5 = 1 */
	if (wc_read(phy, lane, bus, WC_MISC1, &val) == 0)
		wc_write(phy, lane, bus, WC_MISC1,
			 (val & ~0x001Fu) | 0x09u);

	/* 4b. MISC3: FORCE_SPEED_B5=1 (bit7), IND_40BITIF=1 (bit15),
	 *     clear LANEDISABLE (bit6) */
	if (wc_read(phy, lane, bus, WC_MISC3, &val) == 0) {
		val |= (1u << 7) | (1u << 15);
		val &= ~(1u << 6);
		wc_write(phy, lane, bus, WC_MISC3, val);
	}

	/* 5. Release Tx/Rx ASIC reset: MISC6 bits 15,14 = 0 */
	wc_read(phy, lane, bus, WC_MISC6, &val);
	wc_write(phy, lane, bus, WC_MISC6, val & ~0xC000u);

	/*
	 * init_stage_2 per-lane: 64B/66B and PCS configuration.
	 * Written AFTER speed change and ASIC reset release.
	 */

	/* Rx clock compensation: CC_EN (bit 13), CC_DATA_SEL (bit 14) */
	if (wc_read(phy, lane, bus, WC_RX66_CTRL, &val) == 0)
		wc_write(phy, lane, bus, WC_RX66_CTRL,
			 val | (1u << 13) | (1u << 14));

	/* 64/66 sync words for CL49 block boundary detection */
	wc_write(phy, lane, bus, WC_RX66_SCW0, 0xe070);
	wc_write(phy, lane, bus, WC_RX66_SCW1, 0xc0d0);
	wc_write(phy, lane, bus, WC_RX66_SCW2, 0xa0b0);
	wc_write(phy, lane, bus, WC_RX66_SCW3, 0x8090);
	wc_write(phy, lane, bus, WC_RX66_SCW0_MASK, 0xf0f0);
	wc_write(phy, lane, bus, WC_RX66_SCW1_MASK, 0xf0f0);
	wc_write(phy, lane, bus, WC_RX66_SCW2_MASK, 0xf0f0);
	wc_write(phy, lane, bus, WC_RX66_SCW3_MASK, 0xf0f0);

	/* FIBER_MODE (bit 0) = 1, PMA_PMD_FORCED_SPEED_ENC_EN (bit 5) = 0 */
	if (wc_read(phy, lane, bus, WC_MISC2, &val) == 0)
		wc_write(phy, lane, bus, WC_MISC2,
			 (val | 0x0001u) & ~(1u << 5));

	/* Disable PLL powerdown, disable SGMII auto-detect */
	if (wc_read(phy, lane, bus, WC_CONTROL1000X1, &val) == 0) {
		val |= (1u << 6);
		val &= ~(1u << 4);
		wc_write(phy, lane, bus, WC_CONTROL1000X1, val);
	}

	/* FIFO elasticity + disable Tx CRS */
	if (wc_read(phy, lane, bus, WC_CONTROL1000X3, &val) == 0) {
		val = (val & ~(0x3u << 1)) | (2u << 1);
		val |= (1u << 13);
		wc_write(phy, lane, bus, WC_CONTROL1000X3, val);
	}

	/* Diagnostic: read CL49 and CL45 PCS status for 10G SFP+ ports. */
	if (port >= 49 && port <= 52) {
		uint16_t cl49_status, cl49_lsm, pcs_status;
		uint16_t sigdet, misc1_rb, misc3_rb, rx66_rb, cl49_ctrl;
		int cnt;

		/*
		 * Enable IEEE loopback for testing without SFPs.
		 * For 1-lane ports, OpenMDK does NOT toggle the PLL sequencer
		 * (that resets FORCE_SPEED!). Just set MDIO_CONT_EN + GLOOP1G.
		 * TODO: remove this once real link testing works.
		 */

		/* Enable MDIO control for loopback mode */
		wc_read(phy, 0, bus, WC_XGXSCONTROL, &val);
		wc_write(phy, 0, bus, WC_XGXSCONTROL, val | (1u << 4));

		/* Set GLOOP1G for this lane (no sequencer toggle for 1-lane) */
		wc_read(phy, lane, bus, 0x8017u, &val);
		wc_write(phy, lane, bus, 0x8017u, val | (1u << lane));

		usleep(500000); /* 500ms settle for PCS to lock */

		/* Readback key config registers to verify writes */
		wc_read(phy, lane, bus, WC_ANARXSTATUS, &sigdet);
		wc_read(phy, lane, bus, WC_MISC1, &misc1_rb);
		wc_read(phy, lane, bus, WC_MISC3, &misc3_rb);
		wc_read(phy, lane, bus, WC_RX66_CTRL, &rx66_rb);
		wc_read(phy, lane, bus, WC_CL49_CTRL1, &cl49_ctrl);
		fprintf(stderr, "[serdes] port %d diag: SIGDET=0x%04x "
			"MISC1=0x%04x MISC3=0x%04x RX66_CTRL=0x%04x "
			"CL49_CTRL1=0x%04x\n",
			port, sigdet, misc1_rb, misc3_rb, rx66_rb, cl49_ctrl);

		/* Verify sync word writes */
		{
			uint16_t scw0, scw0m;
			wc_read(phy, lane, bus, WC_RX66_SCW0, &scw0);
			wc_read(phy, lane, bus, WC_RX66_SCW0_MASK, &scw0m);
			fprintf(stderr, "[serdes] port %d diag: "
				"SCW0=0x%04x SCW0_MASK=0x%04x "
				"(expect e070/f0f0)\n",
				port, scw0, scw0m);
		}

		/* Poll CL49 status — may take time to achieve BLOCK_LOCK */
		for (cnt = 0; cnt < 5; cnt++) {
			wc_read(phy, lane, bus, 0x8367u, &cl49_status);
			wc_read(phy, lane, bus, 0x8368u, &cl49_lsm);
			wc_cl45_read(phy, bus, 3, 0x0001, &pcs_status);

			fprintf(stderr, "[serdes] port %d [%d]: "
				"CL49_STATUS=0x%04x CL49_LSM=0x%04x "
				"PCS=0x%04x BLOCK_LOCK=%d LINK=%d\n",
				port, cnt, cl49_status, cl49_lsm, pcs_status,
				(cl49_status >> 15) & 1,
				(pcs_status >> 2) & 1);
			if ((cl49_status >> 15) & 1)
				break;
			usleep(200000);
		}
	}

	/* Reset AER to safe state */
	miim_write(phy, bus, WC_REG_AER_BLK, 0);

	fprintf(stderr, "[serdes] port %d: 10G SFI configured\n", port);
	return 0;
}

int bcm56846_serdes_link_get(int unit, int port, int *link_up)
{
	int phy, bus, lane;
	uint16_t pcs_status;

	(void)unit;

	if (!link_up)
		return -EINVAL;
	*link_up = 0;

	if (port_to_phy_bus(port, &phy, &bus, &lane) != 0)
		return -EINVAL;

	miim_divider_init();

	/* Read CL45 PCS_IEEESTATUS1r: devad=3, reg=1.
	 * Bit 2 = RX_LINK_STATUS (latched low). */
	if (wc_cl45_read(phy, bus, 3, 0x0001, &pcs_status) != 0)
		return -EIO;

	*link_up = (pcs_status >> 2) & 1;

	/* Reset AER */
	miim_write(phy, bus, WC_REG_AER_BLK, 0);

	return 0;
}
