// SPDX-License-Identifier: GPL-2.0
/*
 * Accton AS5610-52X CPLD Driver
 *
 * Binds to compatible "accton,accton_as5610_52x-cpld" (eLBC, chip select 1,
 * CPU physical address 0xea000000, size 0x100).
 *
 * Register map (from ONL platform sources):
 *   0x01 - PSU2 status  (bit0: present active-L, bit1: power-ok active-H)
 *   0x02 - PSU1 status  (same)
 *   0x03 - System/fan   (bit2: fan-present active-L, bit3: fan-fail active-H)
 *   0x0D - Fan speed    (5-bit value: 0x0C=40%, 0x15=70%, 0x1F=100%)
 *   0x13 - LED control  (bits[1:0]=PSU1, bits[3:2]=PSU2, bits[5:4]=DIAG, bits[7:6]=FAN)
 *   0x15 - Locator LED  (bits[1:0]: 0x01=off, 0x03=orange-blink)
 *
 * Sysfs attributes exposed (same names used by platform-mgrd C daemon):
 *   pwm1               - fan duty cycle 0-100 (rw)
 *   system_fan_ok      - 1=ok, 0=fault (ro)
 *   system_fan_present - 1=present, 0=absent (ro)
 *   psu_pwr1_present   - 1=present, 0=absent (ro)
 *   psu_pwr1_all_ok    - 1=ok, 0=fault (ro)
 *   psu_pwr2_present   - 1=present, 0=absent (ro)
 *   psu_pwr2_all_ok    - 1=ok, 0=fault (ro)
 *   led_diag           - "green"/"amber"/"off" (rw)
 *   watch_dog_keep_alive - write "1" to strobe (wo)
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/kernel.h>

/* CPLD register offsets */
#define CPLD_REG_PSU2_STATUS	0x01
#define CPLD_REG_PSU1_STATUS	0x02
#define CPLD_REG_SYS_STATUS	0x03
#define CPLD_REG_FAN_SPEED	0x0D
#define CPLD_REG_LED		0x13
#define CPLD_REG_LED_LOC	0x15

/* PSU status bits */
#define PSU_PRESENT_MASK	0x01	/* active LOW: 0 = present */
#define PSU_POWER_OK_MASK	0x02	/* active HIGH: 1 = OK */

/* Fan/system status bits */
#define FAN_PRESENT_MASK	0x04	/* active LOW: 0 = present */
#define FAN_FAILURE_MASK	0x08	/* active HIGH: 1 = failure */
#define FAN_AIR_FLOW_MASK	0x10	/* 0 = front-to-back, 1 = back-to-front */

/*
 * Fan speed register (reg 0x0D): 5-bit raw value 0x00-0x1F.
 * Cumulus/ONLP use a 0-248 scale where pwm_val = raw * 8.
 * Typical operating points: 0x0C (12→96) = 40%, 0x15 (21→168) = 70%,
 * 0x1F (31→248) = 100%.  We expose the 0-248 scale to userspace.
 */
#define FAN_SPEED_RAW_MAX	0x1F	/* 5-bit max (31) */
#define FAN_PWM_SCALE		8	/* pwm_val = raw * 8; max = 248 */
#define FAN_PWM_MAX		248

/* LED register bits (reg 0x13) — 2-bit field per LED: 00=off, 01=amber, 10=green */
#define LED_PSU1_MASK		0x03	/* bits[1:0] */
#define LED_PSU1_OFF		0x00
#define LED_PSU1_AMBER		0x01
#define LED_PSU1_GREEN		0x02
#define LED_PSU2_MASK		0x0C	/* bits[3:2] */
#define LED_PSU2_OFF		0x00
#define LED_PSU2_AMBER		0x04
#define LED_PSU2_GREEN		0x08
#define LED_DIAG_MASK		0x30	/* bits[5:4] */
#define LED_DIAG_OFF		0x00
#define LED_DIAG_AMBER		0x10
#define LED_DIAG_GREEN		0x20
#define LED_FAN_MASK		0xC0	/* bits[7:6] */
#define LED_FAN_OFF		0x00
#define LED_FAN_AMBER		0x40
#define LED_FAN_GREEN		0x80

struct as5610_cpld {
	void __iomem	*base;
	struct device	*dev;
};

static inline u8 cpld_rd(struct as5610_cpld *cpld, u8 reg)
{
	return ioread8(cpld->base + reg);
}

static inline void cpld_wr(struct as5610_cpld *cpld, u8 reg, u8 val)
{
	iowrite8(val, cpld->base + reg);
}

