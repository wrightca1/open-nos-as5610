/*
 * nos-kernel-bde — Kernel BDE for BCM56846 (Trident+)
 * PCI probe, BAR0 map, DMA pool, S-Channel transport.
 * No Broadcom/OpenNSL code; from RE docs only.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#define PCI_VENDOR_ID_BROADCOM	0x14e4
#define PCI_DEVICE_ID_BCM56846	0xb846

#define BAR0_SIZE	(256 * 1024)
#define DMA_POOL_SIZE	(4 * 1024 * 1024)	/* max order-10 alloc on PPC32/mpc85xx */

/* CMICm DMA offsets (unused for now, retained for future DMA support) */
#define CMICM_DMA_CTRL(ch)    (0x31140 + 4 * (ch))
#define CMICM_DMA_DESC0(ch)   (0x31158 + 4 * (ch))
#define CMICM_DMA_STAT        0x31150

/*
 * S-Channel PIO — CMICe interface on BCM56846 (Trident+, pre-iProc).
 *
 * The BCM56846 has a transitional CMIC that exposes CMICm-style register
 * addresses (0x31000+, 0x33000+) but they are NOT functional for PIO.
 * The working SCHAN interface uses legacy CMICe registers:
 *
 *   SCHAN_CTRL = BAR0 + 0x0050  (CMICe byte-write format)
 *   SCHAN_D(n) = BAR0 + n*4     (n=0..21, overlaps CMIC reg space 0x0000-0x0054)
 *
 * CMICe byte-write format for SCHAN_CTRL:
 *   Write (0x80 | bit_num) to SET a bit in the register.
 *   Write (0x00 | bit_num) to CLR a bit in the register.
 *   Read returns the full 32-bit register value.
 *
 * SCHAN_CTRL status bits (read):
 *   bit 0  = START (host-asserted)
 *   bit 1  = DONE (set by HW on completion)
 *   bit 2  = ABORT
 *   bit 22 = SBUS TIMEOUT
 *   bit 21 = SBUS NAK
 *
 * SCHAN response header at SCHAN_D(0):
 *   bit 6  = ERR (SBUS target error)
 *   bit 0  = NACK
 *
 * IMPORTANT: SCHAN_D(0..20) overlaps CMIC register space 0x0000-0x0050.
 * A SCHAN response overwrites 0x0000..0x0004 (for 4-byte reads).
 * MISC_CONTROL (0x1C) is SCHAN_D(7) but is safe for <= 7-word responses.
 * SCHAN_CTRL (0x50) is SCHAN_D(20) — never overwritten by normal ops.
 *
 * Confirmed experimentally 2026-03-06:
 *   - CMC2 (0x33000) returns 0x80 for reads, MSG not writable — non-functional
 *   - CMICe SCHAN at 0x50 works: all 64 SBUS blocks respond after chip reset
 *   - Write/read verified: wrote 0xDEADBEEF to blk 10, read back matched
 */
#define CMICE_SCHAN_CTRL      0x0050
#define CMICE_SCHAN_D(n)      ((n) * 4)   /* n=0..21 */
#define SCHAN_MAX_MSG_WORDS   21

/* CMICe byte-write values for SCHAN_CTRL */
#define CMICE_SET_START       0x80u   /* SET bit 0 */
#define CMICE_SET_ABORT       0x82u   /* SET bit 2 */
#define CMICE_CLR_START       0x00u   /* CLR bit 0 */
#define CMICE_CLR_DONE        0x01u   /* CLR bit 1 */
#define CMICE_CLR_ABORT       0x02u   /* CLR bit 2 */
#define CMICE_CLR_SER         0x14u   /* CLR bit 20 */
#define CMICE_CLR_NAK         0x15u   /* CLR bit 21 */
#define CMICE_CLR_TIMEOUT     0x16u   /* CLR bit 22 */

/* SCHAN_CTRL read-back bits */
#define SCHAN_CTRL_DONE       (1u << 1)
#define SCHAN_CTRL_TIMEOUT    (1u << 22)
#define SCHAN_CTRL_NAK        (1u << 21)

