/* bcm56846_attach, bcm56846_init, bcm56846_detach */
#include "bcm56846.h"
#include "bde_ioctl.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

extern int bcm56846_config_load(const char *path);
extern int bcm56846_soc_run(const char *script_path);


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
	char soc_path[512];
	size_t dlen;

	(void)unit;
	if (!attached)
		return -1;
	if (bcm56846_config_load(config_path) < 0)
		return -1; /* config.bcm load failed */

	/* Run rc.soc and rc.datapath_0 if present (base = config_path without trailing /) */
	dlen = strlen(config_path);
	if (dlen > 0 && config_path[dlen-1] == '/')
		dlen--;
	if (dlen > 0) {
		snprintf(soc_path, sizeof(soc_path), "%.*s/rc.soc", (int)dlen, config_path);
		bcm56846_soc_run(soc_path);
		snprintf(soc_path, sizeof(soc_path), "%.*s/rc.datapath_0", (int)dlen, config_path);
		bcm56846_soc_run(soc_path);
	}
	return 0;
}
