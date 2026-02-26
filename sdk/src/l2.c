/* L2 table — stubs until Phase 2d */
#include "bcm56846.h"
#include <errno.h>

int bcm56846_l2_addr_add(int unit, const bcm56846_l2_addr_t *addr)
{
	(void)unit;
	(void)addr;
	return 0;
}

int bcm56846_l2_addr_delete(int unit, const uint8_t mac[6], uint16_t vid)
{
	(void)unit;
	(void)mac;
	(void)vid;
	return 0;
}

int bcm56846_l2_addr_get(int unit, const uint8_t mac[6], uint16_t vid, bcm56846_l2_addr_t *out)
{
	(void)unit;
	(void)mac;
	(void)vid;
	(void)out;
	return -ENOSYS;
}
