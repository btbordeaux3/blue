# Boot flow — from power-on to a connected handheld

What happens on the *host* side when the power button is pressed, mapped to
each layer of this repo so the story is concrete:

```
  power button
      │
      v  (first stage runs from ROM/SPL — not in this repo)
  U-Boot / bootloader          → loads kernel Image + blue-vbus.dtbo
      │                           (the overlay, baked via meta-blue,
      │                            describes the USB-C 5V rail GPIOs)
      v
  Linux Kernel                 → parses the device tree
      │                           binds espnet_vbus once `esp32_vbus`
      │                           node probes (compatible "blue,esp32-vbus")
      │                           → the ESP32 5V rail is gated ON
      │                           (hardware is described, not hardcoded)
      v
  initramfs / init            → mounts rootfs, hands over to systemd
      v
  systemd                     → starts NetworkManager, espnet modules
      │                           (KERNEL_MODULE_AUTOLOAD in the recipe),
      │                           then esp-wifi-agent.service
      v
  esp-wifi-agent              → Type=notify: tells systemd "READY=1",
      │                           pings WATCHDOG=1 each tick
      v
  /dev/espenet + NetworkManager
                                wlan0 scans · usb_monitor reads telemetry
                                if 3bbo_Ext appears → connect, else
                                previous network → else NM best available
```

## Why this order matters (interview talking points)

- **Device tree before driver:** `espnet_vbus` cannot probe until the DT
  node exists. Changing the rail GPIO is a DT change, not a `#define`
  recompile — the classic "hardware description separated from code".
- **modules.autoload via Yocto:** `KERNEL_MODULE_AUTOLOAD` in the recipe is
  how the product loads drivers at boot without manual `insmod`/rc scripts.
- **systemd as PID 1:** dependency ordering (`After=NetworkManager.service`),
  `Type=notify` readiness, and `WatchdogSec=30` recovery — the agent is a
  first-class citizen of the boot graph, not a script appended at the end.
- **Full-core image from Yocto:** `blue-image.bb` is the difference between
  "I can run programs" and "I shipped a device".