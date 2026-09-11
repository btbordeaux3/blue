# Blue — adaptive WiFi extender system

The ESP32 side (PlatformIO firmware) is covered in `src/`, `include/`.
This Linux side automates the companion host:

```
blue/
├── src/  include/  lib/   ...   ESP32 firmware (base + mobile units)
├── agent/        systemd daemon: roam between router and the ESP32 softAP
│                 (CMake, libnm/D-Bus, hysteresis, USB telemetry, unit tests)
├── kmod/         kernel modules: /dev/espnet char device, ESP32 USB tether
│                 driver, DT-bound VBUS power rail driver (Kbuild + DKMS)
├── dt-overlay/   device tree overlay describing the ESP32 power rail
├── meta-blue/    Yocto layer packaging everything into a bootable image
├── ci/           GitHub Actions (host build+tests, cross-compile, Yocto)
└── docs/         architecture, boot flow, and a job-keyword map
```

## The story

An ESP32 mobile unit is **tethered to the handled host over USB-C** (power
from the host's 5V rail + a serial telemetry link). It creates a `3bbo_Ext`
softAP when coverage is weak. This daemon notices — via both the **wired**
USB telemetry and a **wifi scan** — connects to it, then restores the
previous network when the AP goes away.

The mobile node is **zero-config**: the daemon provisions the current
network's SSID + password to it over the same wire (boot event or network
change), so the extender always joins whatever the handheld is connected
to — nothing is baked into the firmware.

Behind that, the **base station runs a TinyML classifier** (7-feature online
perceptron: AP-signature + RSSI + time-of-day) that learns your weak zones
and dead-zone hours from real reports, then pre-emptively commands the
repeater on through `3bbo_Ext` before the link collapses — surfacing on the
dashboard whether it was the **base's ML prediction** or the **mobile's own
low-signal detection** that kicked in.

**Kernel is mechanism, userspace is policy:** drivers own the hardware and
the ABI; the daemon owns the decisions.

## Firmware targets and safe parking mode

There are three deliberately separate ESP32 firmware environments:

| Target | Board | Purpose |
|---|---|---|
| `base` | ESP32-S3 DevKitC-1 | Stationary base station; this is the PlatformIO default build target. |
| `base-wroom` | ESP32 DevKit | Base firmware for a classic ESP32 board. |
| `mobile` | ESP32 DevKit | The production mobile firmware in `src/mobile/`; this is preserved separately and is not replaced by the safe image. |
| `mobile-safe` | ESP32 DevKit | Inert parking image in `src/mobile-safe/`; it does not initialize Wi‑Fi, ESPNOW, USB commands, the repeater, or project tasks. |

The base and mobile units communicate over the project's wireless protocol, while
USB-C is the wired host link to the mobile board. Because flashing the base can
change Wi‑Fi and disconnect the host, use `mobile-safe` as a temporary parking
firmware while preparing or debugging the real mobile image:

```sh
# Build only; these commands do not touch a board.
pio run -e mobile-safe
pio run -e mobile

# Review the connected board and port before any upload.
pio device list

# Explicit upload commands; run only after confirming the selected mobile port.
pio run -e mobile-safe -t upload   # temporary inert image
pio run -e mobile -t upload        # restore the production mobile image
```

**Important:** `pio run` only builds. Do not use an upload command until the
correct mobile ESP32 serial port is identified and the hardware is ready. Never
use the `base` environment when the intended board is the mobile ESP32. The
`mobile-safe` source is separate from `src/mobile/`, so building or flashing it
does not overwrite the production mobile firmware source. The safe image has no
Wi‑Fi startup path and therefore cannot switch the base network; it only idles.

Recommended handoff sequence:

1. Build `mobile-safe` and `mobile` without uploading either image.
2. Confirm the serial port belongs to the mobile ESP32, not the base station.
3. Flash `mobile-safe` only when you explicitly want to park the mobile board.
4. Prepare and build the production `mobile` image while the base connection is stable.
5. Flash `mobile` only when the mobile board is connected and you are ready to restore its behavior.
6. Verify the production mobile logs and the base/mobile link before resuming normal operation.


```
sudo pacman -S base-devel cmake device-tree-compiler      # SteamOS: build tools
cmake -S agent -B build -DBUILD_TESTS=ON && cmake --build build
ctest --test-dir build --output-on-failure                # decision engine tests

sudo cmake --install build                                # binary + unit + config
sudo systemctl enable --now esp-wifi-agent
journalctl -u esp-wifi-agent -f
```

Kernel modules (source landings — driver binds to the ESP32 tether):

```
make -C kmod            # Kbuild against the running kernel
sudo insmod kmod/espnet_core.ko
sudo modprobe kmod/espnet_usb.ko   # optional USB tether
sudo insmod kmod/espnet_vbus.ko    # needs the DT node to probe
make -C kmod/test && sudo ./kmod/test/espnet_ct
```

## Dive deeper

- `docs/architecture.md`  — one handoff, top to bottom
- `docs/boot-flow.md`     — power-on to connected
- `docs/job-keywords.md`  — interview / resume map
- `dt-overlay/README.md`  — overlay build + live apply
- `meta-blue/README.md`   — Yocto build (`kas build meta-blue/kas.yml`)