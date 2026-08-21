#include "core/PingBuilder.h"

#include <ArduinoJson.h>

#include "core/Iso8601.h"

namespace campodata {

CodecError PingBuilder::build(const PingSnapshot& snap, bool dual_link,
                              char* out, size_t cap) const {
    if (out == nullptr || cap == 0) return CodecError::BufferTooSmall;

    JsonDocument doc;

    // Spec section 6. A device with no clock omits ts rather than inventing
    // one: a fabricated epoch silently corrupts the server's time series,
    // while a missing field is visible. ts_source says why.
    if (snap.ts > 0 && iso8601::plausible(snap.ts)) {
        char ts[iso8601::kBufferLen];
        if (iso8601::format(snap.ts, ts, sizeof(ts))) doc["ts"] = ts;
    }

    doc["firmware_version"] = snap.firmware_version ? snap.firmware_version : "";
    doc["repo"]             = snap.repo ? snap.repo : "";
    doc["uptime_s"]         = snap.uptime_s;
    doc["link"]             = toString(snap.link);
    doc["rssi"]             = snap.rssi;
    doc["free_heap"]        = snap.free_heap;
    doc["ota_state"]        = toString(snap.ota_state);
    doc["config_version"]   = snap.config_version;

    // The spec's flow shows a reason alongside aborted and failed.
    if (snap.abort_reason != AbortReason::None) {
        doc["reason"] = toString(snap.abort_reason);
    }

    // Additive fields, documented in docs/SPEC-EXTENSIONS.md. Only emitted in
    // Both mode: a single-link device produces exactly the spec's payload, so
    // an unmodified server sees nothing new.
    if (dual_link) {
        JsonObject links = doc["links"].to<JsonObject>();
        JsonObject wifi  = links["wifi"].to<JsonObject>();
        wifi["up"]   = snap.wifi_up;
        wifi["rssi"] = snap.wifi_rssi;
        JsonObject gprs = links["gprs"].to<JsonObject>();
        gprs["up"]   = snap.gprs_up;
        gprs["rssi"] = snap.gprs_rssi;

        doc["ota_link_ready"]       = snap.ota_link_ready;
        doc["ota_transport_secure"] = snap.ota_transport_secure;
    }

    if (snap.ts_source != ClockSource::None) {
        switch (snap.ts_source) {
            case ClockSource::Sntp:   doc["ts_source"] = "sntp";   break;
            case ClockSource::Gsm:    doc["ts_source"] = "gsm";    break;
            case ClockSource::Rtc:    doc["ts_source"] = "rtc";    break;
            case ClockSource::Server: doc["ts_source"] = "server"; break;
            default: break;
        }
    }

    if (_extender != nullptr) {
        JsonObject root = doc.as<JsonObject>();
        _extender(&root, _ctx);
    }

    const size_t written = serializeJson(doc, out, cap);
    if (written == 0 || written >= cap) return CodecError::BufferTooSmall;
    return CodecError::Ok;
}

}  // namespace campodata
