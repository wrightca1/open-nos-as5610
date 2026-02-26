/* ECMP — S-Channel table programming (RE: L3_NEXTHOP_FORMAT.md) */
#include "bcm56846.h"
#include <errno.h>
#include <string.h>

extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);

#define L3_ECMP_BASE         0x0e176000u
#define L3_ECMP_WORDS        1
#define L3_ECMP_STRIDE       4u
#define L3_ECMP_ENTRIES      4096

#define L3_ECMP_GROUP_BASE   0x0e174000u
#define L3_ECMP_GROUP_WORDS  7
#define L3_ECMP_GROUP_STRIDE 32u
#define L3_ECMP_GROUP_ENTRIES 1024

static int ecmp_used[L3_ECMP_ENTRIES];
static int ecmp_group_used[L3_ECMP_GROUP_ENTRIES];
static int next_group_id = 1;

static void set_bit(uint32_t *words, int num_words, int bit, int value)
{
	int wi = bit / 32;
	int bi = bit % 32;
	if (wi < 0 || wi >= num_words)
		return;
	if (value)
		words[wi] |= (1u << bi);
	else
		words[wi] &= ~(1u << bi);
}

static void set_bits_u64(uint32_t *words, int num_words, int start_bit, int width, uint64_t value)
{
	for (int i = 0; i < width; i++) {
		int v = (value >> i) & 1u;
		set_bit(words, num_words, start_bit + i, v);
	}
}

static int l3_ecmp_write(int unit, int index, uint32_t word)
{
	uint32_t addr = L3_ECMP_BASE + (uint32_t)index * L3_ECMP_STRIDE;
	return schan_write_memory(unit, addr, &word, L3_ECMP_WORDS);
}

static int l3_ecmp_group_write(int unit, int group_id, const uint32_t *words)
{
	uint32_t addr = L3_ECMP_GROUP_BASE + (uint32_t)group_id * L3_ECMP_GROUP_STRIDE;
	return schan_write_memory(unit, addr, words, L3_ECMP_GROUP_WORDS);
}

static int alloc_group_id(void)
{
	for (int i = next_group_id; i < L3_ECMP_GROUP_ENTRIES; i++) {
		if (!ecmp_group_used[i]) {
			ecmp_group_used[i] = 1;
			next_group_id = i + 1;
			return i;
		}
	}
	for (int i = 1; i < next_group_id; i++) {
		if (!ecmp_group_used[i]) {
			ecmp_group_used[i] = 1;
			next_group_id = i + 1;
			return i;
		}
	}
	return -1;
}

static void free_group_id(int id)
{
	if (id > 0 && id < L3_ECMP_GROUP_ENTRIES)
		ecmp_group_used[id] = 0;
}

static int alloc_ecmp_slots(int count)
{
	/* First-fit contiguous allocator over 4096 entries */
	if (count <= 0 || count > L3_ECMP_ENTRIES)
		return -1;
	for (int base = 0; base + count <= L3_ECMP_ENTRIES; base++) {
		int ok = 1;
		for (int i = 0; i < count; i++) {
			if (ecmp_used[base + i]) {
				ok = 0;
				base += i; /* skip ahead */
				break;
			}
		}
		if (!ok)
			continue;
		for (int i = 0; i < count; i++)
			ecmp_used[base + i] = 1;
		return base;
	}
	return -1;
}

static void free_ecmp_slots(int base, int count)
{
	if (base < 0 || count <= 0)
		return;
	if (base + count > L3_ECMP_ENTRIES)
		return;
	for (int i = 0; i < count; i++)
		ecmp_used[base + i] = 0;
}

int bcm56846_l3_ecmp_create(int unit, const int *egress_ids, int count, int *ecmp_id)
{
	uint32_t group_words[L3_ECMP_GROUP_WORDS];
	int group, base_ptr;

	if (!egress_ids || count <= 0 || !ecmp_id)
		return -EINVAL;
	if (count > 1023)
		return -EINVAL;

	group = alloc_group_id();
	if (group < 0)
		return -ENOSPC;
	base_ptr = alloc_ecmp_slots(count);
	if (base_ptr < 0) {
		free_group_id(group);
		return -ENOSPC;
	}

	/* Write member table entries */
	for (int i = 0; i < count; i++) {
		uint32_t w = (uint32_t)(egress_ids[i] & 0x3fff);
		if (l3_ecmp_write(unit, base_ptr + i, w) != 0) {
			free_ecmp_slots(base_ptr, count);
			free_group_id(group);
			return -EIO;
		}
	}

	memset(group_words, 0, sizeof(group_words));
	/* BASE_PTR[21:10], COUNT[9:0] */
	set_bits_u64(group_words, L3_ECMP_GROUP_WORDS, 10, 12, (uint64_t)(base_ptr & 0xfff));
	set_bits_u64(group_words, L3_ECMP_GROUP_WORDS, 0, 10, (uint64_t)(count & 0x3ff));

	/* ECMP_GT8 at bit 196 */
	set_bit(group_words, L3_ECMP_GROUP_WORDS, 196, (count > 8) ? 1 : 0);

	/* Precomputed fast-path OIFs when count <= 8 */
	if (count <= 8) {
		static const int oif_bits[8] = { 82, 96, 110, 124, 138, 152, 166, 180 };
		static const int type_bits[8] = { 81, 95, 109, 123, 137, 151, 165, 179 };
		for (int i = 0; i < count; i++) {
			set_bits_u64(group_words, L3_ECMP_GROUP_WORDS, oif_bits[i], 13, (uint64_t)(egress_ids[i] & 0x1fff));
			set_bit(group_words, L3_ECMP_GROUP_WORDS, type_bits[i], 0);
		}
	}

	if (l3_ecmp_group_write(unit, group, group_words) != 0) {
		free_ecmp_slots(base_ptr, count);
		free_group_id(group);
		return -EIO;
	}

	*ecmp_id = group;
	return 0;
}

int bcm56846_l3_ecmp_destroy(int unit, int ecmp_id)
{
	(void)unit;
	/* Best-effort: clear group descriptor; member slots are leaked without stored BASE_PTR/COUNT. */
	if (ecmp_id <= 0 || ecmp_id >= L3_ECMP_GROUP_ENTRIES)
		return -EINVAL;
	{
		uint32_t zero[L3_ECMP_GROUP_WORDS] = { 0 };
		(void) l3_ecmp_group_write(unit, ecmp_id, zero);
	}
	free_group_id(ecmp_id);
	return 0;
}
