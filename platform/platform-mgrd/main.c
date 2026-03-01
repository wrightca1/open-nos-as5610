/*
 * platform-mgrd — AS5610-52X platform management daemon
 *
 * Responsibilities:
 *   - CPLD: PSU/fan status, LED control, watchdog keepalive
 *   - Thermal: 10 temperature sensors (ASIC + board via hwmon)
 *   - Fan PWM: thermal-driven fan speed control
 *   - SFP/QSFP: port presence + EEPROM reads via sysfs or /dev/i2c-N
 *
 * Hardware paths (verified on Cumulus AS5610-52X):
 *   CPLD:  /sys/devices/ff705000.localbus/ea000000.cpld/
 *   Temp:  /sys/class/hwmon/ (scan all hwmon*) +
 *          specific: pci0000.../0000:01:00.0/temp1_input (ASIC die)
 *                    soc.0/.../i2c-9/9-004d/temp[1..7]_input (board LM75)
 *                    soc.0/.../i2c-9/9-0018/temp[1,2]_input (NE1617A)
 *   SFP:   /sys/class/eeprom_dev/eeprom%d/device/eeprom  (N = port + 6)
 *     OR   /dev/i2c-%d (bus = 21 + port), addr 0x50
 *   QSFP:  same with bus = 21 + port (49..52)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdint.h>

/* ---- Tunables --------------------------------------------------------- */
#define POLL_INTERVAL_S     30      /* poll loop period (seconds) */
#define WDT_KEEPALIVE_S     15      /* watchdog keepalive interval */

/* Fan PWM (CPLD pwm1, scale 0–248) vs temperature thresholds (millidegrees C) */
#define TEMP_LOW_MC         35000   /* < 35 °C → low fan */
#define TEMP_MED_MC         45000   /* 35–45 °C → medium fan */
#define TEMP_HIGH_MC        55000   /* > 55 °C → max fan + log warning */
#define PWM_LOW             64
#define PWM_MED             128
#define PWM_HIGH            200
#define PWM_MAX             248

/* ---- Hardware paths --------------------------------------------------- */
#define CPLD_BASE       "/sys/devices/ff705000.localbus/ea000000.cpld"
#define EEPROM_DEV_BASE "/sys/class/eeprom_dev"
#define HWMON_BASE      "/sys/class/hwmon"

/* Number of switch ports (48 SFP+ + 4 QSFP+) */
#define NUM_SFP_PORTS   48
#define NUM_QSFP_PORTS  4
#define NUM_PORTS       (NUM_SFP_PORTS + NUM_QSFP_PORTS)

/* swpN → eeprom device index: eeprom(N+6) */
#define SFP_EEPROM_OFFSET 6
/* swpN → I2C bus: bus = 21 + N */
#define SFP_I2C_BUS_BASE  21

/* SFP EEPROM A0h byte offsets (SFF-8472) */
#define SFP_BYTE_ID         0
#define SFP_BYTE_VENDOR     20
#define SFP_BYTE_VENDOR_LEN 16
#define SFP_BYTE_PN         40
#define SFP_BYTE_PN_LEN     16
#define SFP_BYTE_SN         68
#define SFP_BYTE_SN_LEN     16
#define SFP_BYTE_DOM_CAP    60      /* bit 6: DOM supported */
#define SFP_EEPROM_SIZE     128

/* SFP identifier bytes */
#define SFP_ID_SFP          0x03
#define SFP_ID_QSFP         0x0C
#define SFP_ID_QSFP_PLUS    0x0D
#define SFP_ID_QSFP28       0x11

/* I2C_SLAVE ioctl (avoid pulling in linux/i2c-dev.h dependency) */
#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703
#endif

/* ---- Utility ---------------------------------------------------------- */

static int sysfs_read_int(const char *path, long *val)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int ok = (fscanf(f, "%ld", val) == 1);
	fclose(f);
	return ok ? 0 : -1;
}

static int sysfs_write_str(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	int ok = (fputs(val, f) >= 0);
	fclose(f);
	return ok ? 0 : -1;
}