/* SCHAN response header bits (in SCHAN_D(0)) */
#define SCHAN_RESP_ERR        (1u << 6)
#define SCHAN_RESP_NACK       (1u << 0)

#define SCHAN_POLL_MS         50

/* Chip reset registers */
#define CMIC_CONFIG           0x010C
#define CMIC_SOFT_RESET       0x0580
#define CMIC_SOFT_RESET_2     0x057C
#define CMIC_MISC_CONTROL     0x001C
#define CMIC_SBUS_TIMEOUT     0x0200
#define CMIC_SBUS_RING_MAP_0  0x0204
#define CMIC_SBUS_RING_MAP_1  0x0208
#define CMIC_SBUS_RING_MAP_2  0x020C
#define CMIC_SBUS_RING_MAP_3  0x0210

struct nos_bde_priv {
	struct pci_dev *pdev;
	void __iomem *bar0;
	dma_addr_t dma_pbase;
	void *dma_vbase;
	size_t dma_size;
	int irq;
};

static struct nos_bde_priv *bde_priv;

/*
 * Serialize all S-Channel PIO operations.  Without this, concurrent callers
 * (e.g. nos-switchd link_state_thread + any other SCHAN user) corrupt each
 * other's MSG register writes and CTRL polls, causing spurious ETIMEDOUT.
 */
static DEFINE_MUTEX(schan_mutex);

static irqreturn_t nos_bde_irq(int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;
	/* TODO: read CMIC_CMCx_IRQ_STAT0, clear, wake S-Channel waiters */
	return IRQ_NONE;
}

/*
 * CMICe SCHAN abort: clear all stale state using byte-write protocol.
 * Each write sets or clears exactly one bit in the 32-bit SCHAN_CTRL register.
 */
static void cmice_schan_abort(void __iomem *bar0)
{
	iowrite32(CMICE_SET_ABORT, bar0 + CMICE_SCHAN_CTRL);
	udelay(100);
	iowrite32(CMICE_CLR_ABORT, bar0 + CMICE_SCHAN_CTRL);
	iowrite32(CMICE_CLR_START, bar0 + CMICE_SCHAN_CTRL);
	iowrite32(CMICE_CLR_DONE,  bar0 + CMICE_SCHAN_CTRL);
	iowrite32(CMICE_CLR_SER,   bar0 + CMICE_SCHAN_CTRL);
	iowrite32(CMICE_CLR_NAK,   bar0 + CMICE_SCHAN_CTRL);
	iowrite32(CMICE_CLR_TIMEOUT, bar0 + CMICE_SCHAN_CTRL);
}

/*
 * BCM56846 chip reset sequence.
 * Derived from OpenMDK bcm56840_a0_bmd_reset.c and experimental verification.
 *
 * CRITICAL ordering requirements (verified 2026-03-06):
 *   - SBUS timeout and ring maps MUST be programmed BEFORE releasing
 *     MMU/IP/EP from reset. If MMU comes out of reset before ring maps
 *     are configured, the SBUS agent enters a permanent error state
 *     (err=1 on all SCHAN ops) that persists across software resets
 *     and requires a cold VDD power cycle to clear.
 *   - Ring maps start at 0x0204 (NOT 0x0200). 0x0200 is CMIC_SBUS_TIMEOUT.
 *     Verified experimentally: writing ring map data to 0x200 causes all
 *     blocks to HANG (huge timeout value interpreted as timeout register).
 *
 * Sequence:
 *   1. CPS reset via CMIC_CONFIG bit 5
 *   2. CMIC_SOFT_RESET = 0 (all blocks in reset)
 *   3. SBUS timeout at 0x0200, ring maps at 0x0204-0x0210
 *   4. Progressive release: PLLs -> PLL-post -> PGs+TempMon
 *   5. CMIC_SOFT_RESET_2 (XQ arbiter release)
 *   6. Release IP/EP/MMU LAST (0xFFFF)
 *   7. MISC_CONTROL bit 0 (SCHAN enable) + SCHAN abort
 */
