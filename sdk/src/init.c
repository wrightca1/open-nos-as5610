/*
 * BCM56846 (Trident+) chip initialization.
 *
 * Called directly by bcm56846_init() and also by soc.c "init all".
 *
 * The kernel BDE (nos_kernel_bde.ko) performs the full chip reset sequence
 * during probe() — CPS reset, progressive CMIC_SOFT_RESET, ring maps, and
 * SCHAN abort/clear.  This init function reinforces key settings and handles
 * diagnostic verification of the reset state.
 *
 * Key insight: bde_write_reg() / bde_read_reg() perform DIRECT BAR0 reads
 * and writes via ioread32/iowrite32 in the kernel BDE.  They are NOT S-Channel.
 *
 * Initialization sequence:
 *   1. Log diagnostic registers for debug.
 *   2. Program CMIC_SBUS_TIMEOUT (0x200) + RING_MAP_0..7 (0x204..0x220).
 *   2b. Enable LINK40G in CMIC_MISC_CONTROL for XLMAC SBUS access.
 *   3. Clear stale SCHAN state (CMICe byte-write on SCHAN_CTRL at 0x50).
 *   4. Verify CMIC_SOFT_RESET and probe XLPORT blocks via SCHAN.
 *
 * CMICe SCHAN interface (BCM56846 is pre-iProc, NOT CMICm):
 *   SCHAN_CTRL = BAR0+0x0050  (byte-write: 0x80|bit=SET, 0x00|bit=CLR)
 *   SCHAN_D(n) = BAR0+n*4     (n=0..21, message buffer at 0x0000-0x0054)
 *   CMC2 (0x33000) is NON-FUNCTIONAL — do not use.
 *
 * IMPORTANT: BCM56840/56846 (Trident+) uses CMIC_SOFT_RESET_REG at BAR0+0x580
 * for block resets.  It does NOT have TOP_SOFT_RESET_REG (that's BCM56850+).
 * See docs/reverse-engineering/XLPORT_RESET_AND_SCHAN_ANALYSIS.md.
 */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include "sbus.h"
#include <stdio.h>
#include <unistd.h>

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
 * Compute CDK-style SBUS address for an XLPORT register at a specific
 * physical block number.  Uses the BCM56840 blockport_addr encoding:
 *   blocks 0-15:  addr = (block << 20) | reg_offset
 *   blocks 16-63: addr = ((block & 0xf) | 0x400) << 20) | reg_offset
 *
 * This produces addresses that cdk_addr_to_block() can decode back to
 * the correct block number for SCHAN header construction.
 */
static uint32_t xlport_block_probe_addr(int block, uint32_t reg_offset)
{
	uint32_t b = (uint32_t)block;

	if (b & 0x10)
		b = (b & 0xfu) | 0x400u;
	return (b << 20) | reg_offset;
}

/*
 * bcm56846_verify_reset_and_probe: Verify CMIC_SOFT_RESET is fully deasserted,
 * LINK40G is enabled, and probe each XLPORT block via SCHAN to discover which
 * are accessible.
 *
 * BCM56840/56846 uses CMIC_SOFT_RESET_REG at BAR0+0x580 (NOT TOP_SOFT_RESET_REG).
 * The kernel BDE sets CMIC_SOFT_RESET = 0xFFFF during probe().
 *
 * After verifying reset state, we probe XLPORT_MODE_REG at each physical
 * block 10-27 using proper CDK-format SCHAN reads (via sbus_reg_read).
 * This tells us which blocks are accessible — essential for debugging
 * Phase 7 XLPORT failures.
 */
