/* L3 intf + egress + route + host — S-Channel table programming (RE: L3_NEXTHOP_FORMAT, SWITCHD_L3_ROUTE_PROGRAMMING_ANALYSIS) */
#include "bcm56846.h"
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);

#define EGR_L3_INTF_BASE        0x01264000u
#define EGR_L3_INTF_WORDS       4
#define EGR_L3_INTF_STRIDE      16u

#define ING_L3_NEXT_HOP_BASE    0x0e17c000u
#define ING_L3_NEXT_HOP_WORDS   2
#define ING_L3_NEXT_HOP_STRIDE  8u

#define EGR_L3_NEXT_HOP_BASE    0x0c260000u
#define EGR_L3_NEXT_HOP_WORDS   4
#define EGR_L3_NEXT_HOP_STRIDE  16u

/* L3_DEFIP is TCAM-backed; we do a best-effort WRITE_MEMORY per RE. */
#define L3_DEFIP_BASE           0x0a170000u
#define L3_DEFIP_WORDS          8
#define L3_DEFIP_STRIDE         32u

#define MAX_L3_INTF             4096
#define MAX_L3_NHOP             16384
#define MAX_L3_DEFIP            8192

static int intf_used[MAX_L3_INTF];
static int nhop_used[MAX_L3_NHOP];
static int defip_used[MAX_L3_DEFIP];
static uint32_t defip_prefix[MAX_L3_DEFIP];
static int defip_plen[MAX_L3_DEFIP];

static int alloc_id(int *used, int max, int start)
{
	for (int i = start; i < max; i++) {
		if (!used[i]) {
			used[i] = 1;
			return i;
		}
	}
	return -1;
}

static void free_id(int *used, int max, int id)
{
	if (id > 0 && id < max)
		used[id] = 0;
}

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

static uint64_t mac48_to_u64(const uint8_t mac[6])
{
	return ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
	       ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
	       ((uint64_t)mac[4] << 8) | (uint64_t)mac[5];
}

static int egr_l3_intf_write(int unit, int intf_id, const uint32_t *words)
{
	uint32_t addr = EGR_L3_INTF_BASE + (uint32_t)intf_id * EGR_L3_INTF_STRIDE;
	return schan_write_memory(unit, addr, words, EGR_L3_INTF_WORDS);
}

static int ing_l3_nhop_write(int unit, int nhop_id, const uint32_t *words)
{
	uint32_t addr = ING_L3_NEXT_HOP_BASE + (uint32_t)nhop_id * ING_L3_NEXT_HOP_STRIDE;
	return schan_write_memory(unit, addr, words, ING_L3_NEXT_HOP_WORDS);
}

static int egr_l3_nhop_write(int unit, int nhop_id, const uint32_t *words)
{
	uint32_t addr = EGR_L3_NEXT_HOP_BASE + (uint32_t)nhop_id * EGR_L3_NEXT_HOP_STRIDE;
	return schan_write_memory(unit, addr, words, EGR_L3_NEXT_HOP_WORDS);
}

static int l3_defip_write(int unit, int index, const uint32_t *words)
{
	uint32_t addr = L3_DEFIP_BASE + (uint32_t)index * L3_DEFIP_STRIDE;
	return schan_write_memory(unit, addr, words, L3_DEFIP_WORDS);
}

int bcm56846_l3_intf_create(int unit, const uint8_t mac[6], uint16_t vid, int *intf_id)
{
	uint32_t w[EGR_L3_INTF_WORDS];
	int id;

	if (!mac || !intf_id)
		return -EINVAL;
	id = alloc_id(intf_used, MAX_L3_INTF, 1);
	if (id < 0)
		return -ENOSPC;

	memset(w, 0, sizeof(w));
	/* EGR_L3_INTF: VID bits [24:13], MAC_ADDRESS bits [80:33] */
	set_bits_u64(w, EGR_L3_INTF_WORDS, 13, 12, (uint64_t)(vid & 0xfff));
	set_bits_u64(w, EGR_L3_INTF_WORDS, 33, 48, mac48_to_u64(mac));

	if (egr_l3_intf_write(unit, id, w) != 0) {
		free_id(intf_used, MAX_L3_INTF, id);
		return -EIO;
	}

	*intf_id = id;
	return 0;
}

int bcm56846_l3_intf_destroy(int unit, int intf_id)
{
	uint32_t w[EGR_L3_INTF_WORDS] = { 0, 0, 0, 0 };
	if (intf_id <= 0 || intf_id >= MAX_L3_INTF)
		return -EINVAL;
	(void) egr_l3_intf_write(unit, intf_id, w);
	free_id(intf_used, MAX_L3_INTF, intf_id);
	return 0;
}