static void bcm56846_chip_reset(void __iomem *bar0)
{
	u32 val;

	/* Step 1: CPS Reset — toggle bit 5 of CMIC_CONFIG */
	val = ioread32(bar0 + CMIC_CONFIG);
	iowrite32(val | (1u << 5), bar0 + CMIC_CONFIG);
	mdelay(1);
	iowrite32(val & ~(1u << 5), bar0 + CMIC_CONFIG);
	mdelay(10);

	/* Step 2: All blocks in reset */
	iowrite32(0x00000000, bar0 + CMIC_SOFT_RESET);
	mdelay(50);

	/*
	 * Step 3: SBUS timeout + ring maps BEFORE any blocks leave reset.
	 *
	 * Ring map nibble values: ring 0=CMIC, 1=IPIPE, 2=MMU, 3=EPIPE,
	 * 4=XLPORT, 5=CLPORT. Agent-to-ring mapping from CDK:
	 *   agents  0-7:  0x43052100 (CMIC,IPIPE,MMU,EPIPE,PG,CMIC,PG,EPIPE)
	 *   agents  8-15: 0x33333343 (EPIPE*6,PG,EPIPE)
	 *   agents 16-23: 0x44444333 (XLPORT*5,EPIPE*3)
	 *   agents 24-31: 0x00034444 (0,0,MMU,XLPORT*4)
	 */
	iowrite32(0x000007D0, bar0 + CMIC_SBUS_TIMEOUT);   /* 2000 cycles */
	iowrite32(0x43052100, bar0 + CMIC_SBUS_RING_MAP_0); /* agents  0-7  */
	iowrite32(0x33333343, bar0 + CMIC_SBUS_RING_MAP_1); /* agents  8-15 */
	iowrite32(0x44444333, bar0 + CMIC_SBUS_RING_MAP_2); /* agents 16-23 */
	iowrite32(0x00034444, bar0 + CMIC_SBUS_RING_MAP_3); /* agents 24-31 */

	/* Step 4: Progressive release — PLLs first, then port groups */
	iowrite32(0x00000780, bar0 + CMIC_SOFT_RESET);	/* PLLs out of reset */
	mdelay(100);
	iowrite32(0x0000F780, bar0 + CMIC_SOFT_RESET);	/* PLL post-dividers */
	mdelay(50);
	iowrite32(0x0000FF8F, bar0 + CMIC_SOFT_RESET);	/* PGs + temp monitor */
	mdelay(50);

	/* Step 5: CMIC_SOFT_RESET_2 — XQ arbiter release */
	iowrite32(0x0000007F, bar0 + CMIC_SOFT_RESET_2);
	mdelay(10);

	/* Step 6: Release IP/EP/MMU LAST — bits 4(MMU), 5(IP), 6(EP) */
	iowrite32(0x0000FFFF, bar0 + CMIC_SOFT_RESET);
	mdelay(100);

	/* Step 7: MISC_CONTROL bit 0 — enable SCHAN */
	val = ioread32(bar0 + CMIC_MISC_CONTROL);
	iowrite32(val | 1u, bar0 + CMIC_MISC_CONTROL);

	/* Abort any stale SCHAN op */
	cmice_schan_abort(bar0);

	pr_info("nos-bde: chip reset complete"
		" SOFT_RESET=0x%08x CTRL=0x%08x\n",
		ioread32(bar0 + CMIC_SOFT_RESET),
		ioread32(bar0 + CMICE_SCHAN_CTRL));
}

