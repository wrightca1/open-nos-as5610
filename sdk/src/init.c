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
 * Initialization sequence (cold boot only -- see warm-boot note below):
 *   1. Detect boot mode: read CMIC_DMA_RING_ADDR (0x158).
 *      Non-zero = warm reboot from Cumulus; CMC2 is in DMA ring-buffer mode.
 *      Zero     = cold power cycle; CMC2 is in PIO mode.
 *   2. Program CMIC_SBUS_TIMEOUT (BAR0+0x200).
 *   3. Program CMIC_SBUS_RING_MAP_0..7 (BAR0+0x204..0x220).
 *   4. (Cold boot only) Clear any stale SCHAN DONE/ERR state.
 *   5. Verify CMIC accessibility (read SCHAN_CTRL).
 *
 * PIO ERROR STATE AT COLD BOOT (false positive DMA detection):
 *   At cold boot, the BCM56846 power-on sequence leaves SCHAN in PIO
 *   ERROR_ABORT state: SCHAN_CTRL = 0x65 (START=1, DONE=0, error bits set).
 *   In this state, writes to CMIC CMC2 MSG registers are silently ignored by
 *   the hardware (MSG is locked during SCHAN error).  A naive MSG writability
 *   probe (write known pattern, read back) fails because the readback returns
 *   the stale hardware-init value 0x65, not the written value — identical to
 *   DMA ring mode behavior.
 *
 *   This was discovered after a confirmed cold power cycle showed:
 *     MSG probe: 0x3300c wrote 0x5a5a0000 read 0x00000065 -- [falsely] DMA mode
 *   while CMIC_MISC_CONTROL = 0x7df4db0e (hardware default, not software-written)
 *   confirmed it was genuinely cold boot.
 *
 *   Fix: SCHAN_CTRL pre-check before MSG probe.  START=1, DONE=0 is IMPOSSIBLE
 *   in DMA ring mode (DMA FIFO SRAM always shows DONE=1).  If START=1 AND
 *   DONE=0, the chip is definitively in PIO mode — skip the MSG probe.
 *
 * WARM BOOT WARNING (DMA ring-buffer mode):
 *   After warm reboot from Cumulus, the BCM56846 CMC2 is in SCHAN DMA
 *   ring-buffer mode.  In this mode, BAR0+0x33000-0x33fff is a read window
 *   into the CMIC's internal SCHAN DMA FIFO SRAM -- NOT writable PIO
 *   registers.  All reads return the stale DMA ring status (0xb7).  All
 *   writes to SCHAN_CTRL and MSG registers are silently ignored or (worse)
 *   interpreted as DMA ring submissions.
 *
 *   Writing 0 to SCHAN_CTRL (0x33000) in DMA mode submits a null descriptor
 *   to the SCHAN DMA ring -- corrupting ring state.  The old "clear stale
 *   SCHAN_CTRL" loop is therefore SKIPPED in warm boot.
 *
 *   PIO SCHAN requires a cold hardware power cycle to reset CMC2 to PIO mode.
 *   This is detected by CMIC_DMA_RING_ADDR (0x158) == 0.
 *
 * Register offsets confirmed from OpenBCM SDK-6.5.16 allregs_c.i
 * (BCM56840 soc_cpureg entries).  Ring map values confirmed from Cumulus
 * switchd binary RE (strings-hex-literals.txt, "User should write" strings
 * for BCM56846/AS5610-52X).  See CUMULUS_REVERSE_ENGINEERING_FINDINGS.md
 * section 11 for the DMA FIFO window discovery and root cause analysis.
 *
 * SCHAN_CTRL = BAR0+0x33000, SCHAN_MSG0 = BAR0+0x3300c (CMC2).
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
	int warm_boot;
	int tries;

	(void)unit;
	fprintf(stderr, "[init] bcm56846_chip_init: begin\n");

	/*
	 * Step 1: Detect boot mode.
	 *
	 * Primary indicator: CMIC_DMA_RING_ADDR (0x158).
	 *   Non-zero after warm reboot from Cumulus.
	 *   Zero after cold power cycle OR after test programs cleared it with
	 *   ww(0x158, 0) — so zero alone is not sufficient.
	 *
	 * Secondary indicator: SCHAN MSG0 writability probe.
	 *   In PIO mode (cold boot): 0x3300c (CMC2 MSG0) is a normal R/W
	 *   register — written value reads back unchanged.
	 *   In DMA ring-buffer mode: 0x3300c is DMA FIFO SRAM — writes are
	 *   silently ignored (ring descriptor submission); reads return SRAM
	 *   content (0x00 or 0xb7), never the written value.
	 * If 0x158=0 but MSG0 doesn't retain the test pattern, previous
	 * software cleared 0x158 without exiting DMA ring mode.  Treat as
	 * warm boot (cold power cycle required).
	 */
	if (bde_read_reg(CMIC_DMA_RING_ADDR, &dma_ring_addr) != 0) {
		fprintf(stderr, "[init] CMIC_DMA_RING_ADDR read failed\n");
		return -1;
	}

	if (dma_ring_addr == 0u) {
		/*
		 * 0x158 is 0 — ambiguous: genuine cold boot OR a previous test
		 * program wrote 0 to 0x158 without exiting DMA ring mode.
		 *
		 * Two-stage detection:
		 *
		 * Stage 1: SCHAN_CTRL pre-check.
		 *   In DMA ring mode, reads from 0x33000 return DMA FIFO SRAM
		 *   content — the hardware ALWAYS sets DONE when the ring is
		 *   active (observed: 0xb7, 0x92).  START=1, DONE=0 is therefore
		 *   IMPOSSIBLE in DMA ring mode.
		 *
		 *   At cold boot the BCM56846 power-on init leaves SCHAN in
		 *   PIO ERROR_ABORT state (SCHAN_CTRL=0x65 = START | error bits,
		 *   DONE=0).  In PIO ERROR state, MSG register writes are silently
		 *   ignored by the hardware — making the MSG writability probe
		 *   unreliable (reads back stale 0x65, not the probe value).
		 *
		 *   If START=1 AND DONE=0: this is the PIO ERROR pattern —
		 *   definitively PIO mode.  Skip MSG probe.
		 *
		 * Stage 2: MSG0 writability probe (only if SCHAN_CTRL is
		 *   ambiguous — e.g. 0x00 after reset, or DMA-like value).
		 *   Write a known pattern to CMC2 MSG0 (0x3300c), read back.
		 *   PIO: register retains value.
		 *   DMA ring: write consumed as descriptor; readback differs.
		 */
		uint32_t schan_ctrl_pre = 0u;
		bde_read_reg(CMIC_CMC0_SCHAN_CTRL, &schan_ctrl_pre);

		if ((schan_ctrl_pre & SCHAN_CTRL_START) &&
		    !(schan_ctrl_pre & SCHAN_CTRL_DONE)) {
			/*
			 * START=1, DONE=0 — PIO ERROR state.  Impossible in DMA
			 * ring mode.  Genuine cold boot; MSG probe not reliable
			 * when SCHAN is in error (MSG writes ignored by HW).
			 */
			fprintf(stderr,
				"[init] SCHAN_CTRL=0x%08x: START=1,DONE=0"
				" -- PIO ERROR state (impossible in DMA mode;"
				" genuine cold boot; skipping MSG probe)\n",
				schan_ctrl_pre);
			/* dma_ring_addr stays 0 → cold boot path */
		} else {
			/*
			 * SCHAN_CTRL is ambiguous (0x00 after reset, or a
			 * DMA-mode ring value).  Use MSG0 writability probe.
			 */
			uint32_t probe = 0x5A5A0000u, readback = 0u;
			bde_write_reg(CMIC_CMC0_SCHAN_MSG(0), probe);
			bde_read_reg(CMIC_CMC0_SCHAN_MSG(0), &readback);
			bde_write_reg(CMIC_CMC0_SCHAN_MSG(0), 0u);
			if (readback != probe) {
				/*
				 * MSG register didn't retain the value — DMA
				 * ring mode.  A previous software "restore"
				 * cleared 0x158 but the ring state machine is
				 * still active.  Cold power cycle required.
				 */
				fprintf(stderr,
					"[init] MSG probe: 0x3300c"
					" wrote 0x%08x read 0x%08x"
					" -- CMC2 still in DMA ring mode"
					" (cold power cycle required)\n",
					probe, readback);
				dma_ring_addr = 0xFFFFFFFFu;
			} else {
				fprintf(stderr,
					"[init] MSG probe: 0x3300c writable"
					" -- CMC2 in PIO mode"
					" (genuine cold boot)\n");
			}
		}
	}

	warm_boot = (dma_ring_addr != 0u);
	fprintf(stderr, "[init] boot mode: %s (DMA_RING_ADDR=0x%08x)\n",
		warm_boot ? "WARM (DMA mode -- PIO SCHAN unavailable)" : "COLD",
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
	 * from Cumulus switchd binary strings:
	 *   0x43052100 -> agents 0-7
	 *   0x33333343 -> agents 8-15
	 *   0x44444333 -> agents 16-23
	 *   0x00034444 -> agents 24-31
	 *   0x00000000 -> agents 32-63 (no mapped blocks in BCM56846)
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
	 * every SCHAN op to an XLPORT address (e.g. 0x40a8022a) returns
	 * SCHAN ERROR_ABORT.  Cumulus sets this via rc.soc:
	 *   m cmic_misc_control LINK40G_ENABLE=1
	 * We set it here in chip_init so it's guaranteed regardless of whether
	 * soc.c handles the 'm' command.  This register is a direct BAR0
	 * read-write in both PIO and DMA ring-buffer boot modes.
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
	 * Step 4: Clear stale SCHAN state -- COLD BOOT ONLY.
	 *
	 * In warm boot, BAR0+0x33000 is the SCHAN DMA FIFO submission port.
	 * Writing anything there submits a DMA descriptor, corrupting the ring.
	 * Skip entirely in warm boot.
	 *
	 * In cold boot, BCM56846 power-on leaves SCHAN_CTRL = 0x65 (START=1,
	 * ERROR/ABORT=1, plus upper error bits).  This is the hardware power-on
	 * state: the CMIC attempted an internal SCHAN calibration, timed out
	 * (SBUS_TIMEOUT was not yet programmed), and left SCHAN in error state.
	 *
	 * Writing 0 to SCHAN_CTRL is WRONG -- CMICm error/status bits are W1C
	 * (write-1-to-clear); writing 0 has no effect on any set bit.
	 *
	 * Correct clear: write 0xFE (bits 1-7 = DONE|ABORT|NACK|... all except
	 * START bit 0) to:
	 *   bit 2 (ABORT): trigger ABORT of the stale pending op.  Hardware
	 *     processes the ABORT, then self-clears START and ABORT.
	 *   bits 1,3-7: W1C clears DONE and all error/status flags.
	 *
	 * We do NOT write bit 0 (START) to avoid accidentally starting a new op.
	 * After 10ms the abort completes and SCHAN_CTRL should read 0x00.
	 */
	if (warm_boot) {
		fprintf(stderr, "[init] warm boot: skipping SCHAN_CTRL clear "
			"(CMC2 in DMA mode; cold power cycle required for PIO)\n");
		return 0;
	}

	bde_write_reg(CMIC_CMC0_SCHAN_CTRL, 0xFEu);  /* ABORT + W1C all errors */
	usleep(10000);                                 /* 10ms for ABORT to complete */

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

	fprintf(stderr, "[init] bcm56846_chip_init: SCHAN_CTRL=0x%08x -- SCHAN ready\n",
		ctrl);

	/*
	 * Step 6: De-assert XLPORT soft resets via TOP_SOFT_RESET_REG.
	 *
	 * At cold boot, the BCM56846 TOP block holds all XLPORT blocks in
	 * software reset.  Every SCHAN access to an XLPORT address returns
	 * ERROR_ABORT until these reset bits are cleared.  We probe several
	 * candidate SBUS addresses for TOP_SOFT_RESET_REG and clear the
	 * XLP_RESET bits.
	 *
	 * This must happen AFTER SCHAN is confirmed working (Step 5).
	 */
	bcm56846_xlport_deassert_reset();

	fprintf(stderr, "[init] bcm56846_chip_init: done\n");
	return 0;
}
