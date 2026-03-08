/*
 * Warpcore WC-B0 SerDes MDIO init for BCM56846 (Trident+).
 *
 * Init sequence reverse-engineered from hardware probing on AS5610-52X.
 * Confirmed working via Python scripts: IEEE loopback achieves CL49
 * BLOCK_LOCK on all 4 lanes, CL45 PCS reports link UP.
 *
 * Key findings:
 *  - MIIM registers: CTRL=0x50, PARAM=0x158, ADDR=0x4A0, READ_DATA=0x15C
 *  - CL49_CONTROL1r defaults to 0x0078 (HiGig2 enabled) -- must clear for
 *    standard 10G SFI or CL49 block lock will never be achieved.
 *  - LANEPRBS (0x8015) may default to 0x00FF (PRBS enabled) -- must clear.
 *  - GP0 SYNC_STATUS_COMBO=0 is NORMAL for 10G SFI with FIBER_MODE=1.
 *    Use CL45 PCS registers (devad 3, reg 1) for 10G link status.
 *
 * Port-to-PHY mapping (empirical, 10G SFP+ ports):
 *  - SFP I2C buses 10-13 -> WARPcore PHY 13,17,31 on MDIO bus 1
 *    (confirmed by SIGDET appearing on these PHYs when SFPs have RX light)
 *  - Exact port-number-to-PHY mapping TBD; using best-effort from Cumulus RE.
 */
#include "bcm56846.h"
#include "bcm56846_regs.h"
#include <errno.h>

extern int bde_read_reg(uint32_t offset, uint32_t *value);
extern int bde_write_reg(uint32_t offset, uint32_t value);

/* MIIM parameter encoding */
#define MIIM_PARAM_INTERNAL (1u << 25)
#define MIIM_PARAM_BUS_ID(b) (((uint32_t)(b) & 7u) << 22)
#define MIIM_PARAM_PHY(pa)   (((uint32_t)(pa) & 31u) << 16)
#define MIIM_PARAM_DATA(d)   ((uint32_t)(d) & 0xffffu)

/* WARPcore AER addressing */
#define WC_REG_AER_BLK    0x1f   /* block select register */
#define WC_REG_AER_LANE   0x1e   /* lane/devad select register */
#define WC_AER_BROADCAST  0xffd0 /* AER broadcast block for block select */

/* WARPcore register blocks and offsets */
#define WC_BLK_XGXSBLK1       0x8010u
#define WC_OFF_LANEPRBS        5       /* LANEPRBS register within XGXSBLK1 */

#define WC_BLK_MISC1           0x8308u
#define WC_OFF_MISC1           0       /* MISC1 (speed control) */
#define WC_OFF_MISC2           5       /* MISC2 (FIBER_MODE etc) */

#define WC_BLK_CL49_CTRL      0x8360u
#define WC_OFF_CL49_CTRL1     0       /* CL49_CONTROL1r */
#define WC_OFF_CL49_LSM       7       /* CL49_SM_STATUS_LANE */

#define WC_BLK_COMBO_MII      0xFFE0u
#define WC_OFF_COMBO_MIICTL   0       /* COMBO_MII_CONTROL */

#define WC_BLK_GP2             0x81D0u /* GP2 status registers */

/* Speed values for MISC1[5:0] */
#define WC_SPEED_10G_SFI       0x1fu

/* CL49_CONTROL1r HiGig2 bits to clear for standard 10G */
#define WC_CL49_HG2_MASK      0x077cu

/* COMBO_MIICONTROL bits */
#define WC_MIICTL_AN_EN        (1u << 12)

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

static int wc_read_ln(int phy, int lane, int bus, uint16_t block, int offset,
		       uint16_t *val)
{
	if (miim_write(phy, bus, WC_REG_AER_BLK, WC_AER_BROADCAST) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_LANE, lane & 0x3) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_BLK, block) != 0)
		return -EIO;
	return miim_read(phy, bus, 0x10 + (offset & 0xf), val);
}

static int wc_write_ln(int phy, int lane, int bus, uint16_t block, int offset,
			uint16_t val)
{
	if (miim_write(phy, bus, WC_REG_AER_BLK, WC_AER_BROADCAST) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_LANE, lane & 0x3) != 0)
		return -EIO;
	if (miim_write(phy, bus, WC_REG_AER_BLK, block) != 0)
		return -EIO;
	return miim_write(phy, bus, 0x10 + (offset & 0xf), val);
}

