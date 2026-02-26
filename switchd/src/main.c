/*
 * nos-switchd — Control plane daemon for open-nos-as5610
 * Bridges netlink/TUN and BCM56846 ASIC via libbcm56846.
 */
#include "bcm56846.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PORTS 56
#define PORTS_CONF_DEFAULT "/etc/nos/ports.conf"
#define CONFIG_PATH_DEFAULT "/etc/nos"

static volatile int running = 1;
static int tun_fds[MAX_PORTS];
static int num_ports;

extern int port_config_load(const char *path);
extern int port_config_get_count(void);
extern const char *port_config_get_name(int i);
extern int tun_create_all(const char *names[], int count, int *fds);
extern void tun_close_all(int *fds, int count);
extern void *netlink_thread(void *unit_ptr);
extern void netlink_stop(void);
extern void *link_state_thread(void *unit_ptr);
extern void link_state_stop(void);
extern void *tx_thread(void *arg);
extern void tx_stop(void);
extern int rx_start(int unit, int *tun_fds, int num_ports, void *cookie);

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	int unit = 0;
	const char *config_path = CONFIG_PATH_DEFAULT;
	const char *ports_conf = PORTS_CONF_DEFAULT;
	int i;

	(void)argc;
	(void)argv;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	memset(tun_fds, -1, sizeof(tun_fds));
	if (port_config_load(ports_conf) < 0) {
		fprintf(stderr, "port_config_load failed\n");
		return 1;
	}
	num_ports = port_config_get_count();
	if (num_ports <= 0 || num_ports > MAX_PORTS) {
		fprintf(stderr, "invalid port count %d\n", num_ports);
		return 1;
	}

	if (bcm56846_attach(unit) < 0) {
		fprintf(stderr, "bcm56846_attach failed (is /dev/nos-bde present?)\n");
		return 1;
	}
	if (bcm56846_init(unit, config_path) < 0) {
		fprintf(stderr, "bcm56846_init failed\n");
		bcm56846_detach(unit);
		return 1;
	}

	/* TUN creation for swp1..swpN */
	{
		const char *names[MAX_PORTS];
		for (i = 0; i < num_ports; i++)
			names[i] = port_config_get_name(i);
		if (tun_create_all(names, num_ports, tun_fds) < 0) {
			fprintf(stderr, "tun_create_all failed (need CAP_NET_ADMIN?)\n");
			bcm56846_detach(unit);
			return 1;
		}
		fprintf(stderr, "Created %d TUN interfaces (swp1..swp%d)\n", num_ports, num_ports);
	}

	/* RX callback cookie and start RX (callback writes to TUN on ASIC punt) */
	{
		static struct rx_cookie_tag { int *tun_fds; int num_ports; } rx_cookie;
		rx_cookie.tun_fds = tun_fds;
		rx_cookie.num_ports = num_ports;
		if (rx_start(unit, tun_fds, num_ports, &rx_cookie) < 0)
			fprintf(stderr, "rx_start failed (stub)\n");
	}

	/* Threads: netlink, link-state poll, TX */
	{
		pthread_t th_netlink, th_link, th_tx;
		static struct { int unit; int *tun_fds; int num_ports; } tx_arg;

		tx_arg.unit = unit;
		tx_arg.tun_fds = tun_fds;
		tx_arg.num_ports = num_ports;

		pthread_create(&th_netlink, NULL, netlink_thread, &unit);
		pthread_create(&th_link, NULL, link_state_thread, &unit);
		pthread_create(&th_tx, NULL, tx_thread, &tx_arg);
		fprintf(stderr, "netlink, link-state, TX threads started\n");

		while (running)
			sleep(1);

		netlink_stop();
		link_state_stop();
		tx_stop();
		pthread_join(th_netlink, NULL);
		pthread_join(th_link, NULL);
		pthread_join(th_tx, NULL);
	}

	tun_close_all(tun_fds, num_ports);
	bcm56846_detach(unit);
	return 0;
}