static void bcm56846_verify_reset_and_probe(void)
{
	uint32_t soft_reset = 0, misc_ctrl = 0;
	int blk, ok_count = 0, fail_count = 0;
	uint32_t control_val = 0;

	/* Read and log CMIC_SOFT_RESET */
	bde_read_reg(CMIC_SOFT_RESET, &soft_reset);
	fprintf(stderr, "[init] CMIC_SOFT_RESET(0x580) = 0x%08x%s\n",
		soft_reset,
		(soft_reset == CMIC_SOFT_RESET_ALL_OUT) ?
			" (all out of reset)" : " -- NOT FULLY DEASSERTED");

	/* If not fully deasserted, attempt to fix */
	if (soft_reset != CMIC_SOFT_RESET_ALL_OUT) {
		fprintf(stderr,
			"[init]   fixing: writing 0x%08x to CMIC_SOFT_RESET\n",
			CMIC_SOFT_RESET_ALL_OUT);
		bde_write_reg(CMIC_SOFT_RESET, CMIC_SOFT_RESET_ALL_OUT);
		usleep(10000);  /* 10ms settle */
		bde_read_reg(CMIC_SOFT_RESET, &soft_reset);
		fprintf(stderr, "[init]   CMIC_SOFT_RESET now = 0x%08x\n",
			soft_reset);
	}

	/* Log individual port group status */
	fprintf(stderr, "[init]   PG0=%u PG1=%u PG2=%u PG3=%u "
		"MMU=%u IP=%u EP=%u\n",
		(soft_reset >> 0) & 1, (soft_reset >> 1) & 1,
		(soft_reset >> 2) & 1, (soft_reset >> 3) & 1,
		(soft_reset >> 4) & 1, (soft_reset >> 5) & 1,
		(soft_reset >> 6) & 1);

	/* Verify LINK40G */
	bde_read_reg(CMIC_MISC_CONTROL, &misc_ctrl);
	fprintf(stderr, "[init] CMIC_MISC_CONTROL(0x1c) = 0x%08x, LINK40G=%u\n",
		misc_ctrl, misc_ctrl & 1u);

	if (!(misc_ctrl & CMIC_MISC_CONTROL_LINK40G_ENABLE)) {
		misc_ctrl |= CMIC_MISC_CONTROL_LINK40G_ENABLE;
		bde_write_reg(CMIC_MISC_CONTROL, misc_ctrl);
		fprintf(stderr,
			"[init]   fixed LINK40G_ENABLE -> 0x%08x\n",
			misc_ctrl);
	}

	/*
	 * Sanity check: try reading a non-XLPORT register via SCHAN
	 * to verify SCHAN itself works.  Use GLOBAL_HDRM_LIMIT (IPIPE).
	 */
	if (sbus_reg_read(0x02380002u, &control_val) == 0)
		fprintf(stderr, "[init] SCHAN sanity: IPIPE read OK "
			"(GLOBAL_HDRM_LIMIT=0x%08x)\n", control_val);
	else
		fprintf(stderr, "[init] SCHAN sanity: IPIPE read FAILED"
			" -- SCHAN may be broken\n");

	/*
	 * Probe XLPORT blocks 10-27 via SCHAN READ_REGISTER.
	 *
	 * XLPORT_MODE_REGr (offset 0x80229) is a 32-bit register present
	 * in every XLPORT block.  We try to read it at each physical block
	 * number to determine which blocks exist on this BCM56846 die.
	 *
	 * Expected: BCM56846 has 16 XLPORT blocks (for 52 ports).
	 * Our port mapping uses blocks 11-22, 24-27 (skips 10, 23).
	 */
	fprintf(stderr, "[init] probing XLPORT blocks 10-27 "
		"via SCHAN READ_REG...\n");
	for (blk = 10; blk <= 27; blk++) {
		uint32_t addr;
		uint32_t val = 0xDEADBEEFu;
		int ret;

		addr = xlport_block_probe_addr(blk, XLPORT_MODE_REG_OFF);
		ret = sbus_reg_read(addr, &val);

		if (ret == 0) {
			fprintf(stderr,
				"[init]   block %2d (addr 0x%08x): "
				"OK  val=0x%08x\n",
				blk, addr, val);
			ok_count++;
		} else {
			fprintf(stderr,
				"[init]   block %2d (addr 0x%08x): FAIL\n",
				blk, addr);
			fail_count++;
		}
	}
	fprintf(stderr, "[init] XLPORT probe: %d OK, %d FAIL "
		"(of 18 blocks)\n", ok_count, fail_count);

	if (fail_count == 18)
		fprintf(stderr,
			"[init] WARNING: ALL XLPORT blocks inaccessible!\n"
			"[init]   Possible causes:\n"
			"[init]   - Ring map mismatch (verify 0x204-0x210)\n"
			"[init]   - SBUS timeout too short (0x200)\n"
			"[init]   - CMIC_CONFIG corruption from SCHAN overlap\n"
			"[init]   - BCM56846 needs xport_reset before access\n");
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

		/* Clear stale SCHAN DMA ring config from Cumulus warm boot.
		 * Non-zero ring_cfg (e.g. 0x32000043) may interfere with
		 * SCHAN PIO WRITE_MEM operations.  Bit 5 (CPS_RESET) is
		 * already 0 at this point. */
		if (ring_cfg != 0) {
			bde_write_reg(CMIC_SCHAN_RING_CFG, 0x00000000u);
			fprintf(stderr,
				"[init] cleared ring_cfg(0x10c) -> 0\n");
		}
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

	/* Log ring maps for verification */
	{
		uint32_t rm[4] = {0};
		bde_read_reg(CMIC_SBUS_RING_MAP_0, &rm[0]);
		bde_read_reg(CMIC_SBUS_RING_MAP_1, &rm[1]);
		bde_read_reg(CMIC_SBUS_RING_MAP_2, &rm[2]);
		bde_read_reg(CMIC_SBUS_RING_MAP_3, &rm[3]);
		fprintf(stderr,
			"[init] RING_MAP readback: 0x%08x 0x%08x "
			"0x%08x 0x%08x\n",
			rm[0], rm[1], rm[2], rm[3]);
	}

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
	 * Step 4: Verify CMIC_SOFT_RESET state and probe XLPORT blocks.
	 *
	 * BCM56840/56846 uses CMIC_SOFT_RESET_REG at BAR0+0x580 for block
	 * resets.  There is no TOP_SOFT_RESET_REG on this chip family.
	 * The kernel BDE sets CMIC_SOFT_RESET = 0xFFFF during probe().
	 * We verify and log the state, then probe each XLPORT block via
	 * SCHAN to determine which blocks are accessible.
	 */
	bcm56846_verify_reset_and_probe();

	/*
	 * Step 5: Datapath initialization (MMU buffers, queues, scheduling).
	 * Without this, the ASIC drops all packets — no forwarding occurs.
	 */
	{
		extern int bcm56846_datapath_init(void);
		if (bcm56846_datapath_init() < 0)
			fprintf(stderr,
				"[init] WARNING: datapath init failed\n");
	}

	fprintf(stderr, "[init] bcm56846_chip_init: done\n");
	return 0;
}
