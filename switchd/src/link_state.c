/*
 * Link-state poll thread — poll ASIC port link every 200 ms,
 * reflect to TUN admin state (SIOCSIFFLAGS) so kernel/FRR see link up/down.
 */
#include "bcm56846.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int port_config_get_count(void);
extern const char *port_config_get_name(int i);
extern int tun_set_up(const char *ifname, int up);

#define POLL_MS 200
#define MAX_PORTS 56

static volatile int link_state_running = 1;

void *link_state_thread(void *arg)
{
	int unit = *(int *)arg;
	int num_ports = port_config_get_count();
	int last_up[MAX_PORTS];
	int i, link_up, changed;

	if (num_ports <= 0 || num_ports > MAX_PORTS)
		return NULL;

	memset(last_up, -1, sizeof(last_up));

	while (link_state_running) {
		usleep(POLL_MS * 1000);
		for (i = 0; i < num_ports; i++) {
			if (bcm56846_port_link_status_get(unit, i + 1, &link_up) != 0)
				continue;
			changed = (last_up[i] != link_up);
			last_up[i] = link_up;
			if (changed) {
				const char *name = port_config_get_name(i);
				if (name && tun_set_up(name, link_up) == 0) {
					/* optional: log link state change */
				}
			}
		}
	}
	return NULL;
}

void link_state_stop(void)
{
	link_state_running = 0;
}