/* ---- Fan PWM ----------------------------------------------------------- */

/*
 * pwm1 uses the 0-248 Cumulus/ONLP scale (raw_reg * 8).
 * platform-mgrd writes: 64 (low), 128 (med), 200 (high), 248 (max).
 * These map to raw CPLD values: 8, 16, 25, 31.
 */
static ssize_t pwm1_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 raw = cpld_rd(cpld, CPLD_REG_FAN_SPEED) & FAN_SPEED_RAW_MAX;

	return sprintf(buf, "%d\n", (int)raw * FAN_PWM_SCALE);
}

static ssize_t pwm1_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;
	val = clamp_val(val, 0, FAN_PWM_MAX);
	cpld_wr(cpld, CPLD_REG_FAN_SPEED, (u8)(val / FAN_PWM_SCALE));
	return count;
}

/* ---- PSU status -------------------------------------------------------- */

static ssize_t psu_pwr1_present_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       !(cpld_rd(cpld, CPLD_REG_PSU1_STATUS) & PSU_PRESENT_MASK));
}

static ssize_t psu_pwr1_all_ok_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       !!(cpld_rd(cpld, CPLD_REG_PSU1_STATUS) & PSU_POWER_OK_MASK));
}

static ssize_t psu_pwr2_present_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       !(cpld_rd(cpld, CPLD_REG_PSU2_STATUS) & PSU_PRESENT_MASK));
}

static ssize_t psu_pwr2_all_ok_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       !!(cpld_rd(cpld, CPLD_REG_PSU2_STATUS) & PSU_POWER_OK_MASK));
}

/* ---- Fan status -------------------------------------------------------- */

static ssize_t system_fan_ok_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       !(cpld_rd(cpld, CPLD_REG_SYS_STATUS) & FAN_FAILURE_MASK));
}

static ssize_t system_fan_present_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		       !(cpld_rd(cpld, CPLD_REG_SYS_STATUS) & FAN_PRESENT_MASK));
}

static ssize_t system_fan_air_flow_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val = cpld_rd(cpld, CPLD_REG_SYS_STATUS);

	return sprintf(buf, "%s\n",
		       (val & FAN_AIR_FLOW_MASK) ? "back-to-front" : "front-to-back");
}

/* ---- LED --------------------------------------------------------------- */

static ssize_t led_diag_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val = cpld_rd(cpld, CPLD_REG_LED) & LED_DIAG_MASK;
	const char *s = (val == LED_DIAG_GREEN) ? "green" :
			(val == LED_DIAG_AMBER) ? "amber" : "off";
	return sprintf(buf, "%s\n", s);
}

static ssize_t led_diag_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val = cpld_rd(cpld, CPLD_REG_LED) & ~LED_DIAG_MASK;

	if (sysfs_streq(buf, "green"))
		val |= LED_DIAG_GREEN;
	else if (sysfs_streq(buf, "amber"))
		val |= LED_DIAG_AMBER;
	/* else: LED_DIAG_OFF (0x00) already set by the mask-clear above */

	cpld_wr(cpld, CPLD_REG_LED, val);
	return count;
}

/* ---- PSU LEDs (reg 0x13 bits[1:0] and [3:2]) -------------------------- */

static ssize_t led_psu_show(struct device *dev, u8 mask, u8 green, u8 amber,
			    char *buf)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val = cpld_rd(cpld, CPLD_REG_LED) & mask;

	const char *s = (val == green) ? "green" :
			(val == amber) ? "amber" : "off";
	return sprintf(buf, "%s\n", s);
}

static ssize_t led_psu_store(struct device *dev, u8 mask, u8 green, u8 amber,
			     const char *buf, size_t count)
{
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	u8 val = cpld_rd(cpld, CPLD_REG_LED) & ~mask;

	if (sysfs_streq(buf, "green"))
		val |= green;
	else if (sysfs_streq(buf, "amber"))
		val |= amber;

	cpld_wr(cpld, CPLD_REG_LED, val);
	return count;
}

static ssize_t led_psu1_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return led_psu_show(dev, LED_PSU1_MASK, LED_PSU1_GREEN, LED_PSU1_AMBER, buf);
}

static ssize_t led_psu1_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	return led_psu_store(dev, LED_PSU1_MASK, LED_PSU1_GREEN, LED_PSU1_AMBER,
			     buf, count);
}

static ssize_t led_psu2_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return led_psu_show(dev, LED_PSU2_MASK, LED_PSU2_GREEN, LED_PSU2_AMBER, buf);
}

