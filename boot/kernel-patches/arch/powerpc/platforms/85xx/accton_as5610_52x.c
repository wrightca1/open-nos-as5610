// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Accton/Edgecore AS5610-52X Platform Support
 * Linux 5.10 LTS
 *
 * Hardware: Freescale P2020 SoC (dual e500v2), Broadcom BCM56846 switching ASIC
 * Compatible with Cumulus DTB ("accton,as5610_52x") and ONIE DTB ("accton,5652")
 *
 * Based on mpc85xx_rdb.c by Freescale Semiconductor.
 *
 * To apply to a Linux 5.10 kernel tree:
 *   cp accton_as5610_52x.c $LINUX/arch/powerpc/platforms/85xx/
 *   # Add to Kconfig (before "endif # FSL_SOC_BOOKE"):
 *   #   config ACCTON_AS5610_52X
 *   #       bool "Accton/Edgecore AS5610-52X"
 *   #       select DEFAULT_UIMAGE
 *   #       help
 *   #         Support for the Accton/Edgecore AS5610-52X switching platform.
 *   # Add to Makefile:
 *   #   obj-$(CONFIG_ACCTON_AS5610_52X) += accton_as5610_52x.o
 *   # Add to .config:
 *   #   CONFIG_ACCTON_AS5610_52X=y
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/of_platform.h>

#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/pci-bridge.h>
#include <asm/prom.h>
#include <asm/udbg.h>
#include <asm/mpic.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>
#include "smp.h"
#include "mpc85xx.h"

static void __init as5610_52x_pic_init(void)
{
	struct mpic *mpic;

	mpic = mpic_alloc(NULL, 0,
			  MPIC_BIG_ENDIAN |
			  MPIC_SINGLE_DEST_CPU,
			  0, 256, " OpenPIC  ");
	BUG_ON(mpic == NULL);
	mpic_init(mpic);
}

/*
 * Setup the architecture -- mirrors mpc85xx_rdb_setup_arch() for P2020.
 * The P2020 SoC topology (eTSEC, I2C, PCIe, DUART) is fully described
 * in the DTB; of_platform_populate() (via mpc85xx_common_publish_devices)
 * handles device enumeration automatically.
 */
static void __init as5610_52x_setup_arch(void)
{
	if (ppc_md.progress)
		ppc_md.progress("as5610_52x_setup_arch()", 0);

	mpc85xx_smp_init();

	fsl_pci_assign_primary();

	printk(KERN_INFO "Accton/Edgecore AS5610-52X: PowerPC P2020 + BCM56846\n");
}

/*
 * Probe: match the compatible string from the Device Tree root node.
 *   "accton,as5610_52x" -- Cumulus-derived DTB (our boot/as5610_52x.dtb)
 *   "accton,5652"       -- ONIE kernel DTB style
 */
static int __init as5610_52x_probe(void)
{
	if (of_machine_is_compatible("accton,as5610_52x"))
		return 1;
	if (of_machine_is_compatible("accton,5652"))
		return 1;
	return 0;
}

machine_arch_initcall(as5610_52x, mpc85xx_common_publish_devices);

define_machine(as5610_52x) {
	.name			= "Accton AS5610-52X",
	.probe			= as5610_52x_probe,
	.setup_arch		= as5610_52x_setup_arch,
	.init_IRQ		= as5610_52x_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
	.pcibios_fixup_phb	= fsl_pcibios_fixup_phb,
#endif
	.get_irq		= mpic_get_irq,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};
