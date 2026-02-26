/* libbcm56846 — Public API for BCM56846 (Trident+) */
#ifndef BCM56846_H
#define BCM56846_H

#include "bcm56846_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init */
int bcm56846_attach(int unit);
int bcm56846_init(int unit, const char *config_path);
void bcm56846_detach(int unit);

/* Port */
int bcm56846_port_enable_set(int unit, int port, int enable);
int bcm56846_port_speed_set(int unit, int port, int speed_mbps);
int bcm56846_port_link_status_get(int unit, int port, int *link_up);

/* L2 */
int bcm56846_l2_addr_add(int unit, const bcm56846_l2_addr_t *addr);
int bcm56846_l2_addr_delete(int unit, const uint8_t mac[6], uint16_t vid);
int bcm56846_l2_addr_get(int unit, const uint8_t mac[6], uint16_t vid, bcm56846_l2_addr_t *out);

/* L3 Interface (EGR_L3_INTF: SA_MAC + VLAN per interface) */
int bcm56846_l3_intf_create(int unit, const uint8_t mac[6], uint16_t vid, int *intf_id);
int bcm56846_l3_intf_destroy(int unit, int intf_id);

/* L3 Egress */
int bcm56846_l3_egress_create(int unit, const bcm56846_l3_egress_t *egress, int *egress_id);
int bcm56846_l3_egress_destroy(int unit, int egress_id);

/* L3 Routes */
int bcm56846_l3_route_add(int unit, const bcm56846_l3_route_t *route);
int bcm56846_l3_route_delete(int unit, const bcm56846_l3_route_t *route);
int bcm56846_l3_host_add(int unit, const bcm56846_l3_host_t *host);

/* ECMP */
int bcm56846_l3_ecmp_create(int unit, const int *egress_ids, int count, int *ecmp_id);
int bcm56846_l3_ecmp_destroy(int unit, int ecmp_id);

/* VLAN */
int bcm56846_vlan_create(int unit, uint16_t vid);
int bcm56846_vlan_port_add(int unit, uint16_t vid, int port, int tagged);
int bcm56846_vlan_destroy(int unit, uint16_t vid);

/* Stats (XLMAC counters; RE: STATS_COUNTER_FORMAT.md) */
int bcm56846_stat_get(int unit, int port, bcm56846_stat_t stat, uint64_t *value);

/* Packet I/O */
int bcm56846_tx(int unit, int port, const void *pkt, int len);
int bcm56846_rx_register(int unit, bcm56846_rx_cb_t cb, void *cookie);
int bcm56846_rx_start(int unit);
int bcm56846_rx_stop(int unit);

#ifdef __cplusplus
}
#endif

#endif
