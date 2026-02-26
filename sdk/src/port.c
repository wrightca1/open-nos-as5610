/* Port enable/speed/link — XLPORT/MAC regs via S-Channel (RE: PORT_BRINGUP_REGISTER_MAP.md) */
#include "bcm56846.h"
#include <errno.h>
#include <string.h>

extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);
extern int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words);

#define XLPORT_PORT_ENABLE_OFF   (0x80000u + 0x22au)
#define MAC_MODE_OFF             0x511u
#define MAC_MODE_LINK_STATUS_BIT 5 /* value 0x22 => speed=2 (bits1:0), link=1 (bit5) */

struct xlport_block {
	uint32_t base;
	int ports; /* 1 or 4 */
};

/* Map xe port index (0..51) to xlport block base and lane.
 * Note: xe20–xe27 have lane permutation (PCB routing).
 */
static int xe_to_block_lane(int xe, uint32_t *block_base, int *lane, int *block_idx)
{
	static const struct xlport_block blocks[] = {
		/* 0: xe0-3 */   { 0x40a00000u, 4 },
		/* 1: xe4-7 */   { 0x40b00000u, 4 },
		/* 2: xe8-11 */  { 0x00b00000u, 4 },
		/* 3: xe12-15 */ { 0x00c00000u, 4 },
		/* 4: xe16-19 */ { 0x00d00000u, 4 },
		/* 5: xe20-23 */ { 0x00e00000u, 4 },
		/* 6: xe24-27 */ { 0x00f00000u, 4 },
		/* 7: xe28-31 */ { 0x40000000u, 4 },
		/* 8: xe32-35 */ { 0x40100000u, 4 },
		/* 9: xe36-39 */ { 0x40200000u, 4 },
		/* 10: xe40-43 */{ 0x40300000u, 4 },
		/* 11: xe44-47 */{ 0x40400000u, 4 },
		/* 12: xe48 */   { 0x40600000u, 1 },
		/* 13: xe49 */   { 0x40500000u, 1 },
		/* 14: xe50 */   { 0x40900000u, 1 },
		/* 15: xe51 */   { 0x40800000u, 1 },
	};
	static const int permute_20_23[4] = { 1, 0, 3, 2 };

	if (!block_base || !lane || !block_idx)
		return -1;
	if (xe < 0 || xe > 51)
		return -1;

	if (xe <= 3) {
		*block_idx = 0; *block_base = blocks[0].base; *lane = xe;
		return 0;
	}
	if (xe <= 7) {
		*block_idx = 1; *block_base = blocks[1].base; *lane = xe - 4;
		return 0;
	}
	if (xe <= 11) {
		*block_idx = 2; *block_base = blocks[2].base; *lane = xe - 8;
		return 0;
	}
	if (xe <= 15) {
		*block_idx = 3; *block_base = blocks[3].base; *lane = xe - 12;
		return 0;
	}
	if (xe <= 19) {
		*block_idx = 4; *block_base = blocks[4].base; *lane = xe - 16;
		return 0;
	}
	if (xe <= 23) {
		int o = xe - 20;
		*block_idx = 5; *block_base = blocks[5].base; *lane = permute_20_23[o];
		return 0;
	}
	if (xe <= 27) {
		int o = xe - 24;
		*block_idx = 6; *block_base = blocks[6].base; *lane = permute_20_23[o];
		return 0;
	}
	if (xe <= 31) {
		*block_idx = 7; *block_base = blocks[7].base; *lane = xe - 28;
		return 0;
	}
	if (xe <= 35) {
		*block_idx = 8; *block_base = blocks[8].base; *lane = xe - 32;
		return 0;
	}
	if (xe <= 39) {
		*block_idx = 9; *block_base = blocks[9].base; *lane = xe - 36;
		return 0;
	}
	if (xe <= 43) {
		*block_idx = 10; *block_base = blocks[10].base; *lane = xe - 40;
		return 0;
	}
	if (xe <= 47) {
		*block_idx = 11; *block_base = blocks[11].base; *lane = xe - 44;
		return 0;
	}
	if (xe == 48) {
		*block_idx = 12; *block_base = blocks[12].base; *lane = 0;
		return 0;
	}
	if (xe == 49) {
		*block_idx = 13; *block_base = blocks[13].base; *lane = 0;
		return 0;
	}
	if (xe == 50) {
		*block_idx = 14; *block_base = blocks[14].base; *lane = 0;
		return 0;
	}
	/* xe == 51 */
	*block_idx = 15; *block_base = blocks[15].base; *lane = 0;
	return 0;
}

static uint32_t xlport_enable_shadow[16];

int bcm56846_port_enable_set(int unit, int port, int enable)
{
	int xe = port - 1;
	uint32_t base;
	int lane, block_idx;
	uint32_t val;
	uint32_t addr;

	if (port <= 0 || port > 52)
		return -EINVAL;
	if (xe_to_block_lane(xe, &base, &lane, &block_idx) != 0)
		return -EINVAL;

	if (enable)
		xlport_enable_shadow[block_idx] |= (1u << lane);
	else
		xlport_enable_shadow[block_idx] &= ~(1u << lane);

	val = xlport_enable_shadow[block_idx];
	addr = base + XLPORT_PORT_ENABLE_OFF;
	if (schan_write_memory(unit, addr, &val, 1) != 0)
		return -EIO;
	if (enable)
		bcm56846_serdes_init_10g(unit, port);
	return 0;
}

int bcm56846_port_speed_set(int unit, int port, int speed_mbps)
{
	(void)unit;
	(void)port;
	/* Warpcore MDIO programming sequence is RE'd but not yet implemented here.
	 * For AS5610-52X we operate ports as 10G (MAC_SPEED=2) by default.
	 */
	if (speed_mbps == 10000)
		return 0;
	return -ENOTSUP;
}

int bcm56846_port_link_status_get(int unit, int port, int *link_up)
{
	int xe = port - 1;
	uint32_t base;
	int lane, block_idx;
	uint32_t addr;
	uint32_t val = 0;

	if (!link_up)
		return -EINVAL;
	*link_up = 0;
	if (port <= 0 || port > 52)
		return -EINVAL;
	if (xe_to_block_lane(xe, &base, &lane, &block_idx) != 0)
		return -EINVAL;

	addr = base + (uint32_t)lane * 0x1000u + MAC_MODE_OFF;
	if (schan_read_memory(unit, addr, &val, 1) != 0) {
		/* If read isn't supported yet, fall back to admin enable state. */
		*link_up = (xlport_enable_shadow[block_idx] & (1u << lane)) ? 1 : 0;
		return 0;
	}

	*link_up = ((val >> MAC_MODE_LINK_STATUS_BIT) & 1u) ? 1 : 0;
	return 0;
}
