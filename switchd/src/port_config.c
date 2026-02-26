/* Parse ports.conf: port mode (10G/40G). Build list of interface names swp1..swp52. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PORTS 56
#define MAX_NAME 32

static char port_names[MAX_PORTS][MAX_NAME];
static int port_count;

const char *port_config_get_name(int i)
{
	if (i < 0 || i >= port_count)
		return NULL;
	return port_names[i];
}

int port_config_get_count(void)
{
	return port_count;
}

/* ports.conf format: N=10G or N=40G or N=4x10G. One per line. */
int port_config_load(const char *path)
{
	FILE *f;
	char line[128];
	int n, i;

	port_count = 0;
	memset(port_names, 0, sizeof(port_names));

	f = fopen(path, "r");
	if (!f) {
		/* Default: 52 ports swp1..swp52 */
		port_count = 52;
		for (i = 0; i < port_count; i++)
			snprintf(port_names[i], MAX_NAME, "swp%d", i + 1);
		return 0;
	}
	while (fgets(line, sizeof(line), f) && port_count < MAX_PORTS) {
		if (sscanf(line, "%d=", &n) == 1 && n >= 1 && n <= MAX_PORTS) {
			snprintf(port_names[port_count], MAX_NAME, "swp%d", n);
			port_count++;
		}
	}
	fclose(f);
	if (port_count == 0) {
		port_count = 52;
		for (i = 0; i < port_count; i++)
			snprintf(port_names[i], MAX_NAME, "swp%d", i + 1);
	}
	return 0;
}
