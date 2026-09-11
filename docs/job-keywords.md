# Job-keyword map

Every item a job posting / interviewer is likely to probe, where it lives,
and the one-liner to say about it.

| Posting keyword | Where it lives | One-liner |
|---|---|---|
| Embedded Linux / BSP | `meta-blue/` (Yocto layer) | "I package the whole stack into a bootable image per product." |
| Yocto / OpenEmbedded | `meta-blue/conf/machine/blue-handheld.conf`, `kas.yml`, image/recipes | "kernel module via `inherit module`, daemon via `inherit cmake systemd`, overlay baked via `KERNEL_DEVICETREE`." |
| Device tree / overlays | `dt-overlay/blue-vbus.dts`, linux bbappend | "Hardware is described in DT: the ESP32's power rail binds the driver — change a GPIO in the tree, not the code." |
| Kernel / device drivers | `kmod/espnet_{core,usb,vbus}.c` | "miscdevice + ioctl/sysfs, a USB bulk-IN URB streaming driver, and a DT-bound platform driver." |
| USB / usbcore / URBs | `kmod/espnet_usb.c` | "Binds the ESP32 tether, streams telemetry into a kfifo exposed to user space." |
| C / CMake / cross-compile | `agent/CMakeLists.txt`, `cmake/Toolchain-*.cmake` | "Userspace in portable C11 via CMake; pure-C core cross-builds for aarch64/armv7 in CI." |
| systemd / service design | `agent/systemd/esp-wifi-agent.service` | "Type=notify, WatchdogSec, sd_notify over $NOTIFY_SOCKET, sane hardening." |
| D-Bus / IPC | `agent/src/monitor.c` (libnm over D-Bus) | "Policy daemon talks to NetworkManager through its official client library." |
| Daemon / state machine | `agent/src/decision.c`, `state.c` | "Hysteresis + explicit actions; atomic state persistence." |
| Real-time-ish / racing | hysteresis, async activate callbacks | "Flap guard so transient AP blips can't thrash the radio." |
| Testing / faults | `agent/tests/test_decision.c`, `kmod/test/espnet_ct.c` | "Pure decision logic is unit-tested; a mock NM scenario set is CI-able; kernel ABI smoke test." |
| CI / reproducibility | `ci/*.yml`, `meta-blue/kas.yml` | "Host build+test, cross-aarch64 compile of core, Kbuild matrix, and a manual full-image build." |
| Bootloader / initramfs | `docs/boot-flow.md` (narrative) | "Boot flow documented end-to-end: U-Boot → DT → kernel → initramfs → systemd → agent." |
| Power-aware design | `espnet_vbus`, scan interval | "Extender powered while host awake; agent reduces scan cadence; firmware deep-sleeps the RF path." |

## Resume bullets (steal these)

- Built an **embedded-Linux roaming agent**: systemd daemon watching
  NetworkManager over **D-Bus/libnm**, with **hysteresis** and atomic state
  persistence (arch: `agent/`).
- Wrote **three kernel modules** (char device + ioctl/sysfs ABI, USB driver
  with URB streaming, DT-bound power driver) with a userspace ABI smoke
  test (`kmod/`).
- Authored a **device tree overlay** and a **Yocto layer** that packages the
  daemon, kernel modules and overlay into a bootable **image** (`meta-blue/`,
  `dt-overlay/`).
- Maintained **CI** covering host tests, aarch64/armv7 **cross-compilation**
  and a manual full **image build** (`ci/`).
- Co-designed the full stack with **ESP32 firmware**: the mobile node makes
  RF decisions, the host acts on wired telemetry — "kernel = mechanism,
  userspace = policy".