/*
 * BCM56846 (Trident+) chip initialization -- CMICm bringup.
 *
 * Called by soc.c when rc.soc executes "init all".
 *
 * Key insight: bde_write_reg() / bde_read_reg() perform DIRECT BAR0 reads
 * and writes via ioread32/iowrite32 in the kernel BDE.  They are NOT S-Channel.
 * S-Channel is only for SOC (ASIC-internal) registers and memories; CMIC
 * control registers (SBUS_TIMEOUT, SBUS_RING_MAP, SCHAN_CTRL, DMA) all
 * require direct BAR0 access -- which is what bde_{read,write}_reg() provide.
 *
 * Initialization sequence (always runs regardless of boot mode):
 *   1. Log diagnostic registers (0x158, 0x148, 0x10c, 0x400) for debug.
 *   2. Program CMIC_SBUS_TIMEOUT (BAR0+0x200).
 *   3. Program CMIC_SBUS_RING_MAP_0..7 (BAR0+0x204..0x220).
 *   3b. Enable LINK40G in CMIC_MISC_CONTROL for XLMAC SBUS access.
 *   4. Clear stale SCHAN DONE/ERR state (W1C write to SCHAN_CTRL).
 *   5. Verify CMIC accessibility (read SCHAN_CTRL).
 *   6. De-assert XLPORT soft resets (TOP_SOFT_RESET_REG via SCHAN).
 *
 * HARDWARE POWER-ON DEFAULTS (confirmed 2026-03-05):
 *   After cold power cycle, these registers contain non-zero values that
 *   were previously misidentified as "Cumulus DMA ring mode state":
 *     BAR0+0x10c = 0x32000043  (SCHAN DMA ring config — HW default)
 *     BAR0+0x148 = 0x80000000  (CMIC_DMA_CFG — HW default, DO NOT WRITE)
 *     BAR0+0x400 = 0x505b8d80  (SCHAN DMA ring head pointer — HW default)
 *   These are hardware power-on defaults, NOT persisted Cumulus state.
 *   They do NOT indicate that DMA ring mode is active.
 *
 * BOOT MODE DETECTION:
 *   CMIC_DMA_RING_ADDR (BAR0+0x158) is the most reliable indicator:
 *     0x0294ffd0 (or similar non-zero) = warm boot from Cumulus
 *     0x00000000 = cold boot (P2020 PCIe PERST_N clears this on reboot)
 *   This value is logged for diagnostics but no longer gates initialization.
 *   All init steps run regardless of boot mode — the kernel BDE's per-op
 *   abort/clear in nos_bde_schan_op() handles SCHAN state management.
 *
 * SCHAN_CTRL = BAR0+0x33000, SCHAN_MSG0 = BAR0+0x3300c (CMC1).
 */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include <stdio.h>
#include <unistd.h>

extern int schan_read_memory(int unit, uint32_t addr, uint32_t *data, int num_words);
extern int schan_write_memory(int unit, uint32_t addr, const uint32_t *data, int num_words);

