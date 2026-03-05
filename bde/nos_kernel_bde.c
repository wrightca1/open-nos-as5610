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
 * COLD-BOOT SCHAN STATE (confirmed experimentally):
 *   After a hard power cycle / reboot, SCHAN_CTRL may contain stale
 *   error state (0x65, 0x77, etc.) from the hardware power-on self-test.
 *   Cold-boot indicator: BAR0+0x158 (DMA_RING_ADDR) = 0x00000000.
 *   BAR0+0x148 reads 0x80000000 as the HARDWARE POWER-ON DEFAULT.
 *
 *   HARDWARE POWER-ON DEFAULTS (NOT DMA mode indicators):
 *     BAR0+0x10c = 0x32000043  (SCHAN DMA ring config — always present)
 *     BAR0+0x400 = 0x505b8d80  (SCHAN DMA ring head — always present)
 *   These values were previously misidentified as Cumulus DMA ring state.
 *   They are hardware defaults that exist after cold power cycle.
 *
 *   IMPORTANT: DO NOT write 0 to BAR0+0x148.  Hardware test 2026-03-04
 *   confirmed: writing 0 to BAR0+0x148 disables SCHAN PIO entirely.
 *   Its bit31 function is unknown but must remain set for SCHAN to work.
 *
 *   After bcm56846_chip_init() programs SBUS_TIMEOUT (0x200) and SBUS ring
 *   maps (0x204-0x220), SCHAN PIO operations complete via BDE ioctl:
 *     - SCHAN ops to TOP/accessible regs: ctrl=0x00000002 (DONE, no error)
 *     - SCHAN ops to XLPORT (in soft reset): ctrl=0x00000077 (ABORT+NACK)
 *       This is the expected ERROR_ABORT response from blocks in reset.
 *     - SCHAN_CTRL (0x33000) responds to START/ABORT.
 *
 *   IMPORTANT — CONCURRENT ACCESS:
 *   nos_bde_schan_op() is serialized by schan_mutex.  Without the mutex,
 *   concurrent callers (e.g. nos-switchd link_state_thread + user tests)
 *   corrupt each other's MSG register writes and CTRL polls, causing spurious
 *   ETIMEDOUT returns.
 *
 * WARM-BOOT (soft reboot from Cumulus) — NON-FUNCTIONAL:
 *   After warm reboot from Cumulus, the BCM56846 CMICm remains in
 *   SCHAN ring-buffer DMA mode (Cumulus driver configures this; state
 *   persists through warm reset).
 *
 *   Warm-boot indicator: BAR0+0x158 (DMA_RING_ADDR) = 0x0294ffd0 (or
 *   other non-zero PA = Cumulus ring buffer PA).
 *
 *   PCI reset methods DO NOT help:
 *     - FLR (Function Level Reset): NOT supported by BCM56846
 *       (Device Capabilities register bit 28 = 0)
 *     - PCIe secondary bus reset via bridge: causes momentary link loss
 *       but CMIC internal state is NOT cleared (ring maps persist).
 *
 *   Fix: cold power-cycle the switch.
 *
 * TODO: add warm-boot detection in nos_bde_probe():
 *   if (readl(bar0 + 0x158) != 0) warn that cold power-cycle is required.
 */
#define CMIC_CMC0_SCHAN_CTRL  0x33000
#define CMIC_CMC0_SCHAN_MSG(n)  (0x3300c + (n) * 4)
#define SCHAN_MAX_MSG_WORDS     21

/* CMIC_CMC0_SCHAN_CTRL bit fields (CMICm spec) */
#define SCHAN_CTRL_START        (1u << 0)
#define SCHAN_CTRL_DONE         (1u << 1)
#define SCHAN_CTRL_ABORT        (1u << 2)   /* write 1 to abort stale op; also ERROR_ABORT when set by HW */
#define SCHAN_CTRL_NACK         (1u << 3)   /* HW sets on SBUS NACK (agent not present, block in reset) */
#define SCHAN_CTRL_ERR_MASK     ((1u << 2) | (1u << 3))  /* ERROR_ABORT + NACK */
/*
 * SCHAN completion mask: exit poll on DONE *or* any error bit.
 * CMICm sets DONE=1 on success; on SBUS timeout/NACK it may only set
 * NACK (bit 3) without DONE (bit 1).  Polling only for DONE causes a
 * full 50ms wait on every failed command.  Exit on any completion signal.
 */
#define SCHAN_CTRL_COMPLETE     (SCHAN_CTRL_DONE | SCHAN_CTRL_ERR_MASK)

