# meta-blue — Yocto/OpenEmbedded layer for the handheld BSP

This layer packages every layer of the repo into a bootable product image:

| Recipe | What it builds | Source of truth |
|---|---|---|
| `espnet_0.1.bb` | `espnet_core` / `espnet_usb` / `espnet_vbus` kernel modules (autoloaded) | `kmod/` |
| `esp-wifi-agent_0.1.bb` | the handoff daemon (cmake) + systemd unit | `agent/` |
| `linux-yocto_%.bbappend` | bakes `blue-vbus.dtbo` into `KERNEL_DEVICETREE` | `dt-overlay/` |
| `blue-image.bb` | full rootfs image (systemd + NM + agent + modules) | — |

Recipe inputs are checked-in snapshots under each recipe's `files/`. After
changing the canonical source, refresh a snapshot:

    tar czf meta-blue/recipes-kernel/espnet/files/espnet.tar.gz \
        -C kmod Makefile espnet.h espnet_core.c espnet_usb.c espnet_vbus.c
    tar czf meta-blue/recipes-connectivity/esp-wifi-agent/files/esp-wifi-agent.tar.gz \
        -C agent .

## Build (reproducible, container-friendly)

    pip install kas
    kas build meta-blue/kas.yml

Or manually with `bitbake` after `source oe-init-build-env`. The reference
machine is `blue-handheld` (ARMv8-A; boots under QEMU). Swap in a vendor
SoC machine by overriding `MACHINE` in `kas.yml`.