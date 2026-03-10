/*
 * SBUS register/memory access — proper CDK-format SCHAN messaging.
 *
 * The BCM56846 CMICe SCHAN message format (XGS):
 *   D[0] = header: (opcode << 26) | (dstblk << 20) | (srcblk << 14) | (datalen << 7)
 *   D[1] = address: CDK register/memory address
 *   D[2+] = data words (for writes)
 *
 * Register addresses come from OpenMDK CDK (bcm56840_a0_defs.h).
 * Block number extracted from address: ((addr >> 20) & 0xf) | ((addr >> 26) & 0x30)
 *
 * Per-port registers: addr = (block << 20) | (port << 12) | (offset & ~0xF00000)
 */
#include "bde_ioctl.h"
#include <stdio.h>
#include <string.h>

/* SCHAN opcodes (XGS) */
#define SCHAN_READ_REG_CMD   0x0Bu
#define SCHAN_WRITE_REG_CMD  0x0Du
#define SCHAN_READ_MEM_CMD   0x07u
#define SCHAN_WRITE_MEM_CMD  0x09u

/* Build SCHAN header word.
 * dwords: data word count (number of 32-bit data words, NOT byte count).
 * CDK field name: V2_SCHAN_MSG_DWC at bits [13:7]. */
static inline uint32_t schan_header(uint32_t opcode, uint32_t dstblk,
				    uint32_t dwords)
{
	return (opcode << 26) | (dstblk << 20) | (0u << 14) | (dwords << 7);
}

/* Extract SBUS block number from CDK address */
static inline uint32_t cdk_addr_to_block(uint32_t addr)
{
	return ((addr >> 20) & 0xfu) | ((addr >> 26) & 0x30u);
}

/*
 * Compute per-port register address from CDK base address.
 * CDK formula: (block * 0x100000) | (port * 0x1000) | (offset & ~0xf00000)
 * Must preserve upper address bits (above bit 23) for SBUS sub-block routing.
 */
uint32_t cdk_port_addr(uint32_t base, int port)
{
	uint32_t block = cdk_addr_to_block(base);
	return (base & ~0xF00000u) | (block << 20) | ((uint32_t)port << 12);
}

/*
 * sbus_reg_write: Write a 32-bit SOC register via SCHAN WRITE_REGISTER.
 *   addr: CDK register address (e.g. 0x0238010a for BUFFER_CELL_LIMIT_SPr)
 *   value: 32-bit value to write
 */
int sbus_reg_write(uint32_t addr, uint32_t value)
{
	uint32_t cmd[3];
	int status = -1;

	cmd[0] = schan_header(SCHAN_WRITE_REG_CMD, cdk_addr_to_block(addr), 1);
	cmd[1] = addr;
	cmd[2] = value;

	if (bde_schan_op(cmd, 3, NULL, 0, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] reg_write FAIL addr=0x%08x val=0x%08x status=%d\n",
			addr, value, status);
		return -1;
	}
	return 0;
}

/*
 * sbus_reg_read: Read a 32-bit SOC register via SCHAN READ_REGISTER.
 */
int sbus_reg_read(uint32_t addr, uint32_t *value)
{
	uint32_t cmd[2];
	uint32_t resp[2] = {0, 0};
	int status = -1;

	cmd[0] = schan_header(SCHAN_READ_REG_CMD, cdk_addr_to_block(addr), 1);
	cmd[1] = addr;

	if (bde_schan_op(cmd, 2, resp, 2, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] reg_read FAIL addr=0x%08x status=%d\n",
			addr, status);
		return -1;
	}
	/* Response: resp[0] = response header, resp[1] = data */
	*value = resp[1];
	return 0;
}

/*
 * sbus_reg_modify: Read-modify-write a SOC register field.
 *   addr: CDK register address
 *   mask: bit mask for field (pre-shifted)
 *   value: new field value (pre-shifted)
 */
int sbus_reg_modify(uint32_t addr, uint32_t mask, uint32_t value)
{
	uint32_t cur = 0;
	if (sbus_reg_read(addr, &cur) < 0)
		return -1;
	cur = (cur & ~mask) | (value & mask);
	return sbus_reg_write(addr, cur);
}

/*
 * sbus_mem_write: Write a memory table entry via SCHAN WRITE_MEMORY.
 *   addr: CDK memory base address (e.g. 0x03300800 for THDO_CONFIG_0Am)
 *   index: table entry index
 *   data: pointer to data words
 *   nwords: number of 32-bit words
 */
int sbus_mem_write(uint32_t addr, int index, const uint32_t *data, int nwords)
{
	uint32_t cmd[16]; /* header + address + up to 14 data words */
	uint32_t resp[2] = {0, 0}; /* response header for error checking */
	int status = -1;
	int i;

	if (nwords > 14)
		nwords = 14; /* header+addr+14 = 16 = ioctl cmd[] limit */

	cmd[0] = schan_header(SCHAN_WRITE_MEM_CMD, cdk_addr_to_block(addr),
			      (uint32_t)nwords);
	cmd[1] = addr + (uint32_t)index;
	for (i = 0; i < nwords; i++)
		cmd[2 + i] = data[i];

	if (bde_schan_op(cmd, 2 + nwords, resp, 1, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] mem_write FAIL addr=0x%08x idx=%d status=%d\n",
			addr, index, status);
		return -1;
	}
	/* Check response header for ERR (bit 6) or NACK (bit 0) */
	if (resp[0] & 0x0041u) {
		fprintf(stderr, "[sbus] mem_write RESP_ERR addr=0x%08x idx=%d "
			"resp=0x%08x\n", addr, index, resp[0]);
		return -1;
	}
	return 0;
}

