# espnet_0.1.bb - kernel modules for the ESP32 tether + VBUS rail.
#
# The canonical sources live in kmod/ at the repo root; this recipe consumes
# a snapshot archive (recipe files/espnet.tar.gz). Regenerate the archive
# after touching kmod/ with:
#   tar czf meta-blue/recipes-kernel/espnet/files/espnet.tar.gz \
#       -C kmod Makefile espnet.h espnet_core.c espnet_usb.c espnet_vbus.c

SUMMARY = "espnet - ESP32 extender kernel modules"
DESCRIPTION = "espnet_core (/dev/espnet char device + state board), \
espnet_usb (ESP32 tether bulk-IN streaming driver) and \
espnet_vbus (DT-bound power rail driver for the USB-C-tethered ESP32)"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = "file://espnet.tar.gz"

S = "${WORKDIR}"

# load at boot
KERNEL_MODULE_AUTOLOAD += "espnet_core espnet_usb espnet_vbus"

RPROVIDES:${PN} = "kernel-module-espnet-core \
                   kernel-module-espnet-usb \
                   kernel-module-espnet-vbus"

# so the device node is predictable regardless of boot order
FILES:${PN} += "${sysconfdir}/modules-load.d/*.conf"