# esp-wifi-agent_0.1.bb - userspace roaming policy daemon.
#
# Canonical sources: agent/ at the repo root, snapshotted into
# files/esp-wifi-agent.tar.gz. Built with CMake (inherit cmake), installed
# as a systemd service (inherit systemd), depending on the NetworkManager
# client library (libnm) + glib, exactly as on the host build.

SUMMARY = "esp-wifi-agent - ESP32 extender WiFi handoff daemon"
DESCRIPTION = "Systemd daemon that watches NetworkManager over D-Bus and \
connects to the USB-C tethered ESP32's softAP when it appears, then \
restores the previous network (or NM's best available) when it disappears."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "glib-2.0 networkmanager pkgconfig-native"
RDEPENDS:${PN} += "networkmanager"

inherit cmake systemd

SRC_URI = "file://esp-wifi-agent.tar.gz"

S = "${WORKDIR}"

EXTRA_OECMAKE += "-DENABLE_INSTALL=ON -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release"

SYSTEMD_SERVICE:${PN} = "esp-wifi-agent.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# runtime state directory for the "previous network" persistence
do_install:append() {
    install -d ${D}${localstatedir}/lib/esp-wifi-agent
    install -d ${D}${sysconfdir}/tmpfiles.d
    printf 'd %s/lib/esp-wifi-agent 0755 root root -\n' \
        "${localstatedir}" > ${D}${sysconfdir}/tmpfiles.d/esp-wifi-agent.conf
}

FILES:${PN} += "${sysconfdir}/esp-wifi-agent.conf \
                ${sysconfdir}/tmpfiles.d/esp-wifi-agent.conf"