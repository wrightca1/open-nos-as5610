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
#define DMA_POOL_SIZE	(8 * 1024 * 1024)

/* CMICm offsets from BAR0 (RE: ASIC_INIT_AND_DMA_MAP, SCHAN_AND_RING_BUFFERS) */
#define CMICM_DMA_CTRL(ch)    (0x31140 + 4 * (ch))
#define CMICM_DMA_DESC0(ch)   (0x31158 + 4 * (ch))
#define CMICM_DMA_STAT        0x31150
#define CMIC_CMC0_SCHAN_CTRL  0x32800

/* S-Channel uses a dedicated DMA buffer; max 8 cmd + 16 data words */
#define SCHAN_BUF_WORDS  32
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

/* S-Channel op: copy cmd/data to DMA buffer, kick channel 0, poll for completion.
 * Caller (nos_user_bde) passes cmd[8], data[16], len; result copied back to data[].
 * Buffer layout at DMA base: first SCHAN_BUF_WORDS words = command + data in/out.
 */
int nos_bde_schan_op(const u32 *cmd, int cmd_words, u32 *data, int data_words, int *status)
{
	struct nos_bde_priv *p = bde_priv;
	void __iomem *bar0;
	u32 *buf;
	int i;
	unsigned long timeout;

	if (!p || cmd_words <= 0 || cmd_words > 8 || data_words > 16)
		return -EINVAL;
	bar0 = p->bar0;
	buf = (u32 *)p->dma_vbase;
	*status = -1;

	memcpy(buf, cmd, sizeof(u32) * cmd_words);
	if (data && data_words > 0)
		memcpy(buf + cmd_words, data, sizeof(u32) * data_words);

	/* Point channel 0 at our buffer and start (RE: CMICM_DMA_DESC0, CMICM_DMA_CTRL) */
	iowrite32((u32)(p->dma_pbase & 0xffffffff), bar0 + CMICM_DMA_DESC0(0));
	iowrite32(1, bar0 + CMICM_DMA_CTRL(0));

	timeout = jiffies + msecs_to_jiffies(SCHAN_POLL_MS);
	while (time_before(jiffies, timeout)) {
		if (ioread32(bar0 + CMICM_DMA_STAT) & 1) {
			if (data && data_words > 0)
				memcpy(data, buf + cmd_words, sizeof(u32) * data_words);
			*status = 0;
			return 0;
		}
		msleep(1);
	}
	*status = -ETIMEDOUT;
	return 0;
}
EXPORT_SYMBOL(nos_bde_schan_op);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel BDE for BCM56846 (open-nos-as5610)");
MODULE_AUTHOR("open-nos-as5610");
