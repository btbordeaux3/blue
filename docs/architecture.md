# Architecture — Blue: ESP32 extender handoff system

```
                over the air (WiFi)
   +------------------- 3bbo_Ext softAP  -------------------+
   |                                                        |
+------------------+           +-------------------------------+
| ESP32 mobile     |  USB-C   | Handheld Linux host           |
| (firmware/)      |  power   | (this repo's linux/ layers)   |
|  RF scanning     |  +       |                               |
|  power_manager   |  serial  |  device tree   blue-vbus.dts  |
|  softAP (3bbo_Ext)| telemetry|      | probe                  |
|                  |<-------->|  +---------------------+      |
|  AP_UP/AP_DOWN   |  JSON    |  | espnet_vbus (kmod)  |      |
+------------------+          |  +---------------------+      |
                              |  espnet_usb --/dev/espnet     |
                              |  espnet_core  (FIFO,ioctl)    |
                              |          ^ publish state      |
                              |          | ioctl               |
                              |  +---------------------+      |
                              |  | esp-wifi-agent       |      |
                              |  | decision engine      |      |
                              |  | scan + USB fused     |      |
                              |  +--------+------------+      |
                              |           | D-Bus             |
                              |  +--------v------------+      |
                              |  | NetworkManager (libnm)|     |
                              |  +---------------------+      |
                              +-------------------------------+
```

## Node-side intelligence (ESP32 firmware)

The two nodes split the decision so the dashboard can always say *who* turned
the extender on:

| Trigger | Actor | Mechanism | Website label |
|---|---|---|---|
| Current signal is weak *right now* | mobile | `power_manager` RSSI threshold in `STATE_EVALUATE` | `mobile_low` |
| Heading into a known-bad zone/time | base | on-device TinyML classifier, commands over ESPNOW | `base_ml` |

**TinyML on the base.** Every fingerprint the mobile reports is also a
*labelled training sample*: label = "was the mobile weak at that instant?".
`command_task` builds a 7-feature vector and hands it to `ml_task`:

- features 0-3: BSSIDs feature-hashed into 4 signature bins (the *place*)
- feature 4: current top-3 RSSI (scaled)
- features 5-6: `sin/cos(2π·hour/24)` — cyclical time (the *when*, so 23:00
  reads as adjacent to 00:00, not 23 hours away)

Learning is a single-layer perceptron (logistic regression) with one SGD step
per sample, over a **256-sample ring buffer that evicts the oldest entry at
capacity**; the model is re-fit from the live ring after eviction so stale
signatures roll off (concept-drift handling). Model + ring persist to SPIFFS
(`/ml_model.bin`, magic-versioned).

**Cold start is explicit:** the base sends *no* commands until it has
`ML_MIN_SAMPLES` samples **and** `ML_MIN_POS` weak ones — early on, the
mobile's own RSSI logic covers everything. Actions are confidence-gated
(`p≥0.62` to activate, `p≤0.38` to recover), and the base only deactivates
what it activated.

The mobile echoes `activation_source` + confidence in its next fingerprint;
the base pushes a decision event (`push_task → /api/decision`), and the
Cloudflare worker merges it into `/api/status` for the dashboard's
"Repeater ON — Base ML prediction (82%)" / "ON — Mobile detected low
signal" cards. The raw RF map (`rf_map.dat`) is independently bounded to
`MAP_MAX_ENTRIES` (FIFO trim in `storage_task`).

## Control flow (one handoff)

1. **ESP32 mobile** boots, powered by the host's USB-C 5V rail. It runs the
   existing PlatformIO firmware; its `power_manager` state machine decides
   when coverage is weak and brings up the `3bbo_Ext` softAP.
2. **Provision (host -> ESP32):** the agent reads the host's *current*
   network (SSID + PSK via libnm, secrets as root) and pushes it over the
   USB wire as `{"cmd":"net","ssid":..,"psk":..}` — whenever the network
   changes or the ESP32 emits `{"evt":"BOOT"}`. No credentials in firmware.
3. **Wired hint:** the ESP32 emits one-line JSON over its USB console
   (`{"evt":"AP_UP",...}`). The daemon's `usb_monitor` latches it.
4. **Wireless hint:** NetworkManager's driver layer sees the new AP and the
   daemon's `monitor` (libnm/D-Bus) requests a scan every `scan_interval_sec`.
5. **Decide:** `decision.c` fuses both hints, feeds the hysteresis counters
   (`req_seen`/`req_missed` — no flapping on blips), and picks an action:
   CONNECT_EXT / RESTORE_PREVIOUS / FALLBACK / NONE.
6. **Act:** connect to the extender (profile auto-created), stashing the
   current connection as `previous` in the state file. On AP loss: restore
   `previous` if still in range, else disconnect and let NM autoconnect.
7. **Kernel publish:** state is mirrored into `espnet_core` via
   `ESPNET_IOCTL_SET_AP`, readable by any client through
   `ESPNET_IOCTL_GET_STATE` / sysfs.

## Layer principle

- **Kernel = mechanism** (`kmod/`): char device, URB streaming, DT-bound
  power rail, ioctl/sysfs ABI.
- **Userspace = policy** (`agent/`): hysteresis, fallback order, state
  persistence, commands. If the daemon dies, the kernel knows nothing
  beyond last state — that's by design (watchdog restarts the agent).

## Build layers

- Firmware: `pio run -e base` / `pio run -e mobile` (PlatformIO)
- Host build & unit tests: `cmake -S agent -B build && cmake --build build`
- Kernel: `make -C kmod` (Kbuild)
- Product image: `kas build meta-blue/kas.yml` (Yocto)
- CI: `ci/*.yml` (host, cross-aarch64, and manual Yocto)