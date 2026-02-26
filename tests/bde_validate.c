/*
 * Phase 1d — BDE validation tests
 * Run on target (AS5610) with nos-kernel-bde.ko and nos-user-bde.ko loaded.
 * Tests: open /dev/nos-bde, READ_REG(0), GET_DMA_INFO + mmap write/read, READ_REG(0x32800).
 */
#include "bde_ioctl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CMIC_CMC0_SCHAN_CTRL  0x32800

static int fd = -1;

static int open_bde(void)
{
	fd = open(NOS_BDE_DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open " NOS_BDE_DEVICE);
		return -1;
	}
	return 0;
}

static void close_bde(void)
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

/* 1d: NOS_BDE_READ_REG(0) — first BAR0 dword (or PCI-related) */
static int test_read_reg0(void)
{
	struct nos_bde_reg r = { .offset = 0, .value = 0 };
	if (ioctl(fd, NOS_BDE_READ_REG, &r) < 0) {
		perror("NOS_BDE_READ_REG(0)");
		return -1;
	}
	printf("  READ_REG(0) = 0x%08x\n", r.value);
	return 0;
}

/* 1d: mmap DMA pool, write pattern, read back */
static int test_mmap_dma(void)
{
	struct nos_bde_dma_info info;
	void *p;
	uint32_t pattern = 0xdeadbeef;
	uint32_t *word;

	if (ioctl(fd, NOS_BDE_GET_DMA_INFO, &info) < 0) {
		perror("NOS_BDE_GET_DMA_INFO");
		return -1;
	}
	if (info.size == 0) {
		printf("  GET_DMA_INFO size 0\n");
		return -1;
	}
	printf("  DMA pbase=0x%llx size=%u\n", (unsigned long long)info.pbase, info.size);

	p = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap DMA");
		return -1;
	}
	word = (uint32_t *)p;
	*word = pattern;
	/* Ensure write is visible (could use barrier; simple test) */
	if (*word != pattern) {
		printf("  mmap write/read back failed: wrote 0x%x read 0x%x\n", pattern, *word);
		munmap(p, info.size);
		return -1;
	}
	printf("  mmap DMA: wrote 0x%x, read back 0x%x\n", pattern, *word);
	munmap(p, info.size);
	return 0;
}

/* 1d: read CMIC register 0x32800 (S-Channel control) — confirms BAR0 accessible */
static int test_read_schan_ctrl(void)
{
	struct nos_bde_reg r = { .offset = CMIC_CMC0_SCHAN_CTRL, .value = 0 };
	if (ioctl(fd, NOS_BDE_READ_REG, &r) < 0) {
		perror("NOS_BDE_READ_REG(0x32800)");
		return -1;
	}
	printf("  READ_REG(0x32800) CMIC_CMC0_SCHAN_CTRL = 0x%08x\n", r.value);
	return 0;
}

int main(void)
{
	int ok = 0;

	printf("BDE validation (Phase 1d)\n");
	if (open_bde() < 0)
		return 1;

	printf("Test 1: READ_REG(0)\n");
	if (test_read_reg0() == 0) ok++;
	printf("Test 2: GET_DMA_INFO + mmap write/read\n");
	if (test_mmap_dma() == 0) ok++;
	printf("Test 3: READ_REG(0x32800) S-Channel control\n");
	if (test_read_schan_ctrl() == 0) ok++;

	close_bde();
	printf("Passed %d/3\n", ok);
	return ok == 3 ? 0 : 1;
}
