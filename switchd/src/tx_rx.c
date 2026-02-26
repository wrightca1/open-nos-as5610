/*
 * TX thread: epoll on TUN fds; read packet -> bcm56846_tx(unit, port, pkt, len).
 * RX: bcm56846_rx_register callback writes packet to correct TUN fd.
 * Port numbering: TUN index i = swp(i+1) = BCM port (i+1) 1-based.
 */
#include "bcm56846.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#define MAX_PORTS 56
#define MAX_PKT_SIZE 2048
#define EPOLL_MAX 64

struct rx_cookie {
	int *tun_fds;
	int num_ports;
};

static volatile int tx_running = 1;

static void rx_callback(int unit, int port, const void *pkt, int len, void *cookie)
{
	struct rx_cookie *c = (struct rx_cookie *)cookie;
	int idx;
	ssize_t n;

	(void)unit;
	idx = port - 1; /* BCM port 1-based -> TUN index 0-based */
	if (idx < 0 || idx >= c->num_ports || c->tun_fds[idx] < 0)
		return;
	n = write(c->tun_fds[idx], pkt, len);
	if (n != (ssize_t)len) {
		/* partial or error; stub ignores */
	}
}

void *tx_thread(void *arg)
{
	struct tx_args {
		int unit;
		int *tun_fds;
		int num_ports;
	} *a = (struct tx_args *)arg;
	int epfd, i, nfds, fd;
	struct epoll_event ev, events[EPOLL_MAX];
	unsigned char *pkt;
	ssize_t len;

	pkt = malloc(MAX_PKT_SIZE);
	if (!pkt)
		return NULL;
	epfd = epoll_create1(0);
	if (epfd < 0) {
		free(pkt);
		return NULL;
	}
	for (i = 0; i < a->num_ports; i++) {
		if (a->tun_fds[i] < 0)
			continue;
		ev.events = EPOLLIN;
		ev.data.fd = a->tun_fds[i];
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, a->tun_fds[i], &ev) < 0)
			continue;
	}

	while (tx_running) {
		nfds = epoll_wait(epfd, events, EPOLL_MAX, 500);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 0; i < nfds; i++) {
			fd = events[i].data.fd;
			len = read(fd, pkt, MAX_PKT_SIZE);
			if (len <= 0)
				continue;
			/* Find port index for this fd */
			for (int j = 0; j < a->num_ports; j++) {
				if (a->tun_fds[j] == fd) {
					bcm56846_tx(a->unit, j + 1, pkt, (int)len);
					break;
				}
			}
		}
	}

	close(epfd);
	free(pkt);
	return NULL;
}

void tx_stop(void)
{
	tx_running = 0;
}

/* Register RX callback and start RX; call once after SDK init. */
int rx_start(int unit, int *tun_fds, int num_ports, struct rx_cookie *cookie)
{
	cookie->tun_fds = tun_fds;
	cookie->num_ports = num_ports;
	if (bcm56846_rx_register(unit, rx_callback, cookie) != 0)
		return -1;
	return bcm56846_rx_start(unit);
}
