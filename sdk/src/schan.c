/* S-Channel write/read — BDE ioctl NOS_BDE_SCHAN_OP */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include <errno.h>
#include <string.h>

/*
 * BCM56846 CMICm SCHAN format: addr IS the full SCHAN command word.
 *   bits[31:26] = opcode (0x0A=READ_REGISTER, 0x0B=WRITE_REGISTER, etc.)
 *   bits[25:16] = SBUS destination (6-bit agent 0-63; agent 3 = TOP block)
 *   bits[15:0]  = register/memory address within the SBUS block
 *
 * addr must go in cmd[0] (MSG[0]) so the CMIC can read bits[25:16] for routing.
 * Previous bug: 0x28000000|n in MSG[0] had bits[25:16]=0x000 (agent 0, wrong)
 * and 0x2a000000|n had bits[25:16]=0x200 (agent 512, invalid) → ctrl=0x77 for all ops.
 */
int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words)
{
	uint32_t cmd[1];
	int status;
	(void)unit;
	if (num_words <= 0 || num_words > 16)
		return -1;
	cmd[0] = addr;
	if (bde_schan_op(cmd, 1, (uint32_t *)data, num_words, &status) < 0)
		return -1;
	return status == 0 ? 0 : -1;
}

int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words)
{
	uint32_t cmd[1];
	int status;
	(void)unit;
	if (!data || num_words <= 0 || num_words > 16)
		return -1;
	memset(data, 0, sizeof(uint32_t) * (size_t)num_words);
	cmd[0] = addr;
	if (bde_schan_op(cmd, 1, data, num_words, &status) < 0)
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
	return schan_read_memory(unit, addr, data, len);
}
