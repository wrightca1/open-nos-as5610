/*
 * Simple userspace I2C register access via /dev/i2c-N.
 * Uses I2C_SLAVE ioctl for device addressing.
 */
#include "i2c.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define I2C_SLAVE 0x0703

int i2c_read_reg(int bus, int addr, uint8_t reg, uint8_t *val)
{
	char path[32];
	int fd;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;
	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, &reg, 1) != 1) {
		close(fd);
		return -1;
	}
	if (read(fd, val, 1) != 1) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int i2c_write_reg(int bus, int addr, uint8_t reg, uint8_t val)
{
	char path[32];
	int fd;
	uint8_t buf[2] = {reg, val};

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;
	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, buf, 2) != 2) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}
