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
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#define PCI_VENDOR_ID_BROADCOM	0x14e4
#define PCI_DEVICE_ID_BCM56846	0xb846

#define BAR0_SIZE	(256 * 1024)
#define DMA_POOL_SIZE	(4 * 1024 * 1024)	/* max order-10 alloc on PPC32/mpc85xx */

/* CMICm offsets from BAR0 (RE: ASIC_INIT_AND_DMA_MAP, SCHAN_AND_RING_BUFFERS) */
#define CMICM_DMA_CTRL(ch)    (0x31140 + 4 * (ch))
#define CMICM_DMA_DESC0(ch)   (0x31158 + 4 * (ch))
#define CMICM_DMA_STAT        0x31150
/*
 * S-Channel PIO — CMC2 is the channel libopennsl uses on BCM56846.
 *
 * Confirmed from hardware BAR0 register dump and libopennsl binary strings:
 *   "S-bus PIO Message Register Set; PCI offset from: 0x3300c to: 0x33060"
 *   SCHAN_CTRL at 0x33000 (CMC2_BASE + 0x0)
 *   SCHAN_MSG  at 0x3300c (CMC2_BASE + 0x00c, through 0x33060 = MSG20)
 *
 * The switchd binary contains 4 MSG sets (0x3100c/CMC0, 0x3200c/CMC1,
 * 0x3300c/CMC2, 0x1000c/alias).  libopennsl uses the 0x3300c set = CMC2.
 *
 * IMPORTANT — WHY SCHAN IS CURRENTLY NON-FUNCTIONAL:
 *   After warm reboot from Cumulus, the BCM56846 CMC2 is still in
 *   SCHAN ring-buffer DMA mode (configured by the Cumulus BCM SDK).
 *   In ring-buffer mode, direct PIO writes to SCHAN_CTRL (0x33000) are
 *   non-writable from PCIe — the CMIC hardware controls 0x33000 internally.
 *   The other CMC addresses (0x31000, 0x32000, 0x32800) ARE writable from
 *   PCIe but are not the active SCHAN CMC, so writing START has no effect.
 *
 *   Fix: perform a cold power-cycle of the switch so the BCM56846 resets
 *   to its factory default (PIO mode).  After a cold boot, SCHAN_CTRL at
 *   0x33000 will accept writes and PIO operations will complete normally.
 *   Ring maps and SBUS_TIMEOUT must be re-programmed on each cold boot.
 *
 *   Alternative (not yet implemented): implement SCHAN ring-buffer DMA mode
 *   in nos_bde_schan_op() so we can use the existing Cumulus-era CMIC config
 *   without a cold power cycle.
 */
#define CMIC_CMC0_SCHAN_CTRL  0x33000
#define CMIC_CMC0_SCHAN_MSG(n)  (0x3300c + (n) * 4)
#define SCHAN_MAX_MSG_WORDS     21

/* CMIC_CMC0_SCHAN_CTRL bit fields (CMICm spec) */
#define SCHAN_CTRL_START        (1u << 0)
#define SCHAN_CTRL_DONE         (1u << 1)
#define SCHAN_CTRL_ABORT        (1u << 2)   /* write 1 to abort stale op; also ERROR status when set by HW */
#define SCHAN_CTRL_ERR_MASK     ((1u << 2) | (1u << 3))  /* ERROR_ABORT + NACK */

#define SCHAN_POLL_MS    500

struct nos_bde_priv {
	struct pci_dev *pdev;
	void __iomem *bar0;
	dma_addr_t dma_pbase;
	void *dma_vbase;
	size_t dma_size;
	int irq;
};

static struct nos_bde_priv *bde_priv;

