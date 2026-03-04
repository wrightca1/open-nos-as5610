/* SOC script runner: handles commands from rc.soc, rc.ports_0, etc.
 *
 * Supported commands:
 *   setreg 0xADDR 0xVAL     — write register via BDE
 *   setreg REGNAME 0xVAL    — write named register (lookup table)
 *   getreg 0xADDR           — read register (result ignored in scripts)
 *   rcload PATH             — recursively run another script
 *   init all                — call bcm56846_chip_init() for CMICm bringup
 *   attach *                — no-op (SDK-specific unit attach)
 *   0:                      — no-op (unit selection prefix)
 *   debug -FLAG             — no-op (SDK debug flags)
 *   m REGNAME FIELD=VAL     — read-modify-write named register field
 *   s REGNAME FIELD=VAL     — no-op (named register field set)
 *   setenv NAME VALUE       — no-op (SDK environment variable)
 *   modreg REGNAME FIELD=V  — no-op (alternate field modify syntax)
 *   #                       — comment (skipped)
 *
 * Named register table covers the critical registers used in Cumulus rc.soc.
 * Unknown named registers produce a warning but do not abort the script.
 */
#include "bde_ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

extern int bde_write_reg(uint32_t offset, uint32_t value);
extern int bde_read_reg(uint32_t offset, uint32_t *value);
extern int bcm56846_chip_init(int unit);

/* Named register lookup table for BCM56846 (Trident+).
 * Addresses from BCM SDK RE and Broadcom documentation.
 * Only registers actually referenced in Cumulus rc.soc / rc.forwarding are included.
 */
struct reg_entry {
	const char *name;
	uint32_t    addr;
};

/* Named register field table for 'm' (read-modify-write) command.
 * {register_name, field_name, bit_shift, field_mask_unshifted}
 * All names lowercase — lookup normalises both sides before compare.
 */
struct field_entry {
	const char *reg_name;
	const char *field_name;
	uint8_t     shift;
	uint32_t    mask;
};

static const struct field_entry field_table[] = {
	/* CMIC_MISC_CONTROL (BAR0+0x1c) */
	{ "cmic_misc_control", "link40g_enable",  0u, 0x1u },
	/* Sentinel */
	{ NULL, NULL, 0u, 0u }
};

static const struct reg_entry reg_table[] = {
	/* Debug counters (RX) — IPIPE registers */
	{ "rdbgc0_select",              0x06500380u },
	{ "rdbgc3_select",              0x065003a0u },
	{ "rdbgc4_select",              0x065003a4u },
	{ "rdbgc5_select",              0x065003a8u },
	{ "rdbgc6_select",              0x065003acu },
	/* Debug counters (TX) */
	{ "tdbgc6_select",              0x04b00200u },
	/* IFP meter parity control */
	{ "ifp_meter_parity_control",   0x0a400000u },
	/* XMAC TX control — written per port block; use block 0 as representative */
	{ "xmac_tx_ctrl",               0x40a0082cu },
	/* Hash / ECMP */
	{ "rtag7_hash_seed_a",          0x05e00180u },
	{ "rtag7_hash_ecmp",            0x05e00200u },
	/* CMIC misc */
	{ "cmic_misc_control",          0x0000001cu },
	/* Sentinel */
	{ NULL, 0u }
};

static uint32_t reg_lookup(const char *name)
{
	const struct reg_entry *e;
	char lower[64];
	size_t i;

	/* Case-insensitive compare */
	for (i = 0; i < sizeof(lower) - 1 && name[i]; i++)
		lower[i] = (char)tolower((unsigned char)name[i]);
	lower[i] = '\0';

	for (e = reg_table; e->name; e++) {
		if (strcmp(lower, e->name) == 0)
			return e->addr;
	}
	return 0u; /* not found */
}

/* Recursive depth limit to prevent infinite rcload loops */
#define RCLOAD_MAX_DEPTH 8
static int rcload_depth;

static int soc_run_internal(const char *script_path);

