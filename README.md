# FirmwareUpdater v2

Provisioning, heartbeat, remote configuration and OTA updates for ESP32 devices,
over MQTT for control and HTTPS for the firmware image.

Implements the *Firmware Updater — Especificação Técnica* v1.0 contract.

> **Work in progress.** The protocol codecs, OTA sink, board detection and
> rollback guard are implemented and tested. The public facade, the link layer
> and the MQTT session are not written yet — see [Status](#status).

## Why v2

The v1 library was copied into each project and diverged into nine mutually
incompatible variants. All of them share the same structural problem: the class
has no member state, so everything lives in file-scope globals, and several
force the application to define symbols like `extern bool Atualizando` just to
link.

v2 is one versioned dependency with real objects, typed errors, a non-blocking
loop, and a hardware-free core that runs under unit tests on a PC.

## What it fixes

Three defects reproduced across the whole fleet, each now covered by a test:

**Rollback never worked.** Arduino-ESP32 ships a weak `verifyRollbackLater()`
that returns `false`, and `initArduino()` — which runs *before* `setup()` —
reacts by calling `esp_ota_mark_app_valid_cancel_rollback()` on any pending
image. The rollback window closes before the application runs a single
instruction. The library defines the strong symbol so the decision stays with
the update flow, gated on the confirm response, and self-tests at boot that the
override actually linked.

**The size cap was larger than the partition.** Every variant hardcodes
2 000 000 bytes while the real OTA slot is `0x1E0000` = 1 966 080. An image in
between downloads completely — twenty minutes of paid cellular data — and only
then fails to flash. v2 reads the real slot size from
`esp_ota_get_next_update_partition()`.

**The modem's timezone was discarded.** `modem.getNetworkTime()` returns an
offset that the fleet reads and throws away, so local time gets stored as if it
were UTC. `Iso8601::fromGsmLocal()` applies it.

## Layout

```
include/campodata/     public headers
src/core/              no Arduino.h, no esp_*, no String — runs on the host
src/platform/esp32/    ESP-IDF C APIs, linkable from Arduino and from ESP-IDF
test/                  Unity suites, run with `pio test -e native`
```

The core talks to the outside only through interfaces, which is what makes the
state machine testable without hardware. The ESP32 layer is built on
`esp_ota_ops`, `esp_http_client`, `esp-mqtt` and mbedTLS rather than on
`Update.h`, `HTTPClient` and `PubSubClient` — those IDF libraries ship inside
the Arduino framework, so the same code serves both targets and a future
ESP-IDF port stays small.

## Configuration

Almost nothing needs configuring, because the broker is not the library's
business: host, port, TLS, username, password and the three topic strings all
arrive in the provisioning response and are persisted to NVS. What the project
supplies is only what is needed to reach the API in the first place, plus its
own identity.

Copy `examples/fwup_secrets.example.h` to your project as `fwup_secrets.h`
(gitignored) and fill it in.

```cpp
#include <campodata/FirmwareUpdater.h>
#include "fwup_secrets.h"

campodata::Config cfg;
cfg.link_mode              = campodata::LinkMode::Wifi;   // Wifi | Gprs | Both
cfg.firmware_version       = "2.0.0";
cfg.repo                   = "campotech/medidor-potencia-ac";
cfg.endpoints.api_base_url = FWUP_API_BASE_URL;
cfg.wifi                   = { WIFI_SSID, WIFI_PASS };
cfg.buttonPin              = 0;
```

`firmware_version` and `repo` are per project and belong in `main.cpp`: one
library serves many firmwares.

Precedence, highest first: remote config from `/config/<id>`, then values
provisioned into NVS, then this struct, then the defaults.

## Testing

```bash
pio test -e native      # host tests, no hardware and no server
pio run -e esp32dev     # compiles the library plus examples/BoardInfo
```

Run `examples/BoardInfo` first on any new hardware. It prints the detected
`hardware_model`, the real OTA slot size, and whether the rollback window is
actually open — if it reports `bypassed`, the strong `verifyRollbackLater()`
did not link and automatic rollback would silently never happen.

## Status

| Area | State |
|---|---|
| Provisioning / order / config / ping codecs | done, tested |
| Topic resolution, versions, backoff, ISO 8601 | done, tested |
| Board detection (ESP32-S3 / WROVER / esp32dev) | done, tested |
| OTA sink, streaming SHA-256, rollback guard | done, compiles on all three targets |
| NVS persistence | not started |
| Update state machine | not started |
| Link layer, MQTT session, HTTP client | not started |
| Public `FirmwareUpdater` facade | not started |
| ESP-IDF port | planned |

`src/firmwareupdater.{h,cpp}` is the v1 library, kept so existing projects keep
building. It is excluded from the v2 build and will be removed once every
project has migrated.

## License

MIT
