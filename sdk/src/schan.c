/* S-Channel write/read — BDE ioctl NOS_BDE_SCHAN_OP */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include <errno.h>
#include <string.h>

/* S-Channel command format: 0x2800XXXX (RE: SCHAN_FORMAT_ANALYSIS) */
int schan_write(int unit, uint32_t cmd_word, const uint32_t *data, int len)
{
	uint32_t cmd[8] = { cmd_word };
	int status;
	(void)unit;
	if (len > 16)
		len = 16;
	if (bde_schan_op(cmd, 1, (uint32_t *)data, len, &status) < 0)
		return -1;
	return status == 0 ? 0 : -1;
}

int schan_read(int unit, uint32_t addr, uint32_t *data, int len)
{
	uint32_t cmd[8];
	int status;
	(void)unit;
	if (len > 16)
		len = 16;
	/* Read opcode in cmd_word; format TBD from RE */
	cmd[0] = 0x28000000 | (len << 8) | 0x01; /* placeholder */
	if (bde_schan_op(cmd, 1, data, len, &status) < 0)
		return -1;
	return status == 0 ? 0 : -1;
}