/* CMIC_CMC2_SCHAN_CTRL bits (only valid in PIO / cold-boot mode) */
#define SCHAN_CTRL_START   (1u << 0)
#define SCHAN_CTRL_DONE    (1u << 1)
#define SCHAN_CTRL_ERR     ((1u << 2) | (1u << 3))

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
	uint32_t dma_ring_addr = 0;
	int tries;

	(void)unit;
	fprintf(stderr, "[init] bcm56846_chip_init: begin\n");

	/*
	 * Step 1: Log diagnostic registers.
	 *
	 * These are read-only diagnostics — no gating of init behavior.
	 *
	 * CMIC_DMA_RING_ADDR (0x158): non-zero after Cumulus warm boot
	 *   (Cumulus writes ring buffer PA here).  Zero after cold boot
	 *   (P2020 PCIe PERST_N clears this register on every reboot).
	 *
	 * CMIC_DMA_CFG (0x148): HW default 0x80000000.  DO NOT WRITE.
	 *
	 * SCHAN ring regs (0x10c, 0x400): Hardware power-on defaults
	 *   0x32000043 and 0x505b8d80 respectively.  These are NOT
	 *   indicators of DMA ring mode — they are always present.
	 */
	if (bde_read_reg(CMIC_DMA_RING_ADDR, &dma_ring_addr) != 0) {
		fprintf(stderr, "[init] CMIC_DMA_RING_ADDR read failed\n");
		return -1;
	}

	{
		uint32_t reg_0148 = 0u, ring_cfg = 0u, ring_head = 0u;
		bde_read_reg(CMIC_DMA_CFG, &reg_0148);
		bde_read_reg(CMIC_SCHAN_RING_CFG, &ring_cfg);
		bde_read_reg(CMIC_SCHAN_RING_HEAD, &ring_head);
		fprintf(stderr,
			"[init] diagnostics: DMA_RING_ADDR(0x158)=0x%08x"
			" DMA_CFG(0x148)=0x%08x\n",
			dma_ring_addr, reg_0148);
		fprintf(stderr,
			"[init] diagnostics: ring_cfg(0x10c)=0x%08x"
			" ring_head(0x400)=0x%08x"
			" (HW defaults: 0x32000043, 0x505b8d80)\n",
			ring_cfg, ring_head);
	}

	fprintf(stderr, "[init] boot hint: %s (DMA_RING_ADDR=0x%08x)\n",
		(dma_ring_addr == 0u) ? "COLD (0x158=0)" :
		"WARM (0x158 has Cumulus ring PA)",
		dma_ring_addr);

	/*
	 * Step 2: Program CMIC_SBUS_TIMEOUT.
	 * BAR0+0x200.  Confirmed from allregs_c.i (BCM56624/56840 variant).
	 * 0x7d0 = 2000 cycles (matches schan_timeout_usec=300000 intent).
	 */
	bde_write_reg(CMIC_SBUS_TIMEOUT, CMIC_SBUS_TIMEOUT_VAL);
	fprintf(stderr, "[init] CMIC_SBUS_TIMEOUT(0x200) = 0x%08x\n",
		CMIC_SBUS_TIMEOUT_VAL);

	/*
	 * Step 3: Program CMIC_SBUS_RING_MAP registers (BAR0+0x204..0x220).
	 *
	 * Each register maps 8 agent IDs (4 bits each) to S-bus rings 0-7.
	 * Values are chip-specific for BCM56846 (Trident+ AS5610-52X), taken
	 * from Cumulus switchd binary strings.
	 *
	 * These registers are writable in both PIO and DMA modes and persist
	 * across warm reboots.  Programming them here is safe and correct.
	 */
	bde_write_reg(CMIC_SBUS_RING_MAP_0, BCM56846_RING_MAP_0);
	bde_write_reg(CMIC_SBUS_RING_MAP_1, BCM56846_RING_MAP_1);
	bde_write_reg(CMIC_SBUS_RING_MAP_2, BCM56846_RING_MAP_2);
	bde_write_reg(CMIC_SBUS_RING_MAP_3, BCM56846_RING_MAP_3);
	bde_write_reg(CMIC_SBUS_RING_MAP_4, BCM56846_RING_MAP_4);
	bde_write_reg(CMIC_SBUS_RING_MAP_5, BCM56846_RING_MAP_5);
	bde_write_reg(CMIC_SBUS_RING_MAP_6, BCM56846_RING_MAP_6);
	bde_write_reg(CMIC_SBUS_RING_MAP_7, BCM56846_RING_MAP_7);
	fprintf(stderr,
		"[init] SBUS_RING_MAP 0x204..0x220 programmed: "
		"0x%08x 0x%08x 0x%08x 0x%08x 0 0 0 0\n",
		BCM56846_RING_MAP_0, BCM56846_RING_MAP_1,
		BCM56846_RING_MAP_2, BCM56846_RING_MAP_3);

	/*
	 * Step 3b: Enable LINK40G for XLMAC SBUS access.
	 *
	 * CMIC_MISC_CONTROL (BAR0+0x1c) bit 0 = LINK40G_ENABLE.  This bit
	 * gates SBUS accesses to 40G (XLMAC/XLPORT) port blocks.  Without it,
	 * every SCHAN op to an XLPORT address returns SCHAN ERROR_ABORT.
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
	 * Step 4: Clear stale SCHAN state.
	 *
	 * Always attempt regardless of boot mode.  The kernel BDE's per-op
	 * abort/clear handles SCHAN state, but clearing here gives a clean
	 * baseline for the XLPORT deassert probes in step 6.
	 *
	 * CMICm SCHAN_CTRL error/status bits are W1C (write-1-to-clear).
	 * Write 0xFE (all bits except START) to abort any pending op and
	 * clear all error flags.
	 */
	bde_read_reg(CMIC_CMC0_SCHAN_CTRL, &ctrl);
	fprintf(stderr, "[init] SCHAN_CTRL(0x33000) pre-clear = 0x%08x\n", ctrl);

	if (ctrl != 0u) {
		bde_write_reg(CMIC_CMC0_SCHAN_CTRL, 0xFEu);
		usleep(10000);
	}

	/* Step 5: Verify CMICm is accessible and SCHAN state is clear. */
	if (bde_read_reg(CMIC_CMC0_SCHAN_CTRL, &ctrl) != 0) {
		fprintf(stderr, "[init] SCHAN_CTRL read failed -- BDE not open?\n");
		return -1;
	}

	if (ctrl != 0u) {
		fprintf(stderr, "[init] SCHAN_CTRL=0x%08x after 0xFE clear -- "
			"retrying\n", ctrl);
		for (tries = 0; tries < 5; tries++) {
			bde_write_reg(CMIC_CMC0_SCHAN_CTRL, 0xFEu);
			usleep(10000);
			if (bde_read_reg(CMIC_CMC0_SCHAN_CTRL, &ctrl) == 0 &&
			    ctrl == 0u)
				break;
		}
	}

	fprintf(stderr, "[init] SCHAN_CTRL after clear = 0x%08x%s\n",
		ctrl, (ctrl == 0u) ? " -- PIO ready" : " -- SCHAN NOT clear (may be DMA mode)");

	/*
	 * Step 6: De-assert XLPORT soft resets via TOP_SOFT_RESET_REG.
	 *
	 * At cold boot, the BCM56846 TOP block holds all XLPORT blocks in
	 * software reset.  Every SCHAN access to an XLPORT address returns
	 * ERROR_ABORT until these reset bits are cleared.
	 *
	 * Attempt even if SCHAN_CTRL is not fully clear — the kernel BDE's
	 * per-op abort/clear may succeed where the init-time clear did not.
	 */
	bcm56846_xlport_deassert_reset();

	fprintf(stderr, "[init] bcm56846_chip_init: done\n");
	return 0;
}
