/*
 * espnet_core.c - /dev/espnet char device + state board.
 *
 * Registers a misc device unconditionally (so it exists on ANY Linux, even a
 * dev box without the ESP32 attached) that:
 *   - carries a byte stream produced by espnet_usb (bulk IN over USB) into a
 *     kernel FIFO, readable by user space
 *   - tracks reachability state (ap_active, rssi, vbus_power), settable by
 *     the userspace agent via ESPNET_IOCTL_SET_AP, observable by any client
 *     via ESPNET_IOCTL_GET_STATE or sysfs.
 *
 * Policy lives in user space; the kernel provides the interface. This is the
 * classic "driver = mechanism, daemon = policy" split.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "espnet.h"

#define ESPNET_FIFO_BYTES 8192

static DEFINE_SPINLOCK(es_lock);

static struct espnet_state es_state = {
	.ap_active   = 0,
	.rssi        = 0,
	.vbus_power  = 1,
	.ext_ssid    = "3bbo_Ext",
};

DECLARE_KFIFO(es_rx, u8, ESPNET_FIFO_BYTES);

/* ------------------------------------------------------------------ */
int espnet_core_rx(const u8 *data, size_t len)
{
	/* FIFO full? drop (telemetry is loss-tolerant by design). */
	if (!data || !len)
		return 0;

	if (kfifo_avail(&es_rx) < (unsigned int)len)
		return 0;

	kfifo_in(&es_rx, data, (unsigned int)len);
	return (int)len;
}
EXPORT_SYMBOL_GPL(espnet_core_rx);

int espnet_core_set_ap(bool active)
{
	unsigned long flags;
	spin_lock_irqsave(&es_lock, flags);
	es_state.ap_active = active ? 1 : 0;
	spin_unlock_irqrestore(&es_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(espnet_core_set_ap);

/* ------------------------------------------------------------------ */
static long espnet_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case ESPNET_IOCTL_GET_STATE: {
		struct espnet_state st;
		unsigned long flags;

		spin_lock_irqsave(&es_lock, flags);
		st = es_state;
		spin_unlock_irqrestore(&es_lock, flags);

		st.ext_ssid[ESPNET_EXT_SSID_LEN - 1] = '\0';
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	case ESPNET_IOCTL_SET_AP: {
		__u32 on;
		unsigned long flags;

		if (copy_from_user(&on, (void __user *)arg, sizeof(on)))
			return -EFAULT;

		spin_lock_irqsave(&es_lock, flags);
		es_state.ap_active = on ? 1 : 0;
		spin_unlock_irqrestore(&es_lock, flags);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static ssize_t espnet_read(struct file *filp, char __user *buf,
			   size_t count, loff_t *ppos)
{
	unsigned int copied = 0;
	unsigned int to_read = min_t(size_t, count,
				     (size_t)ESPNET_FIFO_BYTES);

	if (to_read == 0)
		return 0;

	if (kfifo_to_user(&es_rx, buf, to_read, &copied))
		return -EFAULT;
	return (ssize_t)copied;
}

/*
 * Userspace->FIFO producer. The kernel normally fills the FIFO from the
 * ESP32's USB bulk-IN stream (espnet_core_rx); this lets tools, the smoke
 * test, and the agent itself inject telemetry when no hardware is attached
 * ("virtual ESP32"). Write returns the bytes actually queued - the FIFO is
 * loss-tolerant by design, so a full buffer truncates, never blocks.
 */
static ssize_t espnet_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	size_t done = 0;

	while (done < count) {
		u8 chunk[64];
		size_t n = min_t(size_t, count - done, sizeof(chunk));
		int pushed;

		if (copy_from_user(chunk, buf + done, n))
			return done ? (ssize_t)done : -EFAULT;
		pushed = espnet_core_rx(chunk, n);
		if (pushed <= 0)
			break;   /* FIFO full - stop, report what was accepted */
		done += (size_t)pushed;
	}
	return (ssize_t)done;
}

static const struct file_operations espnet_fops = {
	.owner          = THIS_MODULE,
	.read           = espnet_read,
	.write          = espnet_write,
	.unlocked_ioctl = espnet_ioctl,
	.llseek         = noop_llseek,
};

/* ------------------------------------------------------------------ */
static ssize_t ap_active_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	unsigned long flags;
	int v;

	spin_lock_irqsave(&es_lock, flags);
	v = es_state.ap_active;
	spin_unlock_irqrestore(&es_lock, flags);
	return sysfs_emit(buf, "%d\n", v);
}
static DEVICE_ATTR_RO(ap_active);

static ssize_t vbus_power_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	unsigned long flags;
	int v;

	spin_lock_irqsave(&es_lock, flags);
	v = es_state.vbus_power;
	spin_unlock_irqrestore(&es_lock, flags);
	return sysfs_emit(buf, "%d\n", v);
}
static DEVICE_ATTR_RO(vbus_power);

static struct miscdevice espnet_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "espnet",
	.fops  = &espnet_fops,
};

static int __init espnet_core_init(void)
{
	int ret;

	INIT_KFIFO(es_rx);

	ret = misc_register(&espnet_misc);
	if (ret)
		return ret;

	ret = device_create_file(espnet_misc.this_device, &dev_attr_ap_active);
	if (ret)
		goto err_file;

	ret = device_create_file(espnet_misc.this_device, &dev_attr_vbus_power);
	if (ret) {
		device_remove_file(espnet_misc.this_device, &dev_attr_ap_active);
		goto err_file;
	}

	pr_info("espnet: core registered, /dev/espnet ready\n");
	return 0;

err_file:
	misc_deregister(&espnet_misc);
	return ret;
}

static void __exit espnet_core_exit(void)
{
	device_remove_file(espnet_misc.this_device, &dev_attr_ap_active);
	device_remove_file(espnet_misc.this_device, &dev_attr_vbus_power);
	misc_deregister(&espnet_misc);
	pr_info("espnet: core unregistered\n");
}

module_init(espnet_core_init);
module_exit(espnet_core_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Blue project");
MODULE_DESCRIPTION("ESP32 extender link state board and telemetry FIFO");
MODULE_VERSION("0.1");