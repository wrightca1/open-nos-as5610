/* L3 intf + egress + route + host — stubs until Phase 2e */
#include "bcm56846.h"
#include <errno.h>

int bcm56846_l3_intf_create(int unit, const uint8_t mac[6], uint16_t vid, int *intf_id)
{
	(void)unit;
	(void)mac;
	(void)vid;
	if (intf_id)
		*intf_id = 0;
	return 0;
}

int bcm56846_l3_intf_destroy(int unit, int intf_id)
{
	(void)unit;
	(void)intf_id;
	return 0;
}

int bcm56846_l3_egress_create(int unit, const bcm56846_l3_egress_t *egress, int *egress_id)
{
	(void)unit;
	(void)egress;
	if (egress_id)
		*egress_id = 0;
	return 0;
}

int bcm56846_l3_egress_destroy(int unit, int egress_id)
{
	(void)unit;
	(void)egress_id;
	return 0;
}

int bcm56846_l3_route_add(int unit, const bcm56846_l3_route_t *route)
{
	(void)unit;
	(void)route;
	return 0;
}

int bcm56846_l3_route_delete(int unit, const bcm56846_l3_route_t *route)
{
	(void)unit;
	(void)route;
	return 0;
}

int bcm56846_l3_host_add(int unit, const bcm56846_l3_host_t *host)
{
	(void)unit;
	(void)host;
	return 0;
}
