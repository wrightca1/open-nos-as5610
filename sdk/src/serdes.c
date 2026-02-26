/* Warpcore WC-B0 SerDes MDIO init (RE: SERDES_WC_INIT.md). */
#include "bcm56846.h"
#include "bcm56846_regs.h"
#include <errno.h>
#include <string.h>

extern int bde_read_reg(uint32_t offset, uint32_t *value);
extern int bde_write_reg(uint32_t offset, uint32_t value);

/* MIIM_PARAM: INTERNAL_SEL=1 (bit 25), BUS_ID (bits 24:22), PHY_ADDR (20:16), DATA (15:0). */
#define MIIM_PARAM_INTERNAL (1u << 25)
#define MIIM_PARAM_BUS_ID(b) (((uint32_t)(b) & 7u) << 22)
#define MIIM_PARAM_PHY(pa)   (((uint32_t)(pa) & 31u) << 16)
#define MIIM_PARAM_DATA(d)  ((uint32_t)(d) & 0xffffu)

/* Clause-22 page register */
#define MDIO_PAGE_REG 0x1f

static int wc_mdio_write(int unit, int phy_addr, int bus_id, uint32_t reg, uint16_t data)
{
	uint32_t param = MIIM_PARAM_INTERNAL | MIIM_PARAM_BUS_ID(bus_id)
		| MIIM_PARAM_PHY(phy_addr) | MIIM_PARAM_DATA(data);
	(void)unit;
	if (bde_write_reg(CMIC_MIIM_PARAM, param) != 0)
		return -EIO;
	if (bde_write_reg(CMIC_MIIM_ADDRESS, reg & 0x1f) != 0)
		return -EIO;
	return 0;
}

static int wc_mdio_read(int unit, int phy_addr, int bus_id, uint32_t reg, uint16_t *data)
{
	uint32_t param = MIIM_PARAM_INTERNAL | MIIM_PARAM_BUS_ID(bus_id)
		| MIIM_PARAM_PHY(phy_addr) | MIIM_PARAM_DATA(0);
	uint32_t val = 0;
	(void)unit;
	if (bde_write_reg(CMIC_MIIM_PARAM, param) != 0)
		return -EIO;
	if (bde_write_reg(CMIC_MIIM_ADDRESS, reg & 0x1f) != 0)
		return -EIO;
	if (bde_read_reg(CMIC_MIIM_PARAM, &val) != 0)
		return -EIO;
	if (data)
		*data = (uint16_t)(val & 0xffffu);
	return 0;
}

static int page_select(int unit, int phy_addr, int bus_id, uint16_t page)
{
	return wc_mdio_write(unit, phy_addr, bus_id, MDIO_PAGE_REG, page);
}

/* xe 0..7: PHY 17+lane, BUS 2 (RE SERDES_WC_INIT.md). xe8: PHY 5. Others: extrapolate. */
static void xe_to_phy_bus(int xe, int *phy_addr, int *bus_id)
{
	if (xe >= 0 && xe <= 7) {
		*phy_addr = 17 + xe;
		*bus_id = 2;
	} else if (xe == 8) {
		*phy_addr = 5;
		*bus_id = 1;
	} else {
		*phy_addr = 17 + (xe % 8);
		*bus_id = 2;
	}
}

/* Run Warpcore WC-B0 10G SFI init sequence (SERDES_WC_INIT.md §4). */
int bcm56846_serdes_init_10g(int unit, int port)
{
	int xe = port <= 0 || port > 52 ? -1 : port - 1;
	int phy_addr, bus_id;
	if (xe < 0)
		return -EINVAL;
	xe_to_phy_bus(xe, &phy_addr, &bus_id);

	/* 4.1 TX config */
	page_select(unit, phy_addr, bus_id, 0x0000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x17, 0x8010);
	wc_mdio_write(unit, phy_addr, bus_id, 0x18, 0x8370);
	wc_mdio_write(unit, phy_addr, bus_id, 0x18, 0x8370);
	wc_mdio_write(unit, phy_addr, bus_id, 0x18, 0x8370);

	/* 4.2 IEEE block enable */
	page_select(unit, phy_addr, bus_id, 0x0008);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0x8000);
	page_select(unit, phy_addr, bus_id, 0x0000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0x8000);

	/* 4.3 AN/clock recovery */
	page_select(unit, phy_addr, bus_id, 0x1000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x18, 0x8010);
	wc_mdio_write(unit, phy_addr, bus_id, 0x18, 0x8010);
	wc_mdio_write(unit, phy_addr, bus_id, 0x18, 0x8010);

	/* 4.4 SerDes digital control */
	page_select(unit, phy_addr, bus_id, 0x0a00);
	wc_mdio_write(unit, phy_addr, bus_id, 0x10, 0xffe0);

	/* 4.5 Extended page 0 */
	page_select(unit, phy_addr, bus_id, 0x0000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x14, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0xffd0);

	/* 4.6 Core sequencer read (poll ready) */
	page_select(unit, phy_addr, bus_id, 0x3800);
	wc_mdio_read(unit, phy_addr, bus_id, 0x00, NULL);
	page_select(unit, phy_addr, bus_id, 0x0000);

	/* 4.7 RX equalization */
	page_select(unit, phy_addr, bus_id, 0x0000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x11, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x19, 0x8320);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1a, 0x8320);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1b, 0x8320);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1d, 0x8350);
	wc_mdio_write(unit, phy_addr, bus_id, 0x14, 0xffe0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0xffd0);

	/* 4.8 Core sequencer start */
	page_select(unit, phy_addr, bus_id, 0x3800);
	wc_mdio_write(unit, phy_addr, bus_id, 0x01, 0x0010);
	page_select(unit, phy_addr, bus_id, 0x0000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0xffd0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0xffd0);
	page_select(unit, phy_addr, bus_id, 0x3800);
	wc_mdio_write(unit, phy_addr, bus_id, 0x00, 0x0010);

	/* 4.9 Post-init tuning */
	page_select(unit, phy_addr, bus_id, 0x0000);
	wc_mdio_write(unit, phy_addr, bus_id, 0x10, 0xffe0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x14, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x11, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x12, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x10, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x14, 0x8300);
	wc_mdio_write(unit, phy_addr, bus_id, 0x17, 0x8010);
	wc_mdio_write(unit, phy_addr, bus_id, 0x12, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x10, 0x81d0);
	wc_mdio_write(unit, phy_addr, bus_id, 0x1e, 0xffd0);

	return 0;
}
