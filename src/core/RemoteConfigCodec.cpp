#include "core/RemoteConfigCodec.h"

#include <ArduinoJson.h>

#include "core/Iso8601.h"

namespace campodata {
namespace remoteconfig {

CodecError parse(const char* json, RemoteConfig& out) {
    if (json == nullptr) return CodecError::MalformedJson;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        return CodecError::MalformedJson;
    }

    const long long version = doc["config_version"] | -1LL;
    if (version < 0) return CodecError::MissingField;
    out.config_version = static_cast<uint32_t>(version);

    // Keep the current value when a field is absent: a document that mentions
    // only one knob must not silently reset the others.
    out.ping_interval_s     = doc["ping_interval_s"]     | out.ping_interval_s;
    out.ota_button_window_s = doc["ota_button_window_s"] | out.ota_button_window_s;
    out.allow_ota_on_gprs   = doc["allow_ota_on_gprs"]   | out.allow_ota_on_gprs;
    out.mqtt_keepalive_s    = doc["mqtt_keepalive_s"]    | out.mqtt_keepalive_s;
    out.applied_at_required = doc["applied_at_required"] | out.applied_at_required;

    JsonObjectConst sensors = doc["sensors"];
    if (!sensors.isNull()) {
        const long long interval = sensors["read_interval_s"] | -1LL;
        if (interval > 0) {
            out.sensor_read_interval_s = static_cast<uint32_t>(interval);
            out.has_sensor_interval    = true;
        }
    }

    // Extension: lets a device with no clock source of its own timestamp its
    // pings, since /config is retained and costs no extra round trip.
    const char* server_time = doc["server_time"] | static_cast<const char*>(nullptr);
    if (server_time != nullptr) {
        int64_t epoch = 0;
        if (iso8601::parse(server_time, epoch) && iso8601::plausible(epoch)) {
            out.server_time = epoch;
        }
    }

    // A ping interval of zero would busy-publish; clamp rather than obey.
    if (out.ping_interval_s == 0) out.ping_interval_s = 60;

    return CodecError::Ok;
}

CodecError buildAck(const RemoteConfig& cfg, const char* device_id,
                    char* out, size_t cap) {
    if (out == nullptr || cap == 0) return CodecError::BufferTooSmall;

    JsonDocument doc;
    doc["device_id"]      = device_id ? device_id : "";
    doc["config_version"] = cfg.config_version;

    const size_t written = serializeJson(doc, out, cap);
    if (written == 0 || written >= cap) return CodecError::BufferTooSmall;
    return CodecError::Ok;
}

}  // namespace remoteconfig
}  // namespace campodata
