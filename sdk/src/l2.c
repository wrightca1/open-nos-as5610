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
extern int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words);

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

/* Read L2_ENTRY at index via S-Channel READ_MEMORY. */
static int l2_table_read(int unit, int index, uint32_t *words)
{
	uint32_t addr = L2_ENTRY_BASE + (uint32_t)index * L2_ENTRY_STRIDE;
	return schan_read_memory(unit, addr, words, L2_ENTRY_WORDS);
}

/* Delete: write all-zero (VALID=0) at index. */
static int l2_table_delete_at(int unit, int index)
{
	uint32_t words[L2_ENTRY_WORDS] = { 0, 0, 0, 0 };
	return l2_table_write(unit, index, words);
}

/* Unpack 4 words into l2_addr (VALID, VLAN, MAC, PORT, STATIC). */
static void l2_unpack_entry(const uint32_t *words, bcm56846_l2_addr_t *addr)
{
	addr->vid = (uint16_t)((words[0] >> 4) & 0xfff);
	addr->mac[0] = (uint8_t)(words[0] >> 24);
	addr->mac[1] = (uint8_t)(words[0] >> 16);
	addr->mac[2] = (uint8_t)(words[1] >> 24);
	addr->mac[3] = (uint8_t)(words[1] >> 16);
	addr->mac[4] = (uint8_t)(words[1] >> 8);
	addr->mac[5] = (uint8_t)words[1];
	addr->port = (int)(words[2] & 0x7f);
	addr->static_entry = ((words[2] >> 29) & 1u) ? 1 : 0;
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
	uint32_t words[L2_ENTRY_WORDS];
	uint64_t key;
	int index;
	int probe;

	if (!mac || !out)
		return -EINVAL;
	key = l2_hash_key(mac, vid);
	index = (int)(key % L2_ENTRY_ENTRIES);
	if (index < 0)
		index += L2_ENTRY_ENTRIES;
	for (probe = 0; probe < 6; probe++) {
		int idx = (index + probe) % L2_ENTRY_ENTRIES;
		if (l2_table_read(unit, idx, words) != 0)
			continue;
		if ((words[0] & 1u) == 0)
			continue;
		/* Compare MAC and VID (same layout as pack). */
		if (((words[0] >> 4) & 0xfff) != (vid & 0xfff))
			continue;
		if ((uint8_t)(words[0] >> 24) != mac[0] || (uint8_t)(words[0] >> 16) != mac[1])
			continue;
		if ((uint8_t)(words[1] >> 24) != mac[2] || (uint8_t)(words[1] >> 16) != mac[3] ||
		    (uint8_t)(words[1] >> 8) != mac[4] || (uint8_t)words[1] != mac[5])
			continue;
		l2_unpack_entry(words, out);
		return 0;
	}
	return -ENOENT;
}

/* --- L2_USER_ENTRY (TCAM): 0x06168000, 512 entries × 20 bytes (5 words). RE: L2_ENTRY_FORMAT.md §2 --- */
#define L2_USER_ENTRY_BASE    0x06168000u
#define L2_USER_ENTRY_COUNT   512
#define L2_USER_ENTRY_WORDS   5

static int l2_user_table_write(int unit, int index, const uint32_t *words)
{
	uint32_t addr = L2_USER_ENTRY_BASE + (uint32_t)index * (L2_USER_ENTRY_WORDS * 4);
	return schan_write_memory(unit, addr, words, L2_USER_ENTRY_WORDS);
}

static int l2_user_table_read(int unit, int index, uint32_t *words)
{
	uint32_t addr = L2_USER_ENTRY_BASE + (uint32_t)index * (L2_USER_ENTRY_WORDS * 4);
	return schan_read_memory(unit, addr, words, L2_USER_ENTRY_WORDS);
}

/* Pack 5 words: KEY (VALID, MAC, VLAN, KEY_TYPE), MASK (61 bits), DATA (PRI, CPU, PORT_NUM, BPDU). */
static void l2_user_pack(const bcm56846_l2_user_addr_t *addr, uint32_t *words)
{
	uint64_t mac48 = (uint64_t)addr->mac[0] << 40 | (uint64_t)addr->mac[1] << 32 |
			 (uint64_t)addr->mac[2] << 24 | (uint64_t)addr->mac[3] << 16 |
			 (uint64_t)addr->mac[4] << 8 | addr->mac[5];
	uint64_t mask = addr->mask;
	uint16_t vid = addr->vid & 0xfff;
	int port = addr->port & 0x7f;

	/* word[0]: VALID=1, MAC[30:0] in bits[31:1] */
	words[0] = (1u) | ((uint32_t)(mac48 & 0x7fffffffu) << 1);
	/* word[1]: MAC[47:31] in bits[16:0], VLAN[28:17], KEY_TYPE@29, MASK[1:0]@31:30 */
	words[1] = (uint32_t)((mac48 >> 31) & 0x1ffffu) |
		  ((uint32_t)(vid & 0xfff) << 17) |
		  ((uint32_t)(addr->key_type & 1) << 29) |
		  ((uint32_t)(mask & 3u) << 30);
	/* word[2]: MASK[33:2] */
	words[2] = (uint32_t)((mask >> 2) & 0xffffffffu);
	/* word[3]: MASK[61:34], PRI=0, RPE=0. Entry mask: word[3] bit 31 invalid → use 0x7fffffff */
	words[3] = (uint32_t)((mask >> 34) & 0x7fffffffu);
	/* word[4]: RPE=0, CPU, DST_DISCARD=0, PORT_NUM[9:3], BPDU@26. Entry mask: bits 31:29 invalid → 0x1fffffff */
	words[4] = ((uint32_t)(addr->copy_to_cpu & 1) << 1) |
		   ((uint32_t)(port & 0x7f) << 3) |
		   ((uint32_t)(addr->bpdu & 1) << 26);
}

int bcm56846_l2_user_entry_add(int unit, const bcm56846_l2_user_addr_t *addr, int *index_out)
{
	uint32_t words[L2_USER_ENTRY_WORDS];
	int i;

	if (!addr)
		return -EINVAL;
	l2_user_pack(addr, words);
	/* Find first free slot (VALID=0). */
	for (i = 0; i < L2_USER_ENTRY_COUNT; i++) {
		uint32_t read_back[L2_USER_ENTRY_WORDS];
		if (l2_user_table_read(unit, i, read_back) != 0)
			continue;
		if ((read_back[0] & 1u) == 0) {
			if (l2_user_table_write(unit, i, words) != 0)
				return -EIO;
			if (index_out)
				*index_out = i;
			return 0;
		}
	}
	return -ENOSPC;
}

int bcm56846_l2_user_entry_delete(int unit, int index)
{
	uint32_t words[L2_USER_ENTRY_WORDS] = { 0, 0, 0, 0, 0 };
	if (index < 0 || index >= L2_USER_ENTRY_COUNT)
		return -EINVAL;
	return l2_user_table_write(unit, index, words);
}
