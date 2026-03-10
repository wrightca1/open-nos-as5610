/*
 * cpld-ctl: Read/write CPLD registers on AS5610-52X via /dev/mem.
 * CPLD is memory-mapped at 0xEA000000 (eLBC chip select 1).
 *
 * Usage:
 *   cpld-ctl               # dump status
 *   cpld-ctl read REG      # read register (hex)
 *   cpld-ctl write REG VAL # write register (hex)
 *   cpld-ctl fan PCT       # set fan to percentage (0-100)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define CPLD_BASE  0xEA000000
#define CPLD_SIZE  4096

/* CPLD register offsets */
#define REG_PSU2_STATUS   0x01
#define REG_PSU1_STATUS   0x02
#define REG_SYS_STATUS    0x03
#define REG_FAN_SPEED     0x0D
#define REG_LED            0x13
#define REG_LED_LOC        0x15

static volatile uint8_t *cpld;

static int cpld_open(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return -1;
	}
	cpld = mmap(NULL, CPLD_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, CPLD_BASE);
	close(fd);
	if (cpld == MAP_FAILED) {
		perror("mmap CPLD");
		return -1;
	}
	return 0;
}

static void dump_status(void)
{
	uint8_t fan_pwm = cpld[REG_FAN_SPEED] & 0x1F;
	uint8_t sys = cpld[REG_SYS_STATUS];
	uint8_t psu1 = cpld[REG_PSU1_STATUS];
	uint8_t psu2 = cpld[REG_PSU2_STATUS];

	printf("CPLD Registers:\n");
	printf("  PSU2 status (0x01): 0x%02x\n", psu2);
	printf("  PSU1 status (0x02): 0x%02x\n", psu1);
	printf("  System status (0x03): 0x%02x\n", sys);
	printf("    Fan present: %s\n", (sys & 0x04) ? "NO" : "yes");
	printf("    Fan OK:      %s\n", (sys & 0x08) ? "FAILED" : "yes");
	printf("    Fan airflow: %s\n", (sys & 0x10) ? "B2F" : "F2B");
	printf("  Fan PWM (0x0D): raw=%d  duty=%d%%\n",
	       fan_pwm, fan_pwm * 100 / 31);
	printf("  LED (0x13): 0x%02x\n", cpld[REG_LED]);
	printf("  LED LOC (0x15): 0x%02x\n", cpld[REG_LED_LOC]);

	/* Dump first 32 registers */
	printf("\nRaw register dump (0x00-0x1F):\n");
	for (int i = 0; i < 32; i++) {
		if (i % 16 == 0)
			printf("  %02x:", i);
		printf(" %02x", cpld[i]);
		if (i % 16 == 15)
			printf("\n");
	}
}

int main(int argc, char *argv[])
{
	if (cpld_open() < 0)
		return 1;

	if (argc < 2) {
		dump_status();
		return 0;
	}

	if (strcmp(argv[1], "read") == 0 && argc >= 3) {
		unsigned reg = strtoul(argv[2], NULL, 16);
		printf("CPLD[0x%02x] = 0x%02x\n", reg, cpld[reg]);
		return 0;
	}

	if (strcmp(argv[1], "write") == 0 && argc >= 4) {
		unsigned reg = strtoul(argv[2], NULL, 16);
		unsigned val = strtoul(argv[3], NULL, 16);
		cpld[reg] = (uint8_t)val;
		printf("CPLD[0x%02x] <- 0x%02x (readback: 0x%02x)\n",
		       reg, val, cpld[reg]);
		return 0;
	}

	if (strcmp(argv[1], "fan") == 0 && argc >= 3) {
		int pct = atoi(argv[2]);
		if (pct < 0) pct = 0;
		if (pct > 100) pct = 100;
		uint8_t raw = (uint8_t)(pct * 31 / 100);
		if (pct > 0 && raw == 0) raw = 1; /* don't turn off if > 0% */
		cpld[REG_FAN_SPEED] = raw;
		printf("Fan set to %d%% (raw=%d, readback=0x%02x)\n",
		       pct, raw, cpld[REG_FAN_SPEED]);
		return 0;
	}

	fprintf(stderr, "Usage: %s [read REG | write REG VAL | fan PCT]\n",
		argv[0]);
	return 1;
}
