/* ECMP — stubs until Phase 2e */
#include "bcm56846.h"
#include <errno.h>

int bcm56846_l3_ecmp_create(int unit, const int *egress_ids, int count, int *ecmp_id)
{
	(void)unit;
	(void)egress_ids;
	(void)count;
	if (ecmp_id)
		*ecmp_id = 0;
	return 0;
}

int bcm56846_l3_ecmp_destroy(int unit, int ecmp_id)
{
	(void)unit;
	(void)ecmp_id;
	return 0;
}
