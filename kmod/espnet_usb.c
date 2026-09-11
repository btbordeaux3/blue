/*
 * espnet_usb.c - USB driver for the ESP32 tether.
 *
 * Binds to the ESP32's USB identity (default: Espressif native-USB ESP32-S3,
 * VID 0x303a PROD 0x1001), streams its bulk-IN UART/telemetry bytes into the
 * espnet_core FIFO, and hands /dev/espnet to user space.
 *
 * For classic ESP32 dev boards (USB-UART bridge, e.g. CP210x on esp32dev)
 * use the driver_override mechanism to force-bind this driver to the bridge
 * instead of cp210x:
 *
 *     # modprobe espnet_core espnet_usb
 *     # echo "10c4 ea60" > /sys/bus/usb/drivers/espnet_usb/new_id
 *     # echo -n "espnet_usb" > /sys/bus/usb/devices/x-x:1.0/driver_override
 *     # echo "x-x:1.0" > /sys/bus/usb/drivers_probe
 *
 * The bridge firmware transparently forwards the UART stream over its bulk
 * endpoints, so the telemetry JSON reaches the kernel FIFO unchanged.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define ESPNET_URB_BUF 512

extern int espnet_core_rx(const u8 *data, size_t len);

static const struct usb_device_id espnet_ids[] = {
	{ USB_DEVICE(0x303a, 0x1001) }, /* Espressif ESP32-S3 USB CDC */
	{ }                              /* terminator */
};
MODULE_DEVICE_TABLE(usb, espnet_ids);

struct espnet_usb_priv {
	struct usb_device   *udev;
	struct usb_endpoint_descriptor *in_ep;
	struct urb          *urb;
	u8                  *buf;
	dma_addr_t           dma;
};

static void espnet_usb_complete(struct urb *urb)
{
	struct espnet_usb_priv *priv = urb->context;
	int ret;

	if (urb->status == -ECONNRESET || urb->status == -ESHUTDOWN ||
	    urb->status == -ENOENT) {
		/* endpoint was killed (disconnect/unbind); nothing to resubmit */
		return;
	}

	if (urb->status == 0 && urb->actual_length > 0)
		espnet_core_rx(urb->transfer_buffer, urb->actual_length);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret != 0 && ret != -ENODEV)
		dev_warn(&priv->udev->dev, "espnet: urb resubmit failed: %d\n", ret);
}

static int espnet_usb_probe(struct usb_interface *intf,
			    const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt;
	struct espnet_usb_priv *priv;
	int i, ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->udev = udev;

	/* pick the first IN bulk endpoint of the current altsetting */
	alt = intf->cur_altsetting;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
		if (usb_endpoint_dir_in(ep) && usb_endpoint_xfer_bulk(ep)) {
			priv->in_ep = ep;
			break;
		}
	}
	if (!priv->in_ep) {
		kfree(priv);
		return -ENODEV;
	}

	priv->buf = usb_alloc_coherent(udev, ESPNET_URB_BUF, GFP_KERNEL,
				       &priv->dma);
	if (!priv->buf) {
		kfree(priv);
		return -ENOMEM;
	}

	priv->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!priv->urb) {
		usb_free_coherent(udev, ESPNET_URB_BUF, priv->buf, priv->dma);
		kfree(priv);
		return -ENOMEM;
	}

	usb_fill_bulk_urb(priv->urb, udev,
			  usb_rcvbulkpipe(udev, priv->in_ep->bEndpointAddress),
			  priv->buf, ESPNET_URB_BUF,
			  espnet_usb_complete, priv);
	priv->urb->transfer_dma = priv->dma;
	priv->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_set_intfdata(intf, priv);

	ret = usb_submit_urb(priv->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&intf->dev, "espnet: submit in-urb failed: %d\n", ret);
		usb_kill_urb(priv->urb);
		usb_free_urb(priv->urb);
		usb_free_coherent(udev, ESPNET_URB_BUF, priv->buf, priv->dma);
		kfree(priv);
		return ret;
	}

	dev_info(&intf->dev, "espnet: bound ESP32 tether (bulk IN ep 0x%02x)\n",
		 priv->in_ep->bEndpointAddress);
	return 0;
}

static void espnet_usb_disconnect(struct usb_interface *intf)
{
	struct espnet_usb_priv *priv = usb_get_intfdata(intf);
	struct usb_device *udev;

	if (!priv)
		return;

	usb_set_intfdata(intf, NULL);
	udev = priv->udev;

	usb_kill_urb(priv->urb);
	usb_free_urb(priv->urb);
	usb_free_coherent(udev, ESPNET_URB_BUF, priv->buf, priv->dma);
	kfree(priv);

	dev_info(&intf->dev, "espnet: ESP32 tether detached\n");
}

static struct usb_driver espnet_usb_driver = {
	.name       = "espnet_usb",
	.probe      = espnet_usb_probe,
	.disconnect = espnet_usb_disconnect,
	.id_table   = espnet_ids,
};

module_usb_driver(espnet_usb_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Blue project");
MODULE_DESCRIPTION("USB host driver for the ESP32 tether telemetry link");
MODULE_VERSION("0.1");