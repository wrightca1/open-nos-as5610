/* Load config.bcm (key=value). Portmap and other params for ASIC init. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_PORTMAP 56
#define CONFIG_PATH_MAX 256

typedef struct {
	int lane;
	int speed; /* 10 or 40 */
} portmap_entry_t;

static portmap_entry_t portmap[MAX_PORTMAP];
static int portmap_count;

/* Call after load_config; used by init/port code */
int bcm56846_config_get_portmap(int port_id, int *lane, int *speed)
{
	if (port_id < 0 || port_id >= portmap_count || !lane || !speed)
		return -1;
	*lane = portmap[port_id].lane;
	*speed = portmap[port_id].speed;
	return 0;
}

int bcm56846_config_get_port_count(void)
{
	return portmap_count;
}

/* Parse "portmap_N.0=65:10" or "portmap_N=65:10" */
static int parse_portmap_line(const char *line)
{
	int n, lane, speed;
	if (sscanf(line, "portmap_%d.0=%d:%d", &n, &lane, &speed) == 3 ||
	    sscanf(line, "portmap_%d=%d:%d", &n, &lane, &speed) == 3) {
		if (n >= 0 && n < MAX_PORTMAP) {
			portmap[n].lane = lane;
			portmap[n].speed = speed;
			if (n >= portmap_count)
				portmap_count = n + 1;
		}
		return 0;
	}
	return -1;
}

static void trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		e--;
	*e = '\0';
}

int bcm56846_config_load(const char *path)
{
	FILE *f;
	char line[256];
	char filepath[CONFIG_PATH_MAX];

	portmap_count = 0;
	memset(portmap, 0, sizeof(portmap));

	if (!path)
		return -1;
	if (strstr(path, ".bcm"))
		snprintf(filepath, sizeof(filepath), "%s", path);
	else
		snprintf(filepath, sizeof(filepath), "%s%sconfig.bcm", path, (path[0] && path[strlen(path)-1] == '/') ? "" : "/");

	f = fopen(filepath, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		parse_portmap_line(line);
	}
	fclose(f);
	return 0;
}
