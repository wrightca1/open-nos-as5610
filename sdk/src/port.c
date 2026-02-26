/* Port enable/speed/link — stubs until Phase 2c */
#include "bcm56846.h"
#include <errno.h>

int bcm56846_port_enable_set(int unit, int port, int enable)
{
	(void)unit;
	(void)port;
	(void)enable;
	return 0;
}

int bcm56846_port_speed_set(int unit, int port, int speed_mbps)
{
	(void)unit;
	(void)port;
	(void)speed_mbps;
	return 0;
}

int bcm56846_port_link_status_get(int unit, int port, int *link_up)
{
	(void)unit;
	(void)port;
	if (link_up)
		*link_up = 0;
	return 0;
}
