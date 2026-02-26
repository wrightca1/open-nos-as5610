/*
 * nos-switchd — Control plane daemon for open-nos-as5610
 * Bridges netlink/TUN and BCM56846 ASIC via libbcm56846.
 */
#include "bcm56846.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	int unit = 0;
	const char *config_path = "/etc/nos/config.bcm";

	(void)argc;
	(void)argv;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	if (bcm56846_attach(unit) < 0) {
		fprintf(stderr, "bcm56846_attach failed (is /dev/nos-bde present?)\n");
		return 1;
	}
	if (bcm56846_init(unit, config_path) < 0) {
		fprintf(stderr, "bcm56846_init failed\n");
		bcm56846_detach(unit);
		return 1;
	}

	/* TODO: TUN creation, netlink listener, link poll, TX/RX threads */
	while (running)
		sleep(1);

	bcm56846_detach(unit);
	return 0;
}
