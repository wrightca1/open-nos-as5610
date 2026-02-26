/* Register read/write via BDE ioctl */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"

int bcm56846_reg_read32(int unit, uint32_t offset, uint32_t *value)
{
	(void)unit;
	return bde_read_reg(offset, value);
}

int bcm56846_reg_write32(int unit, uint32_t offset, uint32_t value)
{
	(void)unit;
	return bde_write_reg(offset, value);
}