int bcm56846_l3_egress_create(int unit, const bcm56846_l3_egress_t *egress, int *egress_id)
{
	uint32_t ingw[ING_L3_NEXT_HOP_WORDS];
	uint32_t egrw[EGR_L3_NEXT_HOP_WORDS];
	int id;

	if (!egress || !egress_id)
		return -EINVAL;
	if (egress->port <= 0 || egress->port > 255)
		return -EINVAL;
	if (egress->intf_id <= 0 || egress->intf_id >= MAX_L3_INTF)
		return -EINVAL;

	id = alloc_id(nhop_used, MAX_L3_NHOP, 1);
	if (id < 0)
		return -ENOSPC;

	memset(ingw, 0, sizeof(ingw));
	/* ING_L3_NEXT_HOP: ENTRY_TYPE[1:0]=0, PORT_NUM[22:16], MODULE_ID[30:23]=0, T[31]=0 */
	set_bits_u64(ingw, ING_L3_NEXT_HOP_WORDS, 0, 2, 0);
	set_bits_u64(ingw, ING_L3_NEXT_HOP_WORDS, 16, 7, (uint64_t)(egress->port & 0x7f));

	memset(egrw, 0, sizeof(egrw));
	/* EGR_L3_NEXT_HOP (L3 unicast view): ENTRY_TYPE[1:0]=0, INTF_NUM[14:3], L3:MAC_ADDRESS[62:15] */
	set_bits_u64(egrw, EGR_L3_NEXT_HOP_WORDS, 0, 2, 0);
	set_bits_u64(egrw, EGR_L3_NEXT_HOP_WORDS, 3, 12, (uint64_t)(egress->intf_id & 0xfff));
	set_bits_u64(egrw, EGR_L3_NEXT_HOP_WORDS, 15, 48, mac48_to_u64(egress->mac));

	if (ing_l3_nhop_write(unit, id, ingw) != 0 || egr_l3_nhop_write(unit, id, egrw) != 0) {
		free_id(nhop_used, MAX_L3_NHOP, id);
		return -EIO;
	}

	*egress_id = id;
	return 0;
}

int bcm56846_l3_egress_destroy(int unit, int egress_id)
{
	uint32_t ingw[ING_L3_NEXT_HOP_WORDS] = { 0, 0 };
	uint32_t egrw[EGR_L3_NEXT_HOP_WORDS] = { 0, 0, 0, 0 };
	if (egress_id <= 0 || egress_id >= MAX_L3_NHOP)
		return -EINVAL;
	(void) ing_l3_nhop_write(unit, egress_id, ingw);
	(void) egr_l3_nhop_write(unit, egress_id, egrw);
	free_id(nhop_used, MAX_L3_NHOP, egress_id);
	return 0;
}

static int defip_find(uint32_t prefix, int plen)
{
	for (int i = 0; i < MAX_L3_DEFIP; i++) {
		if (defip_used[i] && defip_prefix[i] == prefix && defip_plen[i] == plen)
			return i;
	}
	return -1;
}

static void defip_pack_v4_ucast(uint32_t *w, uint32_t prefix_host, int plen, int nhop_index)
{
	uint32_t ip_mask = (plen <= 0) ? 0 : (uint32_t)(0xffffffffu << (32 - plen));
	uint64_t key = ((uint64_t)0u << 33) | ((uint64_t)prefix_host << 1) | 0u;
	uint64_t mask = ((uint64_t)0x3ffu << 33) | ((uint64_t)ip_mask << 1) | 1u;

	memset(w, 0, sizeof(uint32_t) * L3_DEFIP_WORDS);
	/* VALID0 */
	set_bit(w, L3_DEFIP_WORDS, 0, 1);
	/* MODE0 is bit2 of KEY0; already 0. KEY0[45:2] */
	set_bits_u64(w, L3_DEFIP_WORDS, 2, 44, key);
	/* MASK0[133:90] */
	set_bits_u64(w, L3_DEFIP_WORDS, 90, 44, mask);
	/* ECMP0[206]=0, NEXT_HOP_INDEX0[220:207] */
	set_bits_u64(w, L3_DEFIP_WORDS, 207, 14, (uint64_t)(nhop_index & 0x3fff));
}

int bcm56846_l3_route_add(int unit, const bcm56846_l3_route_t *route)
{
	uint32_t w[L3_DEFIP_WORDS];
	uint32_t prefix_host;
	int idx;

	if (!route)
		return -EINVAL;
	if (route->is_ipv6)
		return -ENOTSUP;
	if (route->prefix_len < 0 || route->prefix_len > 32)
		return -EINVAL;
	if (route->egress_id <= 0 || route->egress_id >= MAX_L3_NHOP)
		return -EINVAL;

	prefix_host = ntohl(route->prefix);
	idx = defip_find(prefix_host, route->prefix_len);
	if (idx < 0) {
		idx = alloc_id(defip_used, MAX_L3_DEFIP, 0);
		if (idx < 0)
			return -ENOSPC;
		defip_prefix[idx] = prefix_host;
		defip_plen[idx] = route->prefix_len;
	}

	defip_pack_v4_ucast(w, prefix_host, route->prefix_len, route->egress_id);
	if (l3_defip_write(unit, idx, w) != 0)
		return -EIO;
	return 0;
}

int bcm56846_l3_route_delete(int unit, const bcm56846_l3_route_t *route)
{
	uint32_t w[L3_DEFIP_WORDS] = { 0 };
	uint32_t prefix_host;
	int idx;

	if (!route)
		return -EINVAL;
	if (route->is_ipv6)
		return -ENOTSUP;
	if (route->prefix_len < 0 || route->prefix_len > 32)
		return -EINVAL;
	prefix_host = ntohl(route->prefix);
	idx = defip_find(prefix_host, route->prefix_len);
	if (idx < 0)
		return 0;
	(void) l3_defip_write(unit, idx, w);
	defip_used[idx] = 0;
	defip_prefix[idx] = 0;
	defip_plen[idx] = 0;
	return 0;
}

int bcm56846_l3_host_add(int unit, const bcm56846_l3_host_t *host)
{
	bcm56846_l3_route_t r;

	if (!host)
		return -EINVAL;
	if (host->is_ipv6)
		return -ENOTSUP;
	memset(&r, 0, sizeof(r));
	r.prefix = htonl(host->addr[0]);
	r.prefix_len = 32;
	r.egress_id = host->egress_id;
	r.is_ipv6 = 0;
	return bcm56846_l3_route_add(unit, &r);
}
