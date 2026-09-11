# Device tree overlay: blue-vbus

Describes the USB-C 5V rail that powers the tethered ESP32 mobile unit.
The `espnet_vbus` kernel driver probes against the `blue,esp32-vbus`
compatible string, so driver and overlay must agree.

## Why an overlay and not a copy of the board DTS?

The product SoC's base `.dts` lives with the kernel/BSP; overlays let you
(1) experiment without rebuilding the kernel and (2) demonstrate live
hw-behavior changes at runtime — exactly what a board bring-up engineer
does on real hardware.

## Build

    make -C dt-overlay      # produces blue-vbus.dtbo (uses dtc)

## Apply live (configfs overlay support)

    sudo -s
    modprobe configfs
    mount -t configfs configfs /sys/kernel/config
    mkdir -p /sys/kernel/config/device-tree/overlays/blue-vbus
    cat blue-vbus.dtbo > /sys/kernel/config/device-tree/overlays/blue-vbus/dtbo
    # verify the driver probed:
    dmesg | tail
    ls /sys/devices/platform/esp32_vbus/
    cat /sys/devices/platform/esp32_vbus/power_enabled   # 1

To remove:  rmdir /sys/kernel/config/device-tree/overlays/blue-vbus

## Package into the product kernel

The meta-blue Yocto layer bbappends the linux kernel recipe so
`blue-vbus.dtbo` is built into KERNEL_DEVICETREE — see
meta-blue/recipes-kernel/linux/.