static int nos_bde_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rc;
	struct nos_bde_priv *priv;

	(void)id;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->pdev = pdev;

	rc = pci_enable_device(pdev);
	if (rc)
		goto err_free;
	pci_set_master(pdev);

	rc = pci_request_regions(pdev, "nos-kernel-bde");
	if (rc)
		goto err_disable;

	priv->bar0 = pci_iomap(pdev, 0, BAR0_SIZE);
	if (!priv->bar0) {
		rc = -ENOMEM;
		goto err_release;
	}

	priv->dma_size = DMA_POOL_SIZE;
	priv->dma_vbase = dma_alloc_coherent(&pdev->dev, priv->dma_size,
					     &priv->dma_pbase, GFP_KERNEL);
	if (!priv->dma_vbase) {
		rc = -ENOMEM;
		goto err_iounmap;
	}

	priv->irq = pdev->irq;
	if (request_irq(priv->irq, nos_bde_irq, IRQF_SHARED, "nos-bde", priv)) {
		rc = -EIO;
		goto err_dma;
	}

	pci_set_drvdata(pdev, priv);
	bde_priv = priv;
	pr_info("nos-kernel-bde: BCM56846 at %pR, BAR0 %p, DMA %pad size %zu\n",
		&pdev->resource[0], priv->bar0, &priv->dma_pbase, priv->dma_size);

	/* Full BCM56846 chip reset: CPS, soft-reset, ring maps, SCHAN clear */
	bcm56846_chip_reset(priv->bar0);

	return 0;

err_dma:
	dma_free_coherent(&pdev->dev, priv->dma_size, priv->dma_vbase, priv->dma_pbase);
err_iounmap:
	pci_iounmap(pdev, priv->bar0);
err_release:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(priv);
	return rc;
}

static void nos_bde_remove(struct pci_dev *pdev)
{
	struct nos_bde_priv *priv = pci_get_drvdata(pdev);

	if (!priv)
		return;
	bde_priv = NULL;
	free_irq(priv->irq, priv);
	pci_set_drvdata(pdev, NULL);
	dma_free_coherent(&pdev->dev, priv->dma_size, priv->dma_vbase, priv->dma_pbase);
	pci_iounmap(pdev, priv->bar0);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(priv);
}

static const struct pci_device_id nos_bde_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, PCI_DEVICE_ID_BCM56846) },
	{ }
};
MODULE_DEVICE_TABLE(pci, nos_bde_pci_tbl);

static struct pci_driver nos_bde_driver = {
	.name	 = "nos-kernel-bde",
	.id_table = nos_bde_pci_tbl,
	.probe	 = nos_bde_probe,
	.remove	 = nos_bde_remove,
};

module_pci_driver(nos_bde_driver);

/* Exported for nos_user_bde.ko */
void __iomem *nos_bde_get_bar0(void)
{
	return bde_priv ? bde_priv->bar0 : NULL;
}
EXPORT_SYMBOL(nos_bde_get_bar0);

dma_addr_t nos_bde_get_dma_pbase(void)
{
	return bde_priv ? bde_priv->dma_pbase : 0;
}
EXPORT_SYMBOL(nos_bde_get_dma_pbase);

size_t nos_bde_get_dma_size(void)
{
	return bde_priv ? bde_priv->dma_size : 0;
}
EXPORT_SYMBOL(nos_bde_get_dma_size);

/*
 * S-Channel PIO op via CMICe interface on BCM56846 (Trident+).
 *
 * Protocol:
 *   1. Abort any stale SCHAN state (byte-write SET/CLR on SCHAN_CTRL at 0x50).
 *   2. Write cmd[0..cmd_words-1] to SCHAN_D(0..cmd_words-1) at BAR0+0x0000.
 *   3. Write data[0..data_words-1] to SCHAN_D(cmd_words..) (for write ops).
 *   4. Write CMICE_SET_START (0x80) to SCHAN_CTRL to trigger the operation.
 *   5. Poll SCHAN_CTRL for DONE bit (bit 1), check TIMEOUT/NAK.
 *   6. Read response from SCHAN_D(0..data_words-1).
 *
 * SCHAN_D registers at 0x0000 overlap CMIC config space (by design in CMICe).
 * SCHAN_CTRL at 0x0050 = SCHAN_D(20) — never overwritten by normal ops.
 */
