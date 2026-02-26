/* TUN device creation for swp1..swpN */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#define TUN_DEVICE "/dev/net/tun"

int tun_create(const char *ifname, int *fd_out)
{
	struct ifreq ifr;
	int fd;

	fd = open(TUN_DEVICE, O_RDWR);
	if (fd < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		close(fd);
		return -1;
	}
	*fd_out = fd;
	return 0;
}

int tun_create_all(const char *names[], int count, int *fds)
{
	int i;
	for (i = 0; i < count; i++) {
		if (tun_create(names[i], &fds[i]) < 0) {
			while (i--)
				close(fds[i]);
			return -1;
		}
	}
	return 0;
}

void tun_close_all(int *fds, int count)
{
	int i;
	for (i = 0; i < count; i++)
		if (fds[i] >= 0)
			close(fds[i]);
}

/* Set interface admin up/down (e.g. to reflect ASIC link state). Uses socket ioctl. */
int tun_set_up(const char *ifname, int up)
{
	struct ifreq ifr;
	int sock;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
		close(sock);
		return -1;
	}
	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;
	if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
		close(sock);
		return -1;
	}
	close(sock);
	return 0;
}
