/* Userspace BDE ioctl interface — must match bde/nos_user_bde.c */
#ifndef BDE_IOCTL_H
#define BDE_IOCTL_H

#include <stdint.h>
#include <sys/ioctl.h>

#define NOS_BDE_DEVICE "/dev/nos-bde"

#define NOS_BDE_MAGIC 'B'
#define NOS_BDE_READ_REG     _IOWR(NOS_BDE_MAGIC, 1, struct nos_bde_reg)
#define NOS_BDE_WRITE_REG    _IOW(NOS_BDE_MAGIC, 2, struct nos_bde_reg)
#define NOS_BDE_GET_DMA_INFO _IOR(NOS_BDE_MAGIC, 3, struct nos_bde_dma_info)
#define NOS_BDE_SCHAN_OP     _IOWR(NOS_BDE_MAGIC, 4, struct nos_bde_schan)

struct nos_bde_reg {
	uint32_t offset;
	uint32_t value;
};

struct nos_bde_dma_info {
	uint64_t pbase;
	uint32_t size;
};

struct nos_bde_schan {
	uint32_t cmd[8];
	uint32_t data[16];
	int32_t  len;
	int32_t  status;
};

/* BDE layer API (implemented in bde_ioctl.c) */
int bde_open(void);
void bde_close(void);
int bde_read_reg(uint32_t offset, uint32_t *value);
int bde_write_reg(uint32_t offset, uint32_t value);
int bde_get_dma_info(uint64_t *pbase, uint32_t *size);
void *bde_mmap_dma(void);
int bde_schan_op(const uint32_t *cmd, int cmd_words, uint32_t *data, int data_len, int *status);

#endif
