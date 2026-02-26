/* Common types for libbcm56846 API */
#ifndef BCM56846_TYPES_H
#define BCM56846_TYPES_H

#include <stdint.h>

typedef void (*bcm56846_rx_cb_t)(int unit, int port, const void *pkt, int len, void *cookie);

typedef struct {
	uint8_t  mac[6];
	uint16_t vid;
	int       port;
	int       static_entry;
} bcm56846_l2_addr_t;

typedef struct {
	uint8_t  mac[6];
	uint16_t vid;
	int      port;
	int      intf_id;
} bcm56846_l3_egress_t;

typedef struct {
	uint32_t prefix;   /* IPv4 or low 32 bits */
	uint32_t prefix6[4]; /* IPv6 full */
	int      prefix_len;
	int      egress_id;
	int      is_ipv6;
} bcm56846_l3_route_t;

typedef struct {
	uint32_t addr[4];  /* IPv4 or IPv6 */
	int      is_ipv6;
	int      egress_id;
} bcm56846_l3_host_t;

#endif
