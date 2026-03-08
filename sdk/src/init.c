/*
 * BCM56846 (Trident+) chip initialization.
 *
 * Called directly by bcm56846_init() and also by soc.c "init all".
 *
 * The kernel BDE (nos_kernel_bde.ko) performs the full chip reset sequence
 * during probe() — CPS reset, progressive CMIC_SOFT_RESET, ring maps, and
 * SCHAN abort/clear.  This init function reinforces key settings and handles
 * XLPORT deassert which requires working SCHAN.
 *
 * Key insight: bde_write_reg() / bde_read_reg() perform DIRECT BAR0 reads
 * and writes via ioread32/iowrite32 in the kernel BDE.  They are NOT S-Channel.
 *
 * Initialization sequence:
 *   1. Log diagnostic registers for debug.
 *   2. Program CMIC_SBUS_TIMEOUT (0x200) + RING_MAP_0..7 (0x204..0x220).
 *   2b. Enable LINK40G in CMIC_MISC_CONTROL for XLMAC SBUS access.
 *   3. Clear stale SCHAN state (CMICe byte-write on SCHAN_CTRL at 0x50).
 *   4. De-assert XLPORT soft resets (TOP_SOFT_RESET_REG via SCHAN).
 *
 * CMICe SCHAN interface (BCM56846 is pre-iProc, NOT CMICm):
 *   SCHAN_CTRL = BAR0+0x0050  (byte-write: 0x80|bit=SET, 0x00|bit=CLR)
 *   SCHAN_D(n) = BAR0+n*4     (n=0..21, message buffer at 0x0000-0x0054)
 *   CMC2 (0x33000) is NON-FUNCTIONAL — do not use.
 */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include <stdio.h>
#include <unistd.h>

extern int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words);
extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);

/* CMICe SCHAN_CTRL (BAR0+0x50) byte-write values */
#define CMICE_SCHAN_CTRL          0x0050u
#define CMICE_SET_ABORT           0x82u   /* SET bit 2 */
#define CMICE_CLR_ABORT           0x02u   /* CLR bit 2 */
#define CMICE_CLR_START           0x00u   /* CLR bit 0 */
#define CMICE_CLR_DONE            0x01u   /* CLR bit 1 */
#define CMICE_CLR_SER             0x14u   /* CLR bit 20 */
#define CMICE_CLR_NAK             0x15u   /* CLR bit 21 */
#define CMICE_CLR_TIMEOUT         0x16u   /* CLR bit 22 */

/*
 * bcm56846_xlport_deassert_reset: find TOP_SOFT_RESET_REG SBUS address and
 * clear XLPORT soft-reset bits.
 *
 * After cold power cycle, BCM56846 TOP block holds all XLPORT blocks in
 * software reset (TOP_SOFT_RESET_REG bits[12:0] = 1).  Until these bits are
 * cleared, every SCHAN op targeting XLPORT/XLMAC returns ERROR_ABORT.
 *
 * The exact SBUS address for TOP_SOFT_RESET_REG is not confirmed from RE.
 * Multiple candidates are probed; a "good" read returns a non-zero value
 * with only bits[12:0] set (the 13 XLP_RESET bits for BCM56846).
 *
 * Returns 0 on success, -1 if all candidates fail.
 */
static int bcm56846_xlport_deassert_reset(void)
{
	static const struct {
		uint32_t addr;
		const char *desc;
	} candidates[] = {
		{ BCM56846_TOP_SOFT_RESET_CAND_0, "RE-confirmed SCHAN word (0x28033200)" },
		{ BCM56846_TOP_SOFT_RESET_CAND_1, "SCHAN addr no-opcode (0x00033200)" },
		{ BCM56846_TOP_SOFT_RESET_CAND_2, "agent<<16|0x200 (0x00030200)" },
		{ BCM56846_TOP_SOFT_RESET_CAND_3, "agent<<20|0x200 (0x00300200)" },
		{ BCM56846_TOP_SOFT_RESET_CAND_4, "ring2<<24|agent<<16|0x200 (0x02030200)" },
		{ BCM56846_TOP_SOFT_RESET_CAND_5, "0x40-prefix (0x40030200)" },
		{ BCM56846_TOP_SOFT_RESET_CAND_6, "bare offset 0x200 (0x00000200)" },
	};
	int i, ret;
	uint32_t val;

	fprintf(stderr, "[init] probing TOP_SOFT_RESET_REG SBUS address...\n");

	for (i = 0; i < (int)(sizeof(candidates)/sizeof(candidates[0])); i++) {
		val = 0xFFFFFFFFu;
		ret = schan_read_memory(0, candidates[i].addr, &val, 1);
		fprintf(stderr, "[init]   0x%08x (%s): ret=%d val=0x%08x\n",
			candidates[i].addr, candidates[i].desc, ret, val);
		if (ret != 0 || val == 0xFFFFFFFFu || val == 0xDEADBEEFu)
			continue;
		/*
		 * Accept if upper 19 bits clear (only XLP bits[12:0] expected).
		 * Writing 0 to a wrong address with only low bits is low-risk.
		 */
		if ((val & ~BCM56846_XLPORT_RESET_MASK) == 0u) {
			fprintf(stderr,
				"[init] TOP_SOFT_RESET_REG = 0x%08x at 0x%08x"
				" (XLP[12:0]=0x%04x)\n",
				val, candidates[i].addr,
				val & BCM56846_XLPORT_RESET_MASK);
			if (val != 0u) {
				uint32_t zero = 0u;
				ret = schan_write_memory(0, candidates[i].addr,
							 &zero, 1);
				fprintf(stderr,
					"[init] TOP_SOFT_RESET_REG write 0"
					" -> ret=%d (0=OK)\n", ret);
			} else {
				fprintf(stderr,
					"[init] TOP_SOFT_RESET_REG already 0"
					" (XLPORT not in reset)\n");
			}
			return 0;
		}
		fprintf(stderr,
			"[init]   unexpected upper bits 0x%08x, skipping\n",
			val & ~BCM56846_XLPORT_RESET_MASK);
	}

	fprintf(stderr,
		"[init] WARNING: TOP_SOFT_RESET_REG not found -- all probes"
		" failed or returned unexpected values.\n"
		"[init] XLPORT SCHAN access will return ERROR_ABORT.\n");
	return -1;
}

