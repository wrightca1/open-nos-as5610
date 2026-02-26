/* VLAN — S-Channel table programming (RE: VLAN_TABLE_FORMAT.md) */
#include "bcm56846.h"
#include <errno.h>
#include <string.h>

extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);

#define VLAN_BASE        0x12168000u
#define VLAN_WORDS       10
#define VLAN_STRIDE      40u

#define EGR_VLAN_BASE    0x0d260000u
#define EGR_VLAN_WORDS   8
#define EGR_VLAN_STRIDE  32u

static uint32_t vlan_ing[4096][VLAN_WORDS];
static uint32_t vlan_egr[4096][EGR_VLAN_WORDS];
static int vlan_valid[4096];
static uint64_t vlan_ing_pbm[4096];
static uint64_t vlan_egr_pbm[4096];
static uint64_t vlan_egr_utbm[4096];

static void vlan_set_port_bitmap(uint32_t words[VLAN_WORDS], uint64_t bm)
{
	/* PORT_BITMAP bits [65:0] => w0,w1,w2[1:0] */
	words[0] = (uint32_t)(bm & 0xffffffffu);
	words[1] = (uint32_t)((bm >> 32) & 0xffffffffu);
	/* bits[65:64] are unused for AS5610; keep 0 */
	words[2] &= ~0x3u;
}

static void vlan_set_ing_port_bitmap(uint32_t words[VLAN_WORDS], uint64_t bm)
{
	/* ING_PORT_BITMAP bits [131:66] => w2[31:2], w3, w4[1:0] */
	uint32_t w2 = words[2] & 0x3u; /* preserve PORT_BITMAP high bits */
	w2 |= (uint32_t)((bm & 0x3fffffffu) << 2);
	words[2] = w2;
	words[3] = (uint32_t)((bm >> 30) & 0xffffffffu);
	words[4] = (words[4] & ~0x3u) | (uint32_t)((bm >> 62) & 0x3u);
}

static void vlan_set_stg_valid_profile(uint32_t words[VLAN_WORDS], int stg, int valid, int profile_ptr)
{
	/* STG w4[12:4], VALID bit205 = w6[13], VLAN_PROFILE_PTR w7[14:8] */
	words[4] &= ~(0x1ffu << 4);
	words[4] |= ((uint32_t)(stg & 0x1ff) << 4);
	if (valid)
		words[6] |= (1u << 13);
	else
		words[6] &= ~(1u << 13);
	words[7] &= ~(0x7fu << 8);
	words[7] |= ((uint32_t)(profile_ptr & 0x7f) << 8);
}

static void egr_vlan_set_ut_port_bitmap(uint32_t words[EGR_VLAN_WORDS], uint64_t bm)
{
	/* UT_PORT_BITMAP bits [161:96] => w3,w4,w5[1:0] */
	words[3] = (uint32_t)(bm & 0xffffffffu);
	words[4] = (uint32_t)((bm >> 32) & 0xffffffffu);
	/* bits[65:64] are unused for AS5610; keep 0 */
	words[5] &= ~0x3u;
}

static void egr_vlan_set_port_bitmap(uint32_t words[EGR_VLAN_WORDS], uint64_t bm)
{
	/* PORT_BITMAP bits [227:162] => w5[31:2], w6, w7[3:0] */
	words[5] &= 0x3u; /* preserve UT bits */
	words[5] |= (uint32_t)((bm & 0x3fffffffu) << 2);
	words[6] = (uint32_t)((bm >> 30) & 0xffffffffu);
	words[7] = (words[7] & ~0xfu) | (uint32_t)((bm >> 62) & 0xfu);
}

static void egr_vlan_set_valid_stg(uint32_t words[EGR_VLAN_WORDS], int stg, int valid)
{
	/* VALID bit0, STG bits [9:1] */
	if (valid)
		words[0] |= 1u;
	else
		words[0] &= ~1u;
	words[0] &= ~(0x1ffu << 1);
	words[0] |= ((uint32_t)(stg & 0x1ff) << 1);
}