/*
 * sbus_mem_read: Read a memory table entry via SCHAN READ_MEMORY.
 */
int sbus_mem_read(uint32_t addr, int index, uint32_t *data, int nwords)
{
	uint32_t cmd[2];
	uint32_t resp[16] = {0};
	int status = -1;
	int i;

	if (nwords > 14)
		nwords = 14; /* response header+14 = 15 within data[16] */

	cmd[0] = schan_header(SCHAN_READ_MEM_CMD, cdk_addr_to_block(addr),
			      (uint32_t)nwords);
	cmd[1] = addr + (uint32_t)index;

	if (bde_schan_op(cmd, 2, resp, 1 + nwords, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] mem_read FAIL addr=0x%08x idx=%d status=%d\n",
			addr, index, status);
		return -1;
	}
	/* resp[0] = response header, resp[1..] = data */
	for (i = 0; i < nwords; i++)
		data[i] = resp[1 + i];
	return 0;
}

/*
 * sbus_mem_write_blk: Write a memory entry with explicit block override.
 *   block: SBUS destination block number (not derived from addr)
 *   addr: CDK memory base address (sub-block bits in [23:20] select
 *         the memory within the block, e.g. 0x00500000 for UCMEM_DATAm)
 *   index: table entry index
 *   data: pointer to data words
 *   nwords: number of 32-bit words
 *
 * This is needed when the block number and memory address have conflicting
 * bits (e.g. XLPORT block 22 with UCMEM_DATAm at 0x00500000).
 */
int sbus_mem_write_blk(uint32_t block, uint32_t addr, int index,
		       const uint32_t *data, int nwords)
{
	uint32_t cmd[16];
	uint32_t resp[2] = {0, 0};
	int status = -1;
	int i;

	if (nwords > 14)
		nwords = 14;

	cmd[0] = schan_header(SCHAN_WRITE_MEM_CMD, block, (uint32_t)nwords);
	cmd[1] = addr + (uint32_t)index;
	for (i = 0; i < nwords; i++)
		cmd[2 + i] = data[i];

	if (bde_schan_op(cmd, 2 + nwords, resp, 1, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] mem_write_blk FAIL blk=%u addr=0x%08x "
			"idx=%d status=%d\n", block, addr, index, status);
		return -1;
	}
	if (resp[0] & 0x0041u) {
		fprintf(stderr, "[sbus] mem_write_blk RESP_ERR blk=%u "
			"addr=0x%08x idx=%d resp=0x%08x\n",
			block, addr, index, resp[0]);
		return -1;
	}
	return 0;
}

/*
 * sbus_mem_read_blk: Read a memory entry with explicit block override.
 */
int sbus_mem_read_blk(uint32_t block, uint32_t addr, int index,
		      uint32_t *data, int nwords)
{
	uint32_t cmd[2];
	uint32_t resp[16] = {0};
	int status = -1;
	int i;

	if (nwords > 14)
		nwords = 14;

	cmd[0] = schan_header(SCHAN_READ_MEM_CMD, block, (uint32_t)nwords);
	cmd[1] = addr + (uint32_t)index;

	if (bde_schan_op(cmd, 2, resp, 1 + nwords, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] mem_read_blk FAIL blk=%u addr=0x%08x "
			"idx=%d status=%d\n", block, addr, index, status);
		return -1;
	}
	for (i = 0; i < nwords; i++)
		data[i] = resp[1 + i];
	return 0;
}

/*
 * sbus_reg_write_blk: Write a 32-bit SOC register with explicit block override.
 */
int sbus_reg_write_blk(uint32_t block, uint32_t addr, uint32_t value)
{
	uint32_t cmd[3];
	int status = -1;

	cmd[0] = schan_header(SCHAN_WRITE_REG_CMD, block, 1);
	cmd[1] = addr;
	cmd[2] = value;

	if (bde_schan_op(cmd, 3, NULL, 0, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] reg_write_blk FAIL blk=%u addr=0x%08x "
			"val=0x%08x status=%d\n", block, addr, value, status);
		return -1;
	}
	return 0;
}

/*
 * sbus_reg_write64: Write a 64-bit SOC register (2 data words).
 */
int sbus_reg_write64(uint32_t addr, const uint32_t *data)
{
	uint32_t cmd[4];
	int status = -1;

	cmd[0] = schan_header(SCHAN_WRITE_REG_CMD, cdk_addr_to_block(addr), 2);
	cmd[1] = addr;
	cmd[2] = data[0];
	cmd[3] = data[1];

	if (bde_schan_op(cmd, 4, NULL, 0, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] reg_write64 FAIL addr=0x%08x status=%d\n",
			addr, status);
		return -1;
	}
	return 0;
}

/*
 * sbus_reg_read64: Read a 64-bit SOC register (2 data words).
 */
int sbus_reg_read64(uint32_t addr, uint32_t *data)
{
	uint32_t cmd[2];
	uint32_t resp[3] = {0, 0, 0};
	int status = -1;

	cmd[0] = schan_header(SCHAN_READ_REG_CMD, cdk_addr_to_block(addr), 2);
	cmd[1] = addr;

	if (bde_schan_op(cmd, 2, resp, 3, &status) < 0 || status != 0) {
		fprintf(stderr, "[sbus] reg_read64 FAIL addr=0x%08x status=%d\n",
			addr, status);
		return -1;
	}
	data[0] = resp[1];
	data[1] = resp[2];
	return 0;
}