int bcm56846_chip_init(int unit)
{
	uint32_t ctrl = 0;

	(void)unit;
	fprintf(stderr, "[init] bcm56846_chip_init: begin\n");

	/*
	 * Step 1: Log diagnostic registers (read-only, no gating).
	 */
	{
		uint32_t dma_ring_addr = 0u, dma_cfg = 0u;
		uint32_t ring_cfg = 0u, ring_head = 0u;
		bde_read_reg(CMIC_DMA_RING_ADDR, &dma_ring_addr);
		bde_read_reg(CMIC_DMA_CFG, &dma_cfg);
		bde_read_reg(CMIC_SCHAN_RING_CFG, &ring_cfg);
		bde_read_reg(CMIC_SCHAN_RING_HEAD, &ring_head);
		fprintf(stderr,
			"[init] diagnostics: DMA_RING_ADDR(0x158)=0x%08x"
			" DMA_CFG(0x148)=0x%08x\n",
			dma_ring_addr, dma_cfg);
		fprintf(stderr,
			"[init] diagnostics: ring_cfg(0x10c)=0x%08x"
			" ring_head(0x400)=0x%08x\n",
			ring_cfg, ring_head);
	}

	/*
	 * Step 2: Program CMIC_SBUS_TIMEOUT (BAR0+0x200) and
	 * CMIC_SBUS_RING_MAP registers (BAR0+0x204..0x220).
	 *
	 * IMPORTANT: 0x200 is SBUS timeout, ring maps start at 0x204.
	 * The kernel BDE programs these during probe(), but we reinforce here.
	 */
	bde_write_reg(CMIC_SBUS_TIMEOUT, 0x7D0);	/* 2000 cycles */
	bde_write_reg(CMIC_SBUS_RING_MAP_0, BCM56846_RING_MAP_0);
	bde_write_reg(CMIC_SBUS_RING_MAP_1, BCM56846_RING_MAP_1);
	bde_write_reg(CMIC_SBUS_RING_MAP_2, BCM56846_RING_MAP_2);
	bde_write_reg(CMIC_SBUS_RING_MAP_3, BCM56846_RING_MAP_3);
	bde_write_reg(CMIC_SBUS_RING_MAP_4, BCM56846_RING_MAP_4);
	bde_write_reg(CMIC_SBUS_RING_MAP_5, BCM56846_RING_MAP_5);
	bde_write_reg(CMIC_SBUS_RING_MAP_6, BCM56846_RING_MAP_6);
	bde_write_reg(CMIC_SBUS_RING_MAP_7, BCM56846_RING_MAP_7);
	fprintf(stderr,
		"[init] SBUS_TIMEOUT=0x7D0 RING_MAP 0x204..0x220: "
		"0x%08x 0x%08x 0x%08x 0x%08x 0 0 0 0\n",
		BCM56846_RING_MAP_0, BCM56846_RING_MAP_1,
		BCM56846_RING_MAP_2, BCM56846_RING_MAP_3);

	/*
	 * Step 2b: Enable LINK40G for XLMAC SBUS access.
	 *
	 * CMIC_MISC_CONTROL (BAR0+0x1c) bit 0 = LINK40G_ENABLE.  This bit
	 * gates SBUS accesses to 40G (XLMAC/XLPORT) port blocks.  Without it,
	 * every SCHAN op to an XLPORT address returns ERROR_ABORT.
	 */
	{
		uint32_t misc_ctrl = 0u;
		if (bde_read_reg(CMIC_MISC_CONTROL, &misc_ctrl) == 0) {
			misc_ctrl |= CMIC_MISC_CONTROL_LINK40G_ENABLE;
			bde_write_reg(CMIC_MISC_CONTROL, misc_ctrl);
			fprintf(stderr,
				"[init] CMIC_MISC_CONTROL(0x1c) |= LINK40G_ENABLE"
				" -> 0x%08x\n", misc_ctrl);
		}
	}

	/*
	 * Step 3: Clear stale SCHAN state using CMICe byte-write protocol.
	 *
	 * CMICe SCHAN_CTRL at BAR0+0x50.  CMC2 (0x33000) is non-functional.
	 */
	bde_read_reg(CMICE_SCHAN_CTRL, &ctrl);
	fprintf(stderr, "[init] SCHAN_CTRL(0x50) pre-clear = 0x%08x\n", ctrl);

	if (ctrl != 0u) {
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_SET_ABORT);
		usleep(1000);
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_CLR_ABORT);
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_CLR_START);
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_CLR_DONE);
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_CLR_SER);
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_CLR_NAK);
		bde_write_reg(CMICE_SCHAN_CTRL, CMICE_CLR_TIMEOUT);
	}

	bde_read_reg(CMICE_SCHAN_CTRL, &ctrl);
	fprintf(stderr, "[init] SCHAN_CTRL after clear = 0x%08x%s\n",
		ctrl, (ctrl == 0u) ? " -- PIO ready" : " -- not clear");

	/*
	 * Step 4: De-assert XLPORT soft resets via TOP_SOFT_RESET_REG.
	 */
	bcm56846_xlport_deassert_reset();

	fprintf(stderr, "[init] bcm56846_chip_init: done\n");
	return 0;
}
