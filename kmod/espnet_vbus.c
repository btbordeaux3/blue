/*
 * espnet_vbus.c - DT-bound platform driver for the USB-C power rail that
 * powers the ESP32 mobile unit.
 *
 * The mobile ESP32 is "kept powered whenever the host is on / not in sleep",
 * so the rail it lives on is described in the device tree (dt-overlay/
 * blue-vbus.dts) and gated by this driver through an enable GPIO. The kernel
 * owns the hardware (mechanism); policy (when to cut/restore) stays with the
 * userspace agent / PM framework.
 */

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

struct espnet_vbus_priv {
	struct gpio_desc *enable;
};

static ssize_t power_enabled_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct espnet_vbus_priv *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n",
			  priv ? gpiod_get_value_cansleep(priv->enable) : 0);
}

static ssize_t power_enabled_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct espnet_vbus_priv *priv = dev_get_drvdata(dev);
	bool value;
	int ret;

	if (!priv)
		return -ENODEV;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(priv->enable, value);
	return count;
}
static DEVICE_ATTR_RW(power_enabled);

static int espnet_vbus_probe(struct platform_device *pdev)
{
	struct espnet_vbus_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* "enable-gpios" from the DT node; low by default, then assert. */
	priv->enable = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable))
		return PTR_ERR(priv->enable);

	gpiod_set_value_cansleep(priv->enable, 1);

	platform_set_drvdata(pdev, priv);

	ret = device_create_file(&pdev->dev, &dev_attr_power_enabled);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "espnet: ESP32 VBUS rail enabled via DT node\n");
	return 0;
}

static void espnet_vbus_remove(struct platform_device *pdev)
{
	struct espnet_vbus_priv *priv = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_power_enabled);
	if (priv)
		gpiod_set_value_cansleep(priv->enable, 0);
}

static const struct of_device_id espnet_vbus_of_match[] = {
	{ .compatible = "blue,esp32-vbus" },
	{ }
};
MODULE_DEVICE_TABLE(of, espnet_vbus_of_match);

static struct platform_driver espnet_vbus_driver = {
	.probe  = espnet_vbus_probe,
	.remove = espnet_vbus_remove,
	.driver = {
		.name           = "blue-esp32-vbus",
		.of_match_table = espnet_vbus_of_match,
	},
};
module_platform_driver(espnet_vbus_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Blue project");
MODULE_DESCRIPTION("DT-bound power rail driver for the USB-C-tethered ESP32");
MODULE_VERSION("0.1");