/* Broadcast write (lane=0, AER selects all lanes for broadcast block) */
static int wc_write(int phy, int bus, uint16_t block, int offset, uint16_t val)
{
	return wc_write_ln(phy, 0, bus, block, offset, val);
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

/*
 * Map port number (1-based) to MDIO bus and PHY address.
 *
 * Empirical mapping from SIGDET probing on AS5610-52X:
 *   SFP I2C buses 10,11,13 -> PHY 13,17,31 on MDIO bus 1
 *   (3 of 4 SFPs had RX light = 3 PHYs with SIGDET)
 *
 * 1G ports (1-48) use MDIO buses 0-2 with standard WARPcore PHY addresses.
 * Exact 1G port mapping is best-effort from WARPcore scan results.
 */
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
		/* TBD -- may be a 4th PHY on MDIO bus 1 or PHY31 lane 1 */
		*phy_addr = 31; *bus_id = 1; *lane = 1;
		return 0;
	}

	/* 1G ports: 4 ports per WARPcore, grouped into blocks of 4.
	 * MDIO0: PHY 01,05,09,13,17,21,31 (7 WCs = 28 lanes)
	 * MDIO2: PHY 01,09,13,17,21,31    (6 WCs = 24 lanes)
	 * Total 52 lanes for 48 1G ports + 4 10G ports. */
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

/*--- Public API ---*/

/*
 * Initialize a WARPcore for 10G SFI operation.
 *
 * This performs the minimum configuration needed:
 *  1. Set MDIO clock dividers
 *  2. Disable PRBS (may be enabled by default)
 *  3. Clear CL49 HiGig2 bits (default 0x0078 breaks block lock)
 *  4. Set speed to 10G SFI (0x1F)
 *  5. Set FIBER_MODE
 *  6. Disable auto-negotiation
 *
 * After calling this, PLL should lock (GP0[7:4] non-zero) and
 * CL49 block lock should be achievable if valid 10G signal is present.
 * Use bcm56846_serdes_link_get() for link status.
 */
int bcm56846_serdes_init_10g(int unit, int port)
{
	int phy, bus, lane;
	uint16_t val;

	(void)unit;

	if (port_to_phy_bus(port, &phy, &bus, &lane) != 0)
		return -EINVAL;

	miim_divider_init();

	/* 1. Disable PRBS on all lanes (may default to 0x00FF = enabled) */
	wc_write(phy, bus, WC_BLK_XGXSBLK1, WC_OFF_LANEPRBS, 0x0000);

	/* 2. Clear CL49_CONTROL1r HiGig2 bits on target lane.
	 *    Default value 0x0078 has TX_HIGIG2_EN and TX_HIGIG2_TB_EN set,
	 *    which generates HiGig2-format blocks that standard CL49 RX
	 *    cannot lock onto. */
	if (wc_read_ln(phy, lane, bus, WC_BLK_CL49_CTRL, WC_OFF_CL49_CTRL1,
		       &val) == 0) {
		if (val & WC_CL49_HG2_MASK)
			wc_write_ln(phy, lane, bus, WC_BLK_CL49_CTRL,
				    WC_OFF_CL49_CTRL1,
				    val & (uint16_t)~WC_CL49_HG2_MASK);
	}

	/* 3. Set speed to 10G SFI on target lane.
	 *    MISC1[5:0] = force_speed = 0x1F */
	if (wc_read_ln(phy, lane, bus, WC_BLK_MISC1, WC_OFF_MISC1, &val) == 0)
		wc_write_ln(phy, lane, bus, WC_BLK_MISC1, WC_OFF_MISC1,
			    (val & ~0x003fu) | WC_SPEED_10G_SFI);

	/* 4. Set FIBER_MODE (MISC2[0] = 1) */
	if (wc_read_ln(phy, lane, bus, WC_BLK_MISC1, WC_OFF_MISC2, &val) == 0)
		wc_write_ln(phy, lane, bus, WC_BLK_MISC1, WC_OFF_MISC2,
			    val | 0x0001u);

	/* 5. Disable auto-negotiation (COMBO_MIICONTROL bit 12) */
	if (wc_read_ln(phy, lane, bus, WC_BLK_COMBO_MII, WC_OFF_COMBO_MIICTL,
		       &val) == 0) {
		if (val & WC_MIICTL_AN_EN)
			wc_write_ln(phy, lane, bus, WC_BLK_COMBO_MII,
				    WC_OFF_COMBO_MIICTL,
				    val & ~WC_MIICTL_AN_EN);
	}

	/* Reset AER to safe state */
	miim_write(phy, bus, WC_REG_AER_BLK, 0);

	return 0;
}

/*
 * Get 10G link status via CL45 PCS registers.
 *
 * For 10G SFI, GP0 SYNC_STATUS_COMBO is always 0 (normal with FIBER_MODE=1).
 * Link status must be read from CL45 PCS_IEEESTATUS1r (devad 3, register 1).
 *
 * Returns: 0 on success, *link_up set to 1 if PCS link is up.
 */
int bcm56846_serdes_link_get(int unit, int port, int *link_up)
{
	int phy, bus, lane;
	uint16_t pcs_status;

	(void)unit;
	(void)lane;

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
