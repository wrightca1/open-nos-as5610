#ifndef I2C_H
#define I2C_H

#include <stdint.h>

int i2c_read_reg(int bus, int addr, uint8_t reg, uint8_t *val);
int i2c_write_reg(int bus, int addr, uint8_t reg, uint8_t val);

#endif
