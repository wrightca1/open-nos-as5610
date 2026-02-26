/*
 * L2 table — L2_ENTRY (hash, 131072 entries, 13 bytes = 4 words).
 * Pack entry from bcm56846_l2_addr_t; hash key from MAC+VLAN; write via S-Channel WRITE_MEMORY.
 * RE: L2_ENTRY_FORMAT.md, SWITCHD_L3_ROUTE_PROGRAMMING_ANALYSIS.md
 */
#include "bcm56846.h"
#include "bcm56846_regs.h"
#include <errno.h>
#include <string.h>

#define L2_ENTRY_BASE     0x07120000
#define L2_ENTRY_ENTRIES  131072
#define L2_ENTRY_WORDS    4
#define L2_ENTRY_STRIDE   16   /* 13-byte entry, 16-byte aligned */
#define KEY_TYPE_L2       0

extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);

/* Hash key: (MAC<<16)|(VLAN<<4)|(KEY_TYPE<<1)|0. VALID=0 in key. */
static uint64_t l2_hash_key(const uint8_t mac[6], uint16_t vid)
{
	uint64_t mac48 = (uint64_t)mac[0] << 40 | (uint64_t)mac[1] << 32 |
			 (uint64_t)mac[2] << 24 | (uint64_t)mac[3] << 16 |
			 (uint64_t)mac[4] << 8 | mac[5];
	return (mac48 << 16) | ((uint64_t)(vid & 0xfff) << 4) | (KEY_TYPE_L2 << 1);
}

/* Pack L2_ENTRY 4 words. Entry bits: VALID@0, KEY_TYPE@3:1, VLAN@15:4, MAC@63:16, PORT@70:64, MODULE@78:71, T@79, STATIC@93. */
static void l2_pack_entry(const bcm56846_l2_addr_t *addr, uint32_t *words)
{
	uint32_t w0, w1, w2;
	uint16_t vid = addr->vid & 0xfff;
	int port = addr->port & 0x7f;
	int mod = 0;
	int static_bit = addr->static_entry ? 1 : 0;

	/* word[0]: VALID=1, KEY_TYPE=0, VLAN_ID[15:4], MAC[47:32] in bits[31:16] */
	w0 = 1u | (KEY_TYPE_L2 << 1) | ((uint32_t)vid << 4) |
	     ((uint32_t)((addr->mac[0] << 8) | addr->mac[1]) << 16);
	/* word[1]: MAC[31:0] */
	w1 = (uint32_t)addr->mac[2] << 24 | (uint32_t)addr->mac[3] << 16 |
	     (uint32_t)addr->mac[4] << 8 | addr->mac[5];
	/* word[2]: PORT_NUM[6:0], MODULE_ID[14:7], T=0, STATIC_BIT[29] */
	w2 = (uint32_t)port | ((uint32_t)(mod & 0xff) << 7) | ((uint32_t)static_bit << 29);

	words[0] = w0;
	words[1] = w1;
	words[2] = w2;
	words[3] = 0;
}

/* Write L2_ENTRY at index via S-Channel WRITE_MEMORY (opcode 0x28, addr = base + index*stride). */
static int l2_table_write(int unit, int index, const uint32_t *words)
{
	uint32_t addr = L2_ENTRY_BASE + (uint32_t)index * L2_ENTRY_STRIDE;
	return schan_write_memory(unit, addr, words, L2_ENTRY_WORDS);
}

/* Delete: write all-zero (VALID=0) at index. */
static int l2_table_delete_at(int unit, int index)
{
	uint32_t words[L2_ENTRY_WORDS] = { 0, 0, 0, 0 };
	return l2_table_write(unit, index, words);
}

int bcm56846_l2_addr_add(int unit, const bcm56846_l2_addr_t *addr)
{
	uint32_t words[L2_ENTRY_WORDS];
	uint64_t key;
	int index;
	int probe;

	if (!addr)
		return -EINVAL;
	l2_pack_entry(addr, words);
	key = l2_hash_key(addr->mac, addr->vid);
	index = (int)(key % L2_ENTRY_ENTRIES);
	if (index < 0)
		index += L2_ENTRY_ENTRIES;
	/* Linear probe 0..5 for collision (per L2_WRITE_PATH: retry 0..5) */
	for (probe = 0; probe < 6; probe++) {
		int idx = (index + probe) % L2_ENTRY_ENTRIES;
		if (l2_table_write(unit, idx, words) == 0)
			return 0;
	}
	return -ENOSPC;
}

int bcm56846_l2_addr_delete(int unit, const uint8_t mac[6], uint16_t vid)
{
	uint64_t key;
	int index;
	int probe;

	if (!mac)
		return -EINVAL;
	key = l2_hash_key(mac, vid);
	index = (int)(key % L2_ENTRY_ENTRIES);
	if (index < 0)
		index += L2_ENTRY_ENTRIES;
	for (probe = 0; probe < 6; probe++) {
		int idx = (index + probe) % L2_ENTRY_ENTRIES;
		if (l2_table_delete_at(unit, idx) == 0)
			return 0;
	}
	return 0; /* best-effort: may not have been present */
}

int bcm56846_l2_addr_get(int unit, const uint8_t mac[6], uint16_t vid, bcm56846_l2_addr_t *out)
{
	(void)unit;
	(void)mac;
	(void)vid;
	(void)out;
	/* TODO: S-Channel read at hash index, parse words into out */
	return -ENOSYS;
}