static int vlan_write_ing(int unit, uint16_t vid)
{
	uint32_t addr = VLAN_BASE + (uint32_t)vid * VLAN_STRIDE;
	return schan_write_memory(unit, addr, vlan_ing[vid], VLAN_WORDS);
}

static int vlan_write_egr(int unit, uint16_t vid)
{
	uint32_t addr = EGR_VLAN_BASE + (uint32_t)vid * EGR_VLAN_STRIDE;
	return schan_write_memory(unit, addr, vlan_egr[vid], EGR_VLAN_WORDS);
}

int bcm56846_vlan_create(int unit, uint16_t vid)
{
	if (vid > 4095)
		return -EINVAL;
	memset(vlan_ing[vid], 0, sizeof(vlan_ing[vid]));
	memset(vlan_egr[vid], 0, sizeof(vlan_egr[vid]));
	vlan_ing_pbm[vid] = 0;
	vlan_egr_pbm[vid] = 0;
	vlan_egr_utbm[vid] = 0;

	/* Default: VALID=1, STG=2, VLAN_PROFILE_PTR=0, no members yet. */
	vlan_set_port_bitmap(vlan_ing[vid], vlan_ing_pbm[vid]);
	vlan_set_ing_port_bitmap(vlan_ing[vid], vlan_ing_pbm[vid]);
	vlan_set_stg_valid_profile(vlan_ing[vid], 2, 1, 0);

	egr_vlan_set_valid_stg(vlan_egr[vid], 2, 1);
	egr_vlan_set_ut_port_bitmap(vlan_egr[vid], vlan_egr_utbm[vid]);
	egr_vlan_set_port_bitmap(vlan_egr[vid], vlan_egr_pbm[vid]);

	vlan_valid[vid] = 1;
	if (vlan_write_ing(unit, vid) != 0 || vlan_write_egr(unit, vid) != 0)
		return -EIO;
	return 0;
}

int bcm56846_vlan_port_add(int unit, uint16_t vid, int port, int tagged)
{
	int bit;

	if (vid > 4095 || port <= 0 || port > 52)
		return -EINVAL;
	if (!vlan_valid[vid]) {
		int rc = bcm56846_vlan_create(unit, vid);
		if (rc != 0)
			return rc;
	}

	/* Port bitmap encoding: bit0=CPU, bit1=xe0(swp1) ... bit52=xe51(swp52) */
	bit = port; /* swpN => xe(N-1) => bit N */

	/* Ingress: add to both PORT_BITMAP and ING_PORT_BITMAP */
	vlan_ing_pbm[vid] |= (uint64_t)1u << bit;
	vlan_set_port_bitmap(vlan_ing[vid], vlan_ing_pbm[vid]);
	vlan_set_ing_port_bitmap(vlan_ing[vid], vlan_ing_pbm[vid]);

	/* Egress: add to PORT_BITMAP; if untagged, add to UT_PORT_BITMAP */
	if (!tagged)
		vlan_egr_utbm[vid] |= (uint64_t)1u << bit;
	vlan_egr_pbm[vid] |= (uint64_t)1u << bit;

	egr_vlan_set_ut_port_bitmap(vlan_egr[vid], vlan_egr_utbm[vid]);
	egr_vlan_set_port_bitmap(vlan_egr[vid], vlan_egr_pbm[vid]);

	if (vlan_write_ing(unit, vid) != 0 || vlan_write_egr(unit, vid) != 0)
		return -EIO;
	return 0;
}

int bcm56846_vlan_destroy(int unit, uint16_t vid)
{
	if (vid > 4095)
		return -EINVAL;
	memset(vlan_ing[vid], 0, sizeof(vlan_ing[vid]));
	memset(vlan_egr[vid], 0, sizeof(vlan_egr[vid]));
	vlan_ing_pbm[vid] = 0;
	vlan_egr_pbm[vid] = 0;
	vlan_egr_utbm[vid] = 0;
	vlan_valid[vid] = 0;
	(void) vlan_write_ing(unit, vid);
	(void) vlan_write_egr(unit, vid);
	return 0;
}
