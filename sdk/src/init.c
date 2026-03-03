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
 * Initialization sequence for cold-boot or fresh-ONIE-install:
 *   1. Program CMIC_SBUS_TIMEOUT (BAR0+0x200) -- sets the SBUS cycle timeout.
 *   2. Program CMIC_SBUS_RING_MAP_0..7 (BAR0+0x204..0x220) -- maps each ASIC
 *      sub-block (by agent ID) to a specific S-bus ring.  Without these the
 *      CMIC returns SCHAN DONE immediately but drives no actual SBUS cycle,
 *      so all S-Channel reads return 0x00000000.
 *   3. Clear any stale SCHAN DONE/ERR state.
 *   4. Verify CMIC accessibility (read SCHAN_CTRL).
 *
 * Register offsets confirmed from OpenBCM SDK-6.5.16 allregs_c.i
 * (BCM56840 soc_cpureg entries).  Ring map values confirmed from Cumulus
 * switchd binary RE (strings-hex-literals.txt, "User should write" strings
 * for BCM56846/AS5610-52X).  See RE doc ASIC_INIT_AND_DMA_MAP.md.
 *
 * SCHAN address: libopennsl string "PCI offset from: 0x3300c to: 0x33060"
 * confirms CMC2 (base 0x33000) is the SCHAN CMC used for BCM56846.
 * SCHAN_CTRL = 0x33000, SCHAN_MSG0 = 0x3300c.  See SCHAN_DISCOVERY_REPORT.md.
 *
 * NOTE: After warm reboot from Cumulus, CMC2 is in ring-buffer DMA mode and
 * 0x33000 is non-writable from PCIe.  PIO SCHAN requires a cold power cycle.
 */
#include "bcm56846_regs.h"
#include "bde_ioctl.h"
#include <stdio.h>
#include <unistd.h>

/* CMIC_CMC0_SCHAN_CTRL bits */
#define SCHAN_CTRL_START   (1u << 0)
#define SCHAN_CTRL_DONE    (1u << 1)
#define SCHAN_CTRL_ERR     ((1u << 2) | (1u << 3))

int bcm56846_chip_init(int unit)
{
	uint32_t ctrl = 0;
	int tries;

	(void)unit;
	fprintf(stderr, "[init] bcm56846_chip_init: begin\n");

	/*
	 * Step 1: Program CMIC_SBUS_TIMEOUT.
	 * BAR0+0x200.  Confirmed from allregs_c.i (BCM56624/56840 variant).
	 * 0x7d0 = 2000 cycles (matches schan_timeout_usec=300000 intent).
	 */
	bde_write_reg(CMIC_SBUS_TIMEOUT, CMIC_SBUS_TIMEOUT_VAL);
	fprintf(stderr, "[init] CMIC_SBUS_TIMEOUT(0x200) = 0x%08x\n",
		CMIC_SBUS_TIMEOUT_VAL);

	/*
	 * Step 2: Program CMIC_SBUS_RING_MAP registers (BAR0+0x204..0x220).
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
	 * Without these, SCHAN_CTRL goes DONE immediately but returns all zeros
	 * because the CMIC drives no actual SBUS cycle (no ring is mapped).
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
	 * Step 3: Clear any stale SCHAN DONE/ERROR state.
	 * Write 0 to SCHAN_CTRL to de-assert any leftover bits.
	 */
	bde_write_reg(CMIC_CMC0_SCHAN_CTRL, 0u);
	usleep(1000);

	/* Step 4: Verify CMICm is accessible by reading back SCHAN_CTRL. */
	if (bde_read_reg(CMIC_CMC0_SCHAN_CTRL, &ctrl) != 0) {
		fprintf(stderr, "[init] SCHAN_CTRL read failed -- BDE not open?\n");
		return -1;
	}

	if (ctrl & (SCHAN_CTRL_DONE | SCHAN_CTRL_ERR)) {
		fprintf(stderr, "[init] SCHAN_CTRL=0x%08x after clear -- "
			"stale state; attempting to clear\n", ctrl);
		for (tries = 0; tries < 10; tries++) {
			bde_write_reg(CMIC_CMC0_SCHAN_CTRL, 0u);
			usleep(5000);
			if (bde_read_reg(CMIC_CMC0_SCHAN_CTRL, &ctrl) == 0 &&
			    !(ctrl & (SCHAN_CTRL_DONE | SCHAN_CTRL_ERR)))
				break;
		}
	}

	fprintf(stderr, "[init] bcm56846_chip_init: done (SCHAN_CTRL=0x%08x)\n", ctrl);
	return 0;
}