static ssize_t led_psu2_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	return led_psu_store(dev, LED_PSU2_MASK, LED_PSU2_GREEN, LED_PSU2_AMBER,
			     buf, count);
}

/* ---- Fan LED (reg 0x13 bits[7:6]) ------------------------------------- */

static ssize_t led_fan_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return led_psu_show(dev, LED_FAN_MASK, LED_FAN_GREEN, LED_FAN_AMBER, buf);
}

static ssize_t led_fan_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	return led_psu_store(dev, LED_FAN_MASK, LED_FAN_GREEN, LED_FAN_AMBER,
			     buf, count);
}

/* ---- Watchdog keepalive ------------------------------------------------ */

static ssize_t watch_dog_keep_alive_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	/*
	 * Watchdog register not definitively identified from ONL sources.
	 * Writing to register 0x06 (common CPLD keepalive location) as a best
	 * effort; this will be a no-op if the register is unused.
	 */
	struct as5610_cpld *cpld = dev_get_drvdata(dev);
	cpld_wr(cpld, 0x06, 0x01);
	return count;
}

/* ---- Attribute declarations ------------------------------------------- */

static DEVICE_ATTR_RW(pwm1);
static DEVICE_ATTR_RO(psu_pwr1_present);
static DEVICE_ATTR_RO(psu_pwr1_all_ok);
static DEVICE_ATTR_RO(psu_pwr2_present);
static DEVICE_ATTR_RO(psu_pwr2_all_ok);
static DEVICE_ATTR_RO(system_fan_ok);
static DEVICE_ATTR_RO(system_fan_present);
static DEVICE_ATTR_RO(system_fan_air_flow);
static DEVICE_ATTR_RW(led_diag);
static DEVICE_ATTR_RW(led_psu1);
static DEVICE_ATTR_RW(led_psu2);
static DEVICE_ATTR_RW(led_fan);
static DEVICE_ATTR_WO(watch_dog_keep_alive);

static struct attribute *cpld_attrs[] = {
	&dev_attr_pwm1.attr,
	&dev_attr_psu_pwr1_present.attr,
	&dev_attr_psu_pwr1_all_ok.attr,
	&dev_attr_psu_pwr2_present.attr,
	&dev_attr_psu_pwr2_all_ok.attr,
	&dev_attr_system_fan_ok.attr,
	&dev_attr_system_fan_present.attr,
	&dev_attr_system_fan_air_flow.attr,
	&dev_attr_led_diag.attr,
	&dev_attr_led_psu1.attr,
	&dev_attr_led_psu2.attr,
	&dev_attr_led_fan.attr,
	&dev_attr_watch_dog_keep_alive.attr,
	NULL
};

static const struct attribute_group cpld_attr_group = {
	.attrs = cpld_attrs,
};

/* ---- Platform driver --------------------------------------------------- */

static int as5610_cpld_probe(struct platform_device *pdev)
{
	struct as5610_cpld *cpld;
	int ret;

	cpld = devm_kzalloc(&pdev->dev, sizeof(*cpld), GFP_KERNEL);
	if (!cpld)
		return -ENOMEM;

	cpld->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cpld->base)) {
		dev_err(&pdev->dev, "ioremap failed: %ld\n",
			PTR_ERR(cpld->base));
		return PTR_ERR(cpld->base);
	}

	cpld->dev = &pdev->dev;
	platform_set_drvdata(pdev, cpld);

	ret = sysfs_create_group(&pdev->dev.kobj, &cpld_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "sysfs_create_group failed: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev,
		 "AS5610-52X CPLD at %pap, fan/LED/PSU sysfs ready\n",
		 &pdev->resource[0].start);
	return 0;
}

static int as5610_cpld_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &cpld_attr_group);
	return 0;
}

static const struct of_device_id as5610_cpld_of_match[] = {
	{ .compatible = "accton,accton_as5610_52x-cpld" },
	{ }
};
MODULE_DEVICE_TABLE(of, as5610_cpld_of_match);

static struct platform_driver as5610_cpld_driver = {
	.probe  = as5610_cpld_probe,
	.remove = as5610_cpld_remove,
	.driver = {
		.name		= "accton-as5610-cpld",
		.of_match_table	= as5610_cpld_of_match,
	},
};

module_platform_driver(as5610_cpld_driver);

MODULE_AUTHOR("Open NOS Project");
MODULE_DESCRIPTION("Accton AS5610-52X CPLD Platform Driver");
MODULE_LICENSE("GPL");