int nos_bde_schan_op(const u32 *cmd, int cmd_words, u32 *data, int data_words, int *status)
{
	struct nos_bde_priv *p = bde_priv;
	void __iomem *bar0;
	int i;
	unsigned long deadline;
	u32 ctrl, saved_misc;

	if (!p || cmd_words <= 0 || cmd_words > SCHAN_MAX_MSG_WORDS ||
	    data_words < 0 || data_words > SCHAN_MAX_MSG_WORDS)
		return -EINVAL;

	if (mutex_lock_interruptible(&schan_mutex))
		return -EINTR;

	bar0 = p->bar0;
	*status = -1;

	/*
	 * Save MISC_CONTROL before SCHAN op.  SCHAN_D(7) at offset 0x1C
	 * overlaps CMIC_MISC_CONTROL — SCHAN responses overwrite it,
	 * clearing bit 0 (SCHAN enable) and breaking all subsequent ops.
	 */
	saved_misc = ioread32(bar0 + CMIC_MISC_CONTROL);

	/* Abort any stale SCHAN state (CMICe byte-write protocol) */
	ctrl = ioread32(bar0 + CMICE_SCHAN_CTRL);
	if (ctrl != 0)
		cmice_schan_abort(bar0);

	/* Write command words to SCHAN_D(0..) */
	for (i = 0; i < cmd_words; i++)
		iowrite32(cmd[i], bar0 + CMICE_SCHAN_D(i));

	/*
	 * NOTE: No pre-write of data[] here.  In CMICe, SCHAN_D(n) at offset
	 * n*4 overlaps CMIC config space (e.g. MISC_CONTROL at 0x1C = D(7)).
	 * Writing data_words beyond cmd_words would clobber critical CMIC
	 * registers before SCHAN starts.  For write ops, the caller must
	 * include the data word in cmd[] (e.g. cmd_words=3 for WRITE_REGISTER:
	 * cmd[0]=header, cmd[1]=address, cmd[2]=data_word).
	 */

	/* Restore MISC_CONTROL after writing cmd words (cmd may have clobbered it) */
	if (cmd_words > 7)
		iowrite32(saved_misc, bar0 + CMIC_MISC_CONTROL);

	/* Assert START via CMICe byte-write */
	iowrite32(CMICE_SET_START, bar0 + CMICE_SCHAN_CTRL);

	/* Poll for DONE (bit 1) */
	deadline = jiffies + msecs_to_jiffies(SCHAN_POLL_MS);
	while (time_before(jiffies, deadline)) {
		ctrl = ioread32(bar0 + CMICE_SCHAN_CTRL);
		if (ctrl & SCHAN_CTRL_DONE) {
			if (ctrl & (SCHAN_CTRL_TIMEOUT | SCHAN_CTRL_NAK)) {
				cmice_schan_abort(bar0);
				/* Restore MISC_CONTROL after SCHAN response clobbered D regs */
				iowrite32(saved_misc, bar0 + CMIC_MISC_CONTROL);
				pr_warn_ratelimited(
				    "nos-bde: SCHAN err ctrl=0x%08x cmd[0]=0x%08x addr=0x%08x\n",
				    ctrl, cmd[0], cmd_words > 1 ? cmd[1] : 0);
				*status = -EIO;
			} else {
				/* Read response from SCHAN_D before clearing state */
				if (data && data_words > 0)
					for (i = 0; i < data_words; i++)
						data[i] = ioread32(bar0 + CMICE_SCHAN_D(i));
				cmice_schan_abort(bar0);
				/* Restore MISC_CONTROL after SCHAN response clobbered D regs */
				iowrite32(saved_misc, bar0 + CMIC_MISC_CONTROL);
				*status = 0;
			}
			goto out;
		}
		usleep_range(10, 50);
	}
	cmice_schan_abort(bar0);
	/* Restore MISC_CONTROL after timeout */
	iowrite32(saved_misc, bar0 + CMIC_MISC_CONTROL);
	pr_warn_ratelimited("nos-bde: SCHAN timeout cmd[0]=0x%08x addr=0x%08x ctrl=0x%08x\n",
			    cmd[0], cmd_words > 1 ? cmd[1] : 0, ctrl);
	*status = -ETIMEDOUT;
out:
	mutex_unlock(&schan_mutex);
	return 0;
}
EXPORT_SYMBOL(nos_bde_schan_op);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel BDE for BCM56846 (open-nos-as5610)");
MODULE_AUTHOR("open-nos-as5610");
