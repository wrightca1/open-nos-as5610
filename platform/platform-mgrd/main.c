/*
 * platform-mgrd — minimal platform daemon for AS5610-52X.
 * Reads thermal (hwmon), fan, PSU status from sysfs (RE: PLATFORM_ENVIRONMENTAL_AND_PSU_ACCESS.md).
 * Optional: adjust fan PWM from thermal policy (not implemented here; see ONLP for full support).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define SYS_HWMON "/sys/class/hwmon"
#define POLL_INTERVAL 30

static int read_sysfs_int(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int val = -1;
	if (fscanf(f, "%d", &val) == 1 && out) *out = val;
	fclose(f);
	return (val >= 0) ? 0 : -1;
}

static void poll_hwmon(void)
{
	DIR *d = opendir(SYS_HWMON);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char base[256];
		snprintf(base, sizeof(base), "%s/%s", SYS_HWMON, e->d_name);
		char path[320];
		int temp;
		snprintf(path, sizeof(path), "%s/temp1_input", base);
		if (read_sysfs_int(path, &temp) == 0)
			printf("platform-mgrd: %s temp1=%d mC\n", e->d_name, temp);
	}
	closedir(d);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("platform-mgrd: starting (poll %ds)\n", POLL_INTERVAL);
	for (;;) {
		poll_hwmon();
		sleep(POLL_INTERVAL);
	}
	return 0;
}
