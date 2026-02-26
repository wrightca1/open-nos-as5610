/*
 * Port statistics — XLMAC counters via S-Channel (RE: STATS_COUNTER_FORMAT.md).
 * S-Channel address = (block_id << 20) | (lane << 12) | reg_offset.
 * Counters are 64-bit (two 32-bit reads). Best-effort if READ_MEMORY does not
 * access register space on this ASIC.
 */
#include "bcm56846.h"
#include <errno.h>
#include <string.h>

extern int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words);

/* Block/lane map for xe0..xe51 (STATS_COUNTER_FORMAT §2). */
struct xlport_stat {
	uint32_t block_id;
	int base_xe;
	int lane[4];
};
static const struct xlport_stat stat_blocks[] = {
	{ 0x40a,  0, { 0, 1, 2, 3 } },
	{ 0x40b,  4, { 0, 1, 2, 3 } },
	{ 0x00b,  8, { 0, 1, 2, 3 } },
	{ 0x00c, 12, { 0, 1, 2, 3 } },
	{ 0x00d, 16, { 0, 1, 2, 3 } },
	{ 0x00e, 20, { 1, 0, 3, 2 } },
	{ 0x00f, 24, { 1, 0, 3, 2 } },
	{ 0x400, 28, { 0, 1, 2, 3 } },
	{ 0x401, 32, { 0, 1, 2, 3 } },
	{ 0x402, 36, { 0, 1, 2, 3 } },
	{ 0x403, 40, { 0, 1, 2, 3 } },
	{ 0x404, 44, { 0, 1, 2, 3 } },
	{ 0x406, 48, { 0, 0, 0, 0 } },
	{ 0x405, 49, { 0, 0, 0, 0 } },
	{ 0x409, 50, { 0, 0, 0, 0 } },
	{ 0x408, 51, { 0, 0, 0, 0 } },
};

static int stat_reg_addr(int xcport, uint32_t reg_offset, uint32_t *addr)
{
	int i;
	for (i = 0; i < (int)(sizeof(stat_blocks) / sizeof(stat_blocks[0])); i++) {
		int base = stat_blocks[i].base_xe;
		int span = (base >= 48) ? 1 : 4;
		if (xcport < base || xcport >= base + span)
			continue;
		int lane = stat_blocks[i].lane[xcport - base];
		*addr = (stat_blocks[i].block_id << 20) | ((uint32_t)lane << 12) | reg_offset;
		return 0;
	}
	return -1;
}

/* XLMAC counter reg offsets (64-bit: two 32-bit words). */
#define REG_RPKT  0x0b
#define REG_RBYT  0x34
#define REG_TPKT  0x45
#define REG_TBYT  0x64

int bcm56846_stat_get(int unit, int port, bcm56846_stat_t stat, uint64_t *value)
{
	uint32_t addr;
	uint32_t words[2];
	uint32_t reg;

	if (!value || port <= 0 || port > 52)
		return -EINVAL;
	switch (stat) {
	case BCM56846_STAT_RPKT: reg = REG_RPKT; break;
	case BCM56846_STAT_RBYT: reg = REG_RBYT; break;
	case BCM56846_STAT_TPKT: reg = REG_TPKT; break;
	case BCM56846_STAT_TBYT: reg = REG_TBYT; break;
	default:
		return -EINVAL;
	}
	if (stat_reg_addr(port - 1, reg, &addr) != 0)
		return -EINVAL;
	memset(words, 0, sizeof(words));
	if (schan_read_memory(unit, addr, words, 2) != 0)
		return -EIO;
	*value = ((uint64_t)words[0] << 32) | words[1];
	return 0;
}