static int sysfs_write_int(const char *path, long val)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", val);
	return sysfs_write_str(path, buf);
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void trim_trailing(char *s, size_t len)
{
	for (int i = (int)len - 1; i >= 0 && (s[i] == ' ' || s[i] == '\0'); i--)
		s[i] = '\0';
}

/* ---- CPLD ------------------------------------------------------------- */

static void cpld_watchdog_keepalive(void)
{
	static const char *path = CPLD_BASE "/watch_dog_keep_alive";
	if (sysfs_write_str(path, "1") < 0 && file_exists(CPLD_BASE)) {
		/* CPLD exists but attribute absent — might be named differently */
	}
}

static void cpld_check_psu(void)
{
	long val;
	char path[256];
	for (int psu = 1; psu <= 2; psu++) {
		snprintf(path, sizeof(path), CPLD_BASE "/psu_pwr%d_present", psu);
		if (sysfs_read_int(path, &val) == 0)
			printf("platform-mgrd: PSU%d present=%ld\n", psu, val);
		snprintf(path, sizeof(path), CPLD_BASE "/psu_pwr%d_all_ok", psu);
		if (sysfs_read_int(path, &val) == 0 && !val)
			printf("platform-mgrd: WARNING PSU%d not OK\n", psu);
	}
}

static void cpld_check_fans(void)
{
	long val;
	if (sysfs_read_int(CPLD_BASE "/system_fan_ok", &val) == 0 && !val)
		printf("platform-mgrd: WARNING fan not OK\n");
	if (sysfs_read_int(CPLD_BASE "/system_fan_present", &val) == 0 && !val)
		printf("platform-mgrd: WARNING fan not present\n");
}

static int cpld_set_led(const char *name, const char *color)
{
	char path[256];
	snprintf(path, sizeof(path), CPLD_BASE "/%s", name);
	return sysfs_write_str(path, color);
}

static int cpld_set_fan_pwm(int pwm)
{
	return sysfs_write_int(CPLD_BASE "/pwm1", pwm);
}

/* ---- Temperature ------------------------------------------------------ */

/* Returns max temperature in millidegrees C across all hwmon sensors */
static long thermal_read_max(void)
{
	DIR *d = opendir(HWMON_BASE);
	if (!d) return -1;

	long max_temp = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char base[256];
		snprintf(base, sizeof(base), "%s/%s", HWMON_BASE, e->d_name);
		for (int i = 1; i <= 10; i++) {
			char path[320];
			snprintf(path, sizeof(path), "%s/temp%d_input", base, i);
			long val;
			if (sysfs_read_int(path, &val) == 0 && val > max_temp)
				max_temp = val;
		}
	}
	closedir(d);
	return max_temp;
}

static void thermal_manage_fans(long max_mc)
{
	int pwm;
	if (max_mc < TEMP_LOW_MC)
		pwm = PWM_LOW;
	else if (max_mc < TEMP_MED_MC)
		pwm = PWM_MED;
	else if (max_mc < TEMP_HIGH_MC)
		pwm = PWM_HIGH;
	else {
		pwm = PWM_MAX;
		printf("platform-mgrd: WARNING high temp %ld.%03ld °C — fans at max\n",
			max_mc / 1000, max_mc % 1000);
	}
	cpld_set_fan_pwm(pwm);
}

/* ---- SFP EEPROM ------------------------------------------------------- */

/* Try sysfs eeprom_dev path first (needs at24 kernel driver bound) */
static int sfp_read_sysfs(int port, uint8_t *buf)
{
	char path[256];
	int eeprom_idx = port + SFP_EEPROM_OFFSET;
	snprintf(path, sizeof(path), "%s/eeprom%d/device/eeprom", EEPROM_DEV_BASE, eeprom_idx);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, SFP_EEPROM_SIZE);
	close(fd);
	return (n == SFP_EEPROM_SIZE) ? 0 : -1;
}

/* Fall back: direct /dev/i2c-N access */
static int sfp_read_i2c(int port, uint8_t *buf)
{
	char devpath[32];
	int bus = SFP_I2C_BUS_BASE + port;
	snprintf(devpath, sizeof(devpath), "/dev/i2c-%d", bus);
	int fd = open(devpath, O_RDWR);
	if (fd < 0) return -1;

	if (ioctl(fd, I2C_SLAVE, 0x50) < 0) {
		close(fd);
		return -1;
	}
	/* Set read offset to 0 */
	uint8_t reg = 0;
	if (write(fd, &reg, 1) != 1) {
		close(fd);
		return -1;
	}
	ssize_t n = read(fd, buf, SFP_EEPROM_SIZE);
	close(fd);
	return (n == SFP_EEPROM_SIZE) ? 0 : -1;
}

static const char *sfp_type_str(uint8_t id)
{
	switch (id) {
	case SFP_ID_SFP:      return "SFP";
	case SFP_ID_QSFP:     return "QSFP";
	case SFP_ID_QSFP_PLUS: return "QSFP+";
	case SFP_ID_QSFP28:   return "QSFP28";
	default:              return "Unknown";
	}
}

static void sfp_print_eeprom(int port, const uint8_t *buf)
{
	char vendor[SFP_BYTE_VENDOR_LEN + 1];
	char pn[SFP_BYTE_PN_LEN + 1];
	char sn[SFP_BYTE_SN_LEN + 1];

	memcpy(vendor, buf + SFP_BYTE_VENDOR, SFP_BYTE_VENDOR_LEN);
	vendor[SFP_BYTE_VENDOR_LEN] = '\0';
	trim_trailing(vendor, sizeof(vendor));

	memcpy(pn, buf + SFP_BYTE_PN, SFP_BYTE_PN_LEN);
	pn[SFP_BYTE_PN_LEN] = '\0';
	trim_trailing(pn, sizeof(pn));

	memcpy(sn, buf + SFP_BYTE_SN, SFP_BYTE_SN_LEN);
	sn[SFP_BYTE_SN_LEN] = '\0';
	trim_trailing(sn, sizeof(sn));

	printf("platform-mgrd: swp%d %s vendor=\"%s\" pn=\"%s\" sn=\"%s\"\n",
		port, sfp_type_str(buf[SFP_BYTE_ID]), vendor, pn, sn);
}

static void sfp_scan_all(void)
{
	uint8_t buf[SFP_EEPROM_SIZE];

	for (int port = 1; port <= NUM_PORTS; port++) {
		memset(buf, 0, sizeof(buf));

		/* Try sysfs first, then direct I2C */
		int ok = sfp_read_sysfs(port, buf);
		if (ok < 0)
			ok = sfp_read_i2c(port, buf);
		if (ok < 0)
			continue;   /* port absent or I2C not ready */

		/* Byte 0 must be a known SFP identifier */
		uint8_t id = buf[SFP_BYTE_ID];
		if (id == 0x00 || id == 0xFF)
			continue;   /* no module */

		sfp_print_eeprom(port, buf);
	}
}

/* ---- Main loop -------------------------------------------------------- */

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	printf("platform-mgrd: starting (AS5610-52X; poll=%ds wdt=%ds)\n",
		POLL_INTERVAL_S, WDT_KEEPALIVE_S);

	/* Initial LED state: diag = green on startup */
	cpld_set_led("led_diag", "green");

	int ticks = 0;
	for (;;) {
		/* Watchdog keepalive every WDT_KEEPALIVE_S seconds */
		if ((ticks % WDT_KEEPALIVE_S) == 0)
			cpld_watchdog_keepalive();

		/* Full platform poll every POLL_INTERVAL_S seconds */
		if ((ticks % POLL_INTERVAL_S) == 0) {
			/* Thermal */
			long max_temp = thermal_read_max();
			if (max_temp > 0) {
				printf("platform-mgrd: max_temp=%ld.%03ld °C\n",
					max_temp / 1000, max_temp % 1000);
				thermal_manage_fans(max_temp);
			}

			/* PSU and fans */
			cpld_check_psu();
			cpld_check_fans();

			/* SFP/QSFP EEPROM scan */
			sfp_scan_all();
		}

		sleep(1);
		ticks++;
	}
	return 0;
}
