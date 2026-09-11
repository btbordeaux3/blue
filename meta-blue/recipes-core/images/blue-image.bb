# blue-image.bb - product image assembling the whole stack from this repo:
#   systemd + NetworkManager + esp-wifi-agent (policy)
#   espnet kernel modules (ESP32 tether + VBUS rail; autoloaded)
#   blue-vbus overlay baked into the kernel (see recipes-kernel/linux)

inherit core-image

SUMMARY = "Blue product image"
DESCRIPTION = "Bootable image for the handheld: the ESP32 extender handoff \
daemon, its kernel driver support, and the device-tree power description."

IMAGE_INSTALL:append = " \
    packagegroup-core-boot \
    systemd \
    networkmanager \
    esp-wifi-agent \
    espnet \
    kernel-modules \
    udev-extraconf \
"

IMAGE_FEATURES:append = " ssh-server-dropbear tools-debug"

IMAGE_ROOTFS_SIZE = "524288"