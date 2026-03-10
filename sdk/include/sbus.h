/* SBUS register/memory access — CDK-format SCHAN messaging */
#ifndef SBUS_H
#define SBUS_H

#include <stdint.h>

/* 32-bit register access */
int sbus_reg_write(uint32_t addr, uint32_t value);
int sbus_reg_read(uint32_t addr, uint32_t *value);
int sbus_reg_modify(uint32_t addr, uint32_t mask, uint32_t value);

/* 64-bit register access */
int sbus_reg_write64(uint32_t addr, const uint32_t *data);
int sbus_reg_read64(uint32_t addr, uint32_t *data);

/* Memory table access */
int sbus_mem_write(uint32_t addr, int index, const uint32_t *data, int nwords);
int sbus_mem_read(uint32_t addr, int index, uint32_t *data, int nwords);

/* Register access with explicit block override */
int sbus_reg_write_blk(uint32_t block, uint32_t addr, uint32_t value);

/* Memory access with explicit block override (for XLPORT sub-block routing) */
int sbus_mem_write_blk(uint32_t block, uint32_t addr, int index,
		       const uint32_t *data, int nwords);
int sbus_mem_read_blk(uint32_t block, uint32_t addr, int index,
		      uint32_t *data, int nwords);

/* Per-port address encoding (CDK block/port format) */
uint32_t cdk_port_addr(uint32_t base, int port);

#endif