#define SCHAN_POLL_MS    50   /* 50ms: SBUS_TIMEOUT=2000 cyc (~10us), 50ms >> enough */

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

	/*
	 * SCHAN diagnostics and PIO initialization.
	 *
	 * Log key CMICm registers to help diagnose SCHAN mode (PIO vs DMA).
	 * Then attempt to clear stale SCHAN state so PIO is ready before
	 * any userspace SCHAN_OP ioctl.
	 *
	 * Hardware power-on defaults (NOT indicators of DMA ring mode):
	 *   0x10c = 0x32000043 (SCHAN DMA ring config)
	 *   0x400 = 0x505b8d80 (SCHAN DMA ring head pointer)
	 *   0x148 = 0x80000000 (CMIC_DMA_CFG — DO NOT WRITE)
	 *
	 * Cumulus warm-boot indicator:
	 *   0x158 = non-zero (Cumulus ring buffer PA, e.g. 0x0294ffd0)
	 *   0x158 = 0 after cold boot (PERST_N clears this)
	 */
	{
		void __iomem *bar0 = priv->bar0;
		u32 schan_ctrl, ring_cfg, ring_addr, ring_head, dma_cfg;

		schan_ctrl = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
		ring_cfg   = ioread32(bar0 + 0x10c);
		ring_addr  = ioread32(bar0 + 0x158);
		ring_head  = ioread32(bar0 + 0x400);
		dma_cfg    = ioread32(bar0 + 0x148);

		pr_info("nos-bde: SCHAN diag: ctrl=0x%08x ring_cfg=0x%08x"
			" ring_addr=0x%08x ring_head=0x%08x dma_cfg=0x%08x\n",
			schan_ctrl, ring_cfg, ring_addr, ring_head, dma_cfg);

		/* Attempt PIO SCHAN clear: abort stale ops + W1C all errors */
		if (schan_ctrl != 0) {
			u32 cleared;
			iowrite32(0xFEu, bar0 + CMIC_CMC0_SCHAN_CTRL);
			mdelay(10);
			cleared = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
			if (cleared == 0)
				pr_info("nos-bde: SCHAN PIO cleared OK"
					" (was 0x%08x, now 0x00)\n", schan_ctrl);
			else
				pr_warn("nos-bde: SCHAN PIO clear incomplete"
					" (was 0x%08x, now 0x%08x)\n",
					schan_ctrl, cleared);
		} else {
			pr_info("nos-bde: SCHAN_CTRL already 0x00 -- PIO ready\n");
		}
	}

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
	int i, total, ret;
	unsigned long timeout;
	u32 ctrl;

	if (!p || cmd_words <= 0 || cmd_words > 8 || data_words < 0 || data_words > 16)
		return -EINVAL;

	total = cmd_words + data_words;
	if (total > SCHAN_MAX_MSG_WORDS)
		return -EINVAL;

	if (mutex_lock_interruptible(&schan_mutex))
		return -EINTR;

	bar0 = p->bar0;
	*status = -1;
	ret = 0;

	/*
	 * Clear any stale SCHAN state before issuing a new operation.
	 *
	 * CMICm SCHAN_CTRL error/status bits are W1C (write-1-to-clear); writing
	 * 0 has no effect.  The ABORT bit (bit 2) triggers an in-flight op
	 * abort when asserted by the host.
	 *
	 * If stale state is present, write 0xFE (bits 1-7, all except START=0):
	 *   bit 2 (ABORT):   trigger abort of any pending SCHAN op.
	 *   bits 1,3-7:      W1C clears DONE and all error/status flags.
	 *
	 * Do NOT use "ctrl | SCHAN_CTRL_ABORT" — if bit 2 is already 1 in ctrl
	 * (hardware-set ERROR state, e.g. cold-boot 0x65), OR-ing adds nothing
	 * and the abort is never re-triggered.  Writing a fixed 0xFE always
	 * freshly asserts ABORT regardless of the current ctrl value.
	 */
	ctrl = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
	if (ctrl != 0) {
		unsigned long abort_timeout = jiffies + msecs_to_jiffies(20);

		iowrite32(0xFEu, bar0 + CMIC_CMC0_SCHAN_CTRL);
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

	/* Poll for completion: DONE or any error bit (NACK/ERROR_ABORT).
	 * CMICm may set NACK without DONE on SBUS timeout (e.g. block in reset).
	 * Exiting on SCHAN_CTRL_COMPLETE prevents 50ms stall per bad command.
	 */
	timeout = jiffies + msecs_to_jiffies(SCHAN_POLL_MS);
	while (time_before(jiffies, timeout)) {
		ctrl = ioread32(bar0 + CMIC_CMC0_SCHAN_CTRL);
		if (ctrl & SCHAN_CTRL_COMPLETE) {
			iowrite32(0, bar0 + CMIC_CMC0_SCHAN_CTRL);
			if (ctrl & SCHAN_CTRL_ERR_MASK) {
				pr_warn_ratelimited(
				    "nos-bde: SCHAN err ctrl=0x%08x cmd[0]=0x%08x addr=0x%08x\n",
				    ctrl, cmd[0], cmd_words > 1 ? cmd[1] : 0);
				*status = -EIO;
			} else {
				/* Read result back (for read ops) */
				if (data && data_words > 0)
					for (i = 0; i < data_words; i++)
						data[i] = ioread32(bar0 + CMIC_CMC0_SCHAN_MSG(i));
				*status = 0;
			}
			goto out;
		}
		usleep_range(10, 50);
	}
	iowrite32(0, bar0 + CMIC_CMC0_SCHAN_CTRL);
	pr_warn_ratelimited("nos-bde: SCHAN timeout cmd[0]=0x%08x addr=0x%08x ctrl=0x%08x\n",
			    cmd[0], cmd_words > 1 ? cmd[1] : 0, ctrl);
	*status = -ETIMEDOUT;
out:
	mutex_unlock(&schan_mutex);
	return ret;
}
EXPORT_SYMBOL(nos_bde_schan_op);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel BDE for BCM56846 (open-nos-as5610)");
MODULE_AUTHOR("open-nos-as5610");
