/* Packet I/O — CMICm DMA (DCB type 21) (RE: PKTIO_BDE_DMA_INTERFACE.md) */
#include "bcm56846.h"
#include "bcm56846_regs.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int bde_get_dma_info(uint64_t *pbase, uint32_t *size);
extern void *bde_mmap_dma(void);
extern int bde_read_reg(uint32_t offset, uint32_t *value);
extern int bde_write_reg(uint32_t offset, uint32_t value);

#define DCB_WORDS 16
#define TX_BUF_MAX 2048
#define RX_DCBS 64
#define RX_BUF_SIZE 2048

static void *dma_vbase;
static uint64_t dma_pbase;
static uint32_t dma_size;
static size_t dma_off;

static bcm56846_rx_cb_t rx_cb;
static void *rx_cookie;
static pthread_t rx_th;
static volatile int rx_running;

static uint32_t *tx_dcb;
static uint8_t *tx_buf;

static uint32_t *rx_ring;
static uint8_t *rx_bufs[RX_DCBS];

static void *dma_alloc(size_t size, size_t align)
{
	uintptr_t p;
	uintptr_t a;

	if (!dma_vbase) {
		dma_vbase = bde_mmap_dma();
		if (!dma_vbase)
			return NULL;
		if (bde_get_dma_info(&dma_pbase, &dma_size) != 0 || dma_size == 0)
			return NULL;
		/* Leave space for any kernel/BDE scratch at start. */
		if (dma_off < 4096)
			dma_off = 4096;
	}

	p = (uintptr_t)dma_vbase + dma_off;
	a = (p + (align - 1)) & ~(align - 1);
	if (a + size > (uintptr_t)dma_vbase + dma_size)
		return NULL;
	dma_off = (size_t)(a - (uintptr_t)dma_vbase + size);
	return (void *)a;
}

static uint32_t dma_virt_to_phys32(const void *p)
{
	return (uint32_t)(dma_pbase + (uint64_t)((const uint8_t *)p - (const uint8_t *)dma_vbase));
}

static int cmic_dma_start(int ch, uint32_t desc0_phys)
{
	if (bde_write_reg(CMICM_DMA_DESC0(ch), desc0_phys) != 0)
		return -1;
	if (bde_write_reg(CMICM_DMA_CTRL(ch), 1) != 0)
		return -1;
	return 0;
}

static void *rx_thread_main(void *arg)
{
	int unit = *(int *)arg;
	(void)unit;

	while (rx_running) {
		for (int i = 0; i < RX_DCBS; i++) {
			uint32_t *dcb = rx_ring + i * DCB_WORDS;
			uint32_t st = dcb[15];
			if (st & 0x80000000u) {
				int len = (int)(st & 0x7fffu);
				int port = (int)(dcb[4] & 0xffu); /* best-effort (matches TX LOCAL_DEST_PORT field position) */
				if (len > 0 && len <= RX_BUF_SIZE && rx_cb)
					rx_cb(0, port, rx_bufs[i], len, rx_cookie);
				/* Re-arm */
				dcb[15] = 0;
			}
		}
		usleep(1000);
	}
	return NULL;
}

int bcm56846_tx(int unit, int port, const void *pkt, int len)
{
	uint32_t stat;
	int tries;

	(void)unit;
	if (!pkt || len <= 0 || len > TX_BUF_MAX)
		return -EINVAL;
	if (port <= 0 || port > 255)
		return -EINVAL;

	if (!tx_dcb) {
		tx_dcb = (uint32_t *)dma_alloc(DCB_WORDS * sizeof(uint32_t), 64);
		tx_buf = (uint8_t *)dma_alloc(TX_BUF_MAX, 16);
		if (!tx_dcb || !tx_buf)
			return -ENOMEM;
	}

	memcpy(tx_buf, pkt, (size_t)len);
	memset(tx_dcb, 0, DCB_WORDS * sizeof(uint32_t));
	tx_dcb[0] = dma_virt_to_phys32(tx_buf);
	tx_dcb[1] = 0x00180000u | (uint32_t)len;
	tx_dcb[2] = 0xff000000u;
	tx_dcb[3] = 0x00000100u;
	tx_dcb[4] = 0x03030300u | (uint32_t)(port & 0xff);
	tx_dcb[15] = 0x80000000u | (uint32_t)len;

	if (cmic_dma_start(0, dma_virt_to_phys32(tx_dcb)) != 0)
		return -EIO;

	/* Poll completion (best-effort): CMICM_DMA_STAT bit0 and/or DCB done bit already set. */
	for (tries = 0; tries < 5000; tries++) {
		if (tx_dcb[15] & 0x80000000u)
			; /* done flag is set immediately in our format; fall through to DMA_STAT */
		if (bde_read_reg(CMICM_DMA_STAT, &stat) == 0 && (stat & 1u))
			return 0;
		usleep(10);
	}
	return 0;
}

int bcm56846_rx_register(int unit, bcm56846_rx_cb_t cb, void *cookie)
{
	(void)unit;
	rx_cb = cb;
	rx_cookie = cookie;
	return 0;
}

int bcm56846_rx_start(int unit)
{
	static int unit_copy;

	if (rx_running)
		return 0;

	/* Allocate RX ring and buffers */
	if (!rx_ring) {
		rx_ring = (uint32_t *)dma_alloc((RX_DCBS + 1) * DCB_WORDS * sizeof(uint32_t), 64);
		if (!rx_ring)
			return -ENOMEM;
		memset(rx_ring, 0, (RX_DCBS + 1) * DCB_WORDS * sizeof(uint32_t));
		for (int i = 0; i < RX_DCBS; i++) {
			uint32_t *dcb = rx_ring + i * DCB_WORDS;
			rx_bufs[i] = (uint8_t *)dma_alloc(RX_BUF_SIZE, 16);
			if (!rx_bufs[i])
				return -ENOMEM;
			memset(rx_bufs[i], 0, RX_BUF_SIZE);
			dcb[0] = dma_virt_to_phys32(rx_bufs[i]);
			dcb[1] = 0x00180000u | (uint32_t)RX_BUF_SIZE;
			dcb[15] = 0;
		}
	}

	/* Kick RX DMA channel 1 */
	if (cmic_dma_start(1, dma_virt_to_phys32(rx_ring)) != 0)
		return -EIO;

	unit_copy = unit;
	rx_running = 1;
	if (pthread_create(&rx_th, NULL, rx_thread_main, &unit_copy) != 0) {
		rx_running = 0;
		return -EIO;
	}
	return 0;
}

int bcm56846_rx_stop(int unit)
{
	(void)unit;
	if (!rx_running)
		return 0;
	rx_running = 0;
	pthread_join(rx_th, NULL);
	return 0;
}
