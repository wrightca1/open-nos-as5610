/* Packet I/O — stubs until Phase 2f */
#include "bcm56846.h"
#include <errno.h>

int bcm56846_tx(int unit, int port, const void *pkt, int len)
{
	(void)unit;
	(void)port;
	(void)pkt;
	(void)len;
	return 0;
}

int bcm56846_rx_register(int unit, bcm56846_rx_cb_t cb, void *cookie)
{
	(void)unit;
	(void)cb;
	(void)cookie;
	return 0;
}

int bcm56846_rx_start(int unit)
{
	(void)unit;
	return 0;
}

int bcm56846_rx_stop(int unit)
{
	(void)unit;
	return 0;
}