static irqreturn_t nos_bde_irq(int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;
	/* TODO: read CMIC_CMCx_IRQ_STAT0, clear, wake S-Channel waiters */
	return IRQ_NONE;
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
 * S-Channel PIO op via SCHAN_MSG registers (CMICm protocol).
 *
 * Protocol:
 *   1. Write cmd[0..cmd_words-1] to CMIC_CMC0_SCHAN_MSG(0..cmd_words-1).
 *   2. Write data[0..data_words-1] to CMIC_CMC0_SCHAN_MSG(cmd_words..).
 *   3. Write SCHAN_CTRL_START to CMIC_CMC0_SCHAN_CTRL.
 *   4. Poll CMIC_CMC0_SCHAN_CTRL for SCHAN_CTRL_DONE.
 *   5. Clear SCHAN_CTRL (write 0).
 *   6. Read result from CMIC_CMC0_SCHAN_MSG(0..data_words-1).
 *
 * SCHAN_MSG base address 0x3300c confirmed from Broadcom binary:
 *   "S-bus PIO Message Register Set; PCI offset from: 0x3300c to: 0x33060"
 */
int nos_bde_schan_op(const u32 *cmd, int cmd_words, u32 *data, int data_words, int *status)
{
	struct nos_bde_priv *p = bde_priv;
	void __iomem *bar0;
	int i, total;
	unsigned long timeout;
	u32 ctrl;

	if (!p || cmd_words <= 0 || cmd_words > 8 || data_words < 0 || data_words > 16)
		return -EINVAL;

	total = cmd_words + data_words;
	if (total > SCHAN_MAX_MSG_WORDS)
		return -EINVAL;

	bar0 = p->bar0;
	*status = -1;

	/*
	 * Clear any stale SCHAN state. If CTRL is non-zero (DONE or START stuck
	 * from prior op / warm reboot), use ABORT to force-clear, then write 0.
	 * Per SCHAN_AND_RING_BUFFERS.md: protocol requires "write 0 → SCHAN_CTRL
	 * [clear any stale state]" before "write START".
	 */
	ctrl = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
	if (ctrl != 0) {
		unsigned long abort_timeout = jiffies + msecs_to_jiffies(20);

		iowrite32(ctrl | SCHAN_CTRL_ABORT, bar0 + CMIC_CMC0_SCHAN_CTRL);
		while (time_before(jiffies, abort_timeout)) {
			ctrl = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
			if (ctrl == 0)
				break;
			usleep_range(50, 200);
		}
		iowrite32(0, bar0 + CMIC_CMC0_SCHAN_CTRL);
	}

	/* Write command words to SCHAN_MSG FIFO */
	for (i = 0; i < cmd_words; i++)
		iowrite32(cmd[i], bar0 + CMIC_CMC0_SCHAN_MSG(i));

	/* Write data words (for write ops) */
	if (data && data_words > 0)
		for (i = 0; i < data_words; i++)
			iowrite32(data[i], bar0 + CMIC_CMC0_SCHAN_MSG(cmd_words + i));

	/* Protocol step 3: write 0 before START (clear any residual) */
	iowrite32(0, bar0 + CMIC_CMC0_SCHAN_CTRL);

	/* Protocol step 4: assert START to trigger operation */
	iowrite32(SCHAN_CTRL_START, bar0 + CMIC_CMC0_SCHAN_CTRL);

	/* Poll for done */
	timeout = jiffies + msecs_to_jiffies(SCHAN_POLL_MS);
	while (time_before(jiffies, timeout)) {
		ctrl = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
		if (ctrl & SCHAN_CTRL_DONE) {
			iowrite32(0, bar0 + CMIC_CMC0_SCHAN_CTRL);
			if (ctrl & SCHAN_CTRL_ERR_MASK) {
				pr_warn("nos-bde: SCHAN error ctrl=0x%08x\n", ctrl);
				*status = -EIO;
			} else {
				/* Read result back (for read ops) */
				if (data && data_words > 0)
					for (i = 0; i < data_words; i++)
						data[i] = ioread32(bar0 + CMIC_CMC0_SCHAN_MSG(i));
				*status = 0;
			}
			return 0;
		}
		usleep_range(10, 100);
	}
	iowrite32(0, bar0 + CMIC_CMC0_SCHAN_CTRL);
	pr_warn("nos-bde: SCHAN timeout cmd[0]=0x%08x\n", cmd[0]);
	*status = -ETIMEDOUT;
	return 0;
}
EXPORT_SYMBOL(nos_bde_schan_op);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel BDE for BCM56846 (open-nos-as5610)");
MODULE_AUTHOR("open-nos-as5610");
