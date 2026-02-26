/* SOC script runner: setreg, getreg, rcload (stub). */
#include "bde_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

extern int bde_write_reg(uint32_t offset, uint32_t value);
extern int bde_read_reg(uint32_t offset, uint32_t *value);

/* Run a script file (rc.soc, rc.datapath_0, etc.). Lines: setreg <addr> <val>, getreg <addr>, rcload <path>. */
int bcm56846_soc_run(const char *script_path)
{
	FILE *f;
	char line[512];
	char cmd[64], arg1[64], arg2[64];
	unsigned int addr, val;
	uint32_t read_val;

	if (!script_path)
		return -1;
	f = fopen(script_path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		/* Trim and skip comments/empty */
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#' || *p == '\n')
			continue;
		if (sscanf(p, "%63s %63s %63s", cmd, arg1, arg2) < 1)
			continue;
		if (strcmp(cmd, "setreg") == 0) {
			if (sscanf(arg1, "0x%x", &addr) == 1) {
				val = 0;
				sscanf(arg2, "0x%x", &val);
				bde_write_reg((uint32_t)addr, (uint32_t)val);
			}
		} else if (strcmp(cmd, "getreg") == 0) {
			if (sscanf(arg1, "0x%x", &addr) == 1 && bde_read_reg((uint32_t)addr, &read_val) == 0)
				; /* could log: printf("getreg 0x%x = 0x%x\n", addr, read_val); */
		}
		/* rcload, debug, m, 0: etc. — no-op for now */
	}
	fclose(f);
	return 0;
}
