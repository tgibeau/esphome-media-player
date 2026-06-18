# GitHub Copilot Workspace Instructions

## Project Summary

ESPHome-based music dashboard firmware for ESP32 touch panels. Displays Sonos
playback state (track, artist, artwork, position) pulled via UPnP/SOAP directly
from the speaker — no Home Assistant required for the Sonos Direct mode.

## Build & Deploy (Dev Device)

**Target device**: Guition ESP32-S3 4848S040 (480×480, 8 MB PSRAM)  
**Dev config**: `devices/guition-esp32-s3-4848s040/dev.yaml`  
**USB serial**: `/dev/cu.usbserial-2110`  
**Device IP**: 192.168.1.118  
**Sonos speaker IP**: 192.168.1.67 (port 1400)

```bash
# Compile
esphome compile devices/guition-esp32-s3-4848s040/dev.yaml

# Flash over USB
esphome upload --device /dev/cu.usbserial-2110 devices/guition-esp32-s3-4848s040/dev.yaml

# Compile + flash in one step
esphome run --device /dev/cu.usbserial-2110 devices/guition-esp32-s3-4848s040/dev.yaml

# Monitor logs (USB)
esphome logs --device /dev/cu.usbserial-2110 devices/guition-esp32-s3-4848s040/dev.yaml

# Monitor logs (OTA over WiFi)
esphome logs --device 192.168.1.118 devices/guition-esp32-s3-4848s040/dev.yaml
```

## Repository Layout

```
builds/              Factory + OTA YAML entry points for all devices (used by CI)
common/
  addon/             Feature packages (music, backlight, network, sonos_direct, …)
  device/            Base device config, sensor definitions, sonos_select
  setup/             First-boot / WiFi setup screens
components/
  artwork_image/     Custom ESPHome component: downloads + renders album art
  sonos_player/      Custom ESPHome component: Sonos UPnP/SOAP polling
  mipi_rgb/          Display driver for RGB panels
  gsl3680/           Touch controller driver
devices/
  guition-esp32-s3-4848s040/   Main dev device; dev.yaml is the test config
docs/                User-facing documentation (Docusaurus)
scripts/             Release & changelog automation
```

## Custom Component: sonos_player

Source: `components/sonos_player/`

### Architecture

- **`SonosPlayer::setup()`** — spawns a FreeRTOS task (`sonos_soap`, pinned to
  APP_CPU / core 1) that owns a persistent `esp_http_client` handle.
- **`SonosPlayer::update()`** (1 s interval) — increments `local_position_`
  locally (no network), then every `kSoapPollDivisor` seconds sends
  `xTaskNotifyGive()` to wake the SOAP task.
- **`soap_task_entry_()`** — wakes on notification, runs
  GetTransportInfo → GetPositionInfo → GetVolume (every 5th cycle) using
  stack-based `snprintf` buffers (no heap allocation in hot path).
- **`apply_poll_result_()`** — called via `Component::defer()` on the main
  loop; updates `local_state_`, `local_position_`, `local_duration_` and
  publishes all sensors except `position_sensor_` (which is published only
  from `update()` to prevent double-publish / 0-flash).

### Key constants (`sonos_player.h`)
| Constant | Value | Meaning |
|---|---|---|
| `kSoapPollDivisor` | 3 | SOAP polls every 3 s |
| `kVolumePollDivisor` | 5 | Volume polled every 5th SOAP cycle (~15 s) |
| `kMaxLoggedFailures` | 3 | Log first 3 consecutive failures |
| `kBackoffCycles` | 6 | Skip 6 update() ticks after repeated failures |

### Known issues / mitigations

| Issue | Fix |
|---|---|
| `phy_track_pll_init` abort (ESP32-S3 v0.2 WiFi PHY bug) | Task pinned to core 1; `wifi: power_save_mode: none` in dev.yaml |
| `std::bad_alloc` → abort in SOAP task | SOAP envelope built with `char[512]` + `snprintf` — no heap alloc |
| API entity-list OOM (`try_send_select_info`) | `ha_config_internal: "true"` in dev.yaml hides large selects from API |
| Socket table exhaustion (TIME_WAIT) | Backoff + persistent HTTP client with `keep_alive_enable: true` |
| Artwork download blocks main loop 2–3 s | `LOCAL_ARTWORK_HTTP_TIMEOUT_MS = 2500` (reduced from 6500) |

## dev.yaml Notes

- `ha_config_internal: "true"` — hides timezone/speaker selects from the API
  so the entity-list response doesn't OOM when an API client connects.
- `wifi: power_save_mode: none` — required to prevent the WiFi PHY abort on
  this chip revision when both the SOAP task and main loop are idle.
- `sonos_default_ip: "192.168.1.67"` — fallback IP written on first boot via
  `on_boot` if NVS has no saved IP.

## All-Devices Compile (CI)

Use the `/compile` skill or run Docker builds against `builds/<slug>.yaml`:

```bash
# Example for one device via Docker (matches CI):
docker run --rm -v "$PWD":/config -w /config \
  ghcr.io/esphome/esphome compile builds/guition-esp32-s3-4848s040.yaml
```

See `.agents/skills/compile/SKILL.md` for the full multi-device workflow.
