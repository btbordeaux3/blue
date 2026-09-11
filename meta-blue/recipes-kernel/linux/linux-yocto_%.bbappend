# Bake the blue-vbus device tree overlay into the product kernel so the
# USB-C power rail (and its enabling GPIO) is described on first boot.
#
# How device tree works with Yocto: KERNEL_DEVICETREE lists entries the
# kernel build turns into .dtb/.dtbo files; the kernel's dtc pipeline needs
# the overlay *source* inside arch/<ARCH>/boot/dts. We fetch it from the
# dt-overlay/ directory in this repo (files/blue-vbus.dts.tar.gz snapshot)
# and drop it in during do_configure. Adapt the arch dir to your BSP.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# blue-handheld is our own machine - accept the stock linux-yocto kernels
COMPATIBLE_MACHINE:blue-handheld = ".*"

SRC_URI:append = " file://blue-vbus.dts.tar.gz"

KERNEL_DEVICETREE += "blue-vbus.dtbo"

do_configure:prepend() {
    for f in $(find ${WORKDIR} -maxdepth 2 -name 'blue-vbus.dts' -print); do
        install -D -m 0644 "$f" \
            ${STAGING_KERNEL_DIR}/arch/${ARCH}/boot/dts/blue-vbus.dts
    done || true
}