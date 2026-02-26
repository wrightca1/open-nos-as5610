/* bcm56846_attach, bcm56846_init, bcm56846_detach */
#include "bcm56846.h"
#include "bde_ioctl.h"
#include <errno.h>
#include <string.h>


static int attached;

int bcm56846_attach(int unit)
{
	(void)unit;
	if (attached)
		return 0;
	if (bde_open() < 0)
		return -1;
	if (!bde_mmap_dma()) {
		bde_close();
		return -1;
	}
	attached = 1;
	return 0;
}

void bcm56846_detach(int unit)
{
	(void)unit;
	bde_close();
	attached = 0;
}

int bcm56846_init(int unit, const char *config_path)
{
	(void)unit;
	(void)config_path;
	if (!attached)
		return -1;
	/* TODO: load config.bcm, run rc.soc, rc.ports_0, rc.datapath_0 */
	return 0;
}
