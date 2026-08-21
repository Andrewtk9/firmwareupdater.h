#pragma once

// The configuration surface. Everything a project owner sets lives here.
//
// Precedence, highest wins:
//   1. remote config from /config/<id>   (only the spec's tunables)
//   2. provisioned values in NVS         (device_id, mqtt_*, api_base_url, topics)
//   3. this struct, passed to begin()
//   4. the defaults below
//
// Provisioned values beat compile-time endpoints unconditionally once
// provisioned. force_endpoints is the single, loudly-logged escape hatch.

#include "campodata/Types.h"

namespace campodata {

struct WifiConfig {
    const char* ssid = nullptr;
    const char* pass = nullptr;
    // 8.5 dBm keeps the Wi-Fi TX transient off the modem's 2 A TDMA bursts.
    int8_t   tx_power_dbm      = 8;
    uint32_t connect_timeout_ms = 20000;
};

struct GprsConfig {
    const char* apn     = nullptr;
    const char* user    = nullptr;
    const char* pass    = nullptr;
    const char* sim_pin = nullptr;

    int8_t   pin_tx       = -1;
    int8_t   pin_rx       = -1;
    int8_t   pin_pwrkey   = -1;
    int8_t   pin_reset    = -1;
    int8_t   pin_power_en = -1;
    uint32_t baud         = 115200;

    // SIM800L V2 self-powers on rail-up and needs to settle before AT works.
    uint32_t boot_settle_ms = 8000;

    // Separate muxes: sharing one means every HTTP stop() closes the MQTT socket.
    uint8_t mux_mqtt = 0;
    uint8_t mux_http = 1;
};

// Broker tuning only.
//
// Host, port, tls, username, password and the three topic strings all arrive in
// the provisioning response and are persisted to NVS - the library never needs
// them compiled in. What is left here is what the server does not send: the
// trust anchor and the client-side timing knobs.
struct MqttTuning {
    // The provisioning response says whether to use TLS, but not which CA to
    // trust, so the certificate still has to come from the project.
    // Validation needs a valid wall clock and is forced off until there is one.
    bool        verify_ca = false;
    const char* ca_pem    = nullptr;

    // 2048 fits this protocol's largest message (~400 B) with headroom. Copying
    // telemetria's 12288 onto a board that also needs a TLS session is how you
    // run out of heap.
    uint16_t buffer_bytes     = 2048;
    uint16_t keepalive_s      = 60;
    uint16_t socket_timeout_s = 30;

    // Update and config are retained, so the broker replays them on reconnect
    // and a persistent session buys nothing.
    bool clean_session = true;
};

// The one place endpoints are defined.
//
// Only the provisioning URL truly has to be compiled in: api_base_url arrives
// in the provisioning response, but the device needs an address to ask in the
// first place. The spec never says where that first URL comes from.
//
// Two hosts because the cellular stack cannot do TLS, so a dual-link device
// needs a plain-HTTP base as well as the TLS one.
//
// Both are placeholders: set them in the project, next to firmware_version and
// repo. Deployment addresses do not belong in a shared library.
struct EndpointConfig {
    const char* api_base_url      = "https://updater.example.com";
    const char* api_base_url_gprs = "http://updater.example.com";

    const char* path_provisioning = "/api/v1/provisioning";
    const char* path_confirm      = "/api/v1/firmware/%s/confirm";  // %s = update_id

    // No path_download: the order carries the full URL. The deployed server
    // builds it from a single configured base, so a cellular device can be
    // handed an https:// URL it cannot fetch - checked at download time.

    const char* auth_bearer  = nullptr;
    const char* ca_pem       = nullptr;
    bool        insecure_tls = true;  // matches current fleet reality
};

struct TopicConfig {
    MqttTopicScheme scheme       = MqttTopicScheme::Pdf;
    const char*     project_slug = nullptr;  // required for SlugEnvRet

    // Hard overrides; provisioning-supplied topics take precedence over these.
    const char* t_ping   = nullptr;
    const char* t_update = nullptr;
    const char* t_config = nullptr;
};

struct OtaPolicy {
    OtaLinkPolicy link_policy        = OtaLinkPolicy::WifiOnly;
    bool          allow_ota_on_gprs  = false;
    bool          require_button     = true;

    // mandatory may waive the button. It never waives the link policy: that
    // guard lives in LinkManager::dataLink() and has no bypass.
    bool     mandatory_waives_button = true;
    uint16_t button_window_s         = 60;

    uint32_t chunk_bytes      = 4096;
    uint32_t budget_ms        = 40;      // per loop(); task WDT panics at 5 s
    uint32_t stall_timeout_ms = 300000;
    uint8_t  max_attempts     = 3;

    // How long the new image has to confirm before we force a rollback. Covers
    // the "boots but hangs" case, which bootloader rollback alone cannot catch.
    uint16_t rollback_deadline_s = 300;

    // Anti-rollback eFuse is unavailable, so downgrade refusal is done here.
    bool allow_downgrade = false;
};

struct PowerPolicy {
    bool defer_wifi_while_modem_busy = true;
    bool quiesce_modem_during_ota    = true;
    bool wifi_off_when_idle          = false;
};

struct Config {
    const char* firmware_version        = nullptr;  // required
    const char* repo                    = nullptr;  // required
    const char* hardware_model_override = nullptr;

    LinkMode       link_mode       = LinkMode::Wifi;
    LinkPreference link_preference = LinkPreference::GprsPreferred;
    MqttBackend    mqtt_backend    = MqttBackend::Auto;
    ClockPolicy    clock_policy    = ClockPolicy::BestEffort;

    WifiConfig     wifi;
    GprsConfig     gprs;
    MqttTuning     mqtt;
    EndpointConfig endpoints;
    TopicConfig    topics;
    OtaPolicy      ota;
    PowerPolicy    power;

    uint16_t ping_interval_s = 60;
    bool     force_endpoints = false;
    bool     subscribe_task_wdt = false;

    static Config defaults();
    ConfigError   validate() const;
};

}  // namespace campodata
