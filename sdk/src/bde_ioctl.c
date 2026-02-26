/* BDE userspace layer: open /dev/nos-bde, ioctl, mmap */
#include "bde_ioctl.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

static int bde_fd = -1;
static void *bde_dma_base = MAP_FAILED;
static size_t bde_dma_size = 0;

int bde_open(void)
{
	if (bde_fd >= 0)
		return 0;
	bde_fd = open(NOS_BDE_DEVICE, O_RDWR);
	return bde_fd >= 0 ? 0 : -1;
}

void bde_close(void)
{
	if (bde_dma_base != MAP_FAILED && bde_dma_base != NULL) {
		munmap(bde_dma_base, bde_dma_size);
		bde_dma_base = MAP_FAILED;
		bde_dma_size = 0;
	}
	if (bde_fd >= 0) {
		close(bde_fd);
		bde_fd = -1;
	}
}

int bde_read_reg(uint32_t offset, uint32_t *value)
{
	struct nos_bde_reg r = { .offset = offset, .value = 0 };
	if (bde_fd < 0 || ioctl(bde_fd, NOS_BDE_READ_REG, &r) < 0)
		return -1;
	*value = r.value;
	return 0;
}

int bde_write_reg(uint32_t offset, uint32_t value)
{
	struct nos_bde_reg r = { .offset = offset, .value = value };
	return (bde_fd >= 0 && ioctl(bde_fd, NOS_BDE_WRITE_REG, &r) == 0) ? 0 : -1;
}

int bde_get_dma_info(uint64_t *pbase, uint32_t *size)
{
	struct nos_bde_dma_info info;
	if (bde_fd < 0 || ioctl(bde_fd, NOS_BDE_GET_DMA_INFO, &info) < 0)
		return -1;
	*pbase = info.pbase;
	*size = info.size;
	return 0;
}

void *bde_mmap_dma(void)
{
	uint64_t pbase;
	uint32_t size;
	if (bde_dma_base != MAP_FAILED && bde_dma_base != NULL)
		return bde_dma_base;
	if (bde_fd < 0 || bde_get_dma_info(&pbase, &size) < 0 || size == 0)
		return NULL;
	bde_dma_base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, bde_fd, 0);
	if (bde_dma_base == MAP_FAILED)
		return NULL;
	bde_dma_size = size;
	return bde_dma_base;
}

int bde_schan_op(const uint32_t *cmd, int cmd_words, uint32_t *data, int data_len, int *status)
{
	struct nos_bde_schan s = { .len = cmd_words, .status = -1 };
	memcpy(s.cmd, cmd, sizeof(uint32_t) * (cmd_words <= 8 ? cmd_words : 8));
	if (data && data_len > 0)
		memcpy(s.data, data, sizeof(uint32_t) * (data_len <= 16 ? data_len : 16));
	if (bde_fd < 0 || ioctl(bde_fd, NOS_BDE_SCHAN_OP, &s) < 0)
		return -1;
	*status = s.status;
	if (data && data_len > 0)
		memcpy(data, s.data, sizeof(uint32_t) * (data_len <= 16 ? data_len : 16));
	return 0;
}
