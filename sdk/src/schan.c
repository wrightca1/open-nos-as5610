/* S-Channel write/read — BDE ioctl NOS_BDE_SCHAN_OP */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include <errno.h>
#include <string.h>

/* WRITE_MEMORY: Word 0 = 0x28 + word_count, Word 1 = table address (RE: SWITCHD_L3_ROUTE_PROGRAMMING_ANALYSIS) */
int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words)
{
	uint32_t cmd[8];
	int status;
	(void)unit;
	if (num_words <= 0 || num_words > 16)
		return -1;
	cmd[0] = 0x28000000 | (num_words & 0xff);
	cmd[1] = addr;
	if (bde_schan_op(cmd, 2, (uint32_t *)data, num_words, &status) < 0)
		return -1;
	return status == 0 ? 0 : -1;
}

/* READ_MEMORY: best-effort based on observed WRITE_MEMORY opcode family. */
int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words)
{
	uint32_t cmd[8];
	int status;
	(void)unit;
	if (!data || num_words <= 0 || num_words > 16)
		return -1;
	memset(data, 0, sizeof(uint32_t) * (size_t)num_words);
	cmd[0] = 0x2a000000 | (num_words & 0xff);
	cmd[1] = addr;
	if (bde_schan_op(cmd, 2, data, num_words, &status) < 0)
		return -1;
	return status == 0 ? 0 : -1;
}

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
	(void)addr;
	return schan_read_memory(unit, addr, data, len);
}