/* Run a single script file. Returns 0 on success; -1 if file missing (non-fatal). */
static int soc_run_internal(const char *script_path)
{
	FILE *f;
	char line[512];
	char cmd[64], arg1[128], arg2[64];
	int nargs;
	unsigned int addr;
	unsigned long val;
	uint32_t read_val;

	if (!script_path)
		return -1;
	f = fopen(script_path, "r");
	if (!f)
		return -1; /* Missing script is non-fatal (e.g. /var/lib/cumulus/rc.datapath_0) */

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		/* Trim leading whitespace */
		while (*p == ' ' || *p == '\t') p++;
		/* Skip blank lines and comments */
		if (!*p || *p == '#' || *p == '\n' || *p == '\r')
			continue;

		/* Skip unit-prefix lines like "0:" */
		if (p[0] >= '0' && p[0] <= '9' && p[1] == ':') {
			p += 2;
			while (*p == ' ' || *p == '\t') p++;
			if (!*p || *p == '\n') continue;
		}

		nargs = sscanf(p, "%63s %127s %63s", cmd, arg1, arg2);
		if (nargs < 1)
			continue;

		/* debug / attach / setenv / modreg / s — silently ignore */
		if (strcmp(cmd, "debug") == 0 ||
		    strcmp(cmd, "attach") == 0 ||
		    strcmp(cmd, "setenv") == 0 ||
		    strcmp(cmd, "modreg") == 0 ||
		    strcmp(cmd, "s") == 0)
			continue;

		if (strcmp(cmd, "init") == 0 && nargs >= 2 && strcmp(arg1, "all") == 0) {
			bcm56846_chip_init(0);
			continue;
		}

		if (strcmp(cmd, "rcload") == 0 && nargs >= 2) {
			if (rcload_depth < RCLOAD_MAX_DEPTH) {
				rcload_depth++;
				soc_run_internal(arg1);
				rcload_depth--;
			}
			continue;
		}

		if (strcmp(cmd, "setreg") == 0 && nargs >= 3) {
			/* Try hex address first */
			addr = 0;
			val  = 0;
			if (sscanf(arg1, "0x%x", &addr) == 1 || sscanf(arg1, "%u", &addr) == 1) {
				if (sscanf(arg2, "0x%lx", &val) != 1)
					sscanf(arg2, "%lu", &val);
				bde_write_reg((uint32_t)addr, (uint32_t)val);
			} else {
				/* Named register */
				uint32_t reg = reg_lookup(arg1);
				if (sscanf(arg2, "0x%lx", &val) != 1)
					sscanf(arg2, "%lu", &val);
				if (reg) {
					bde_write_reg(reg, (uint32_t)val);
				} else {
					fprintf(stderr, "[soc] setreg: unknown register '%s' (skipping)\n", arg1);
				}
			}
			continue;
		}

		if (strcmp(cmd, "getreg") == 0 && nargs >= 2) {
			addr = 0;
			if (sscanf(arg1, "0x%x", &addr) == 1 || sscanf(arg1, "%u", &addr) == 1) {
				bde_read_reg((uint32_t)addr, &read_val);
			}
			continue;
		}

		if (strcmp(cmd, "m") == 0 && nargs >= 3) {
			/* m REGNAME FIELD=VALUE — read-modify-write named register field */
			uint32_t reg_addr = reg_lookup(arg1);
			char field_name[64] = {0};
			unsigned long field_val = 0;
			char lower_reg[64], lower_field[64];
			size_t fi;
			const struct field_entry *fe;

			if (reg_addr == 0u) {
				fprintf(stderr, "[soc] m: unknown register '%s' (skipping)\n",
					arg1);
				continue;
			}

			/* Parse FIELD=VALUE — try hex prefix then decimal */
			if (sscanf(arg2, "%63[^=]=0x%lx", field_name, &field_val) != 2 &&
			    sscanf(arg2, "%63[^=]=0X%lx", field_name, &field_val) != 2 &&
			    sscanf(arg2, "%63[^=]=%lu",   field_name, &field_val) != 2) {
				fprintf(stderr, "[soc] m: bad FIELD=VALUE '%s' (skipping)\n",
					arg2);
				continue;
			}

			/* Normalise register and field names to lowercase for lookup */
			for (fi = 0; fi < sizeof(lower_reg) - 1 && arg1[fi]; fi++)
				lower_reg[fi] = (char)tolower((unsigned char)arg1[fi]);
			lower_reg[fi] = '\0';
			for (fi = 0; fi < sizeof(lower_field) - 1 && field_name[fi]; fi++)
				lower_field[fi] = (char)tolower((unsigned char)field_name[fi]);
			lower_field[fi] = '\0';

			/* Look up field definition */
			for (fe = field_table; fe->reg_name; fe++) {
				if (strcmp(lower_reg, fe->reg_name) == 0 &&
				    strcmp(lower_field, fe->field_name) == 0)
					break;
			}
			if (!fe->reg_name) {
				fprintf(stderr,
					"[soc] m: unknown field '%s.%s' (skipping)\n",
					arg1, field_name);
				continue;
			}

			/* Read-modify-write */
			{
				uint32_t cur = 0u;
				bde_read_reg(reg_addr, &cur);
				cur = (cur & ~(fe->mask << fe->shift)) |
				      ((uint32_t)(field_val & fe->mask) << fe->shift);
				bde_write_reg(reg_addr, cur);
				fprintf(stderr,
					"[soc] m %s.%s=%lu -> 0x%08x\n",
					arg1, field_name, field_val, cur);
			}
			continue;
		}

		/* Unknown command — warn but keep going */
		fprintf(stderr, "[soc] unknown cmd '%s' in %s (skipping)\n", cmd, script_path);
	}
	fclose(f);
	return 0;
}

/* Public entry point */
int bcm56846_soc_run(const char *script_path)
{
	int ret;
	rcload_depth = 0;
	ret = soc_run_internal(script_path);
	if (ret < 0)
		fprintf(stderr, "[soc] script not found: %s\n", script_path);
	return ret;
}
