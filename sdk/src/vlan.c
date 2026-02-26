/* VLAN — stubs until Phase 2g */
#include "bcm56846.h"
#include <errno.h>

int bcm56846_vlan_create(int unit, uint16_t vid)
{
	(void)unit;
	(void)vid;
	return 0;
}

int bcm56846_vlan_port_add(int unit, uint16_t vid, int port, int tagged)
{
	(void)unit;
	(void)vid;
	(void)port;
	(void)tagged;
	return 0;
}

int bcm56846_vlan_destroy(int unit, uint16_t vid)
{
	(void)unit;
	(void)vid;
	return 0;
}
