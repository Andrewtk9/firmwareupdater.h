#include "core/ProvisioningCodec.h"

#include <ArduinoJson.h>
#include <string.h>

namespace campodata {
namespace provisioning {
namespace {

// Copies into a fixed field, refusing rather than truncating. A silently
// truncated device_id or MQTT password fails much later and much more
// confusingly than a rejected response.
bool copyField(const char* src, char* dst, size_t cap) {
    if (src == nullptr) return false;
    const size_t len = strlen(src);
    if (len == 0 || len >= cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

}  // namespace

CodecError buildRequest(const ProvisionRequest& req, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return CodecError::BufferTooSmall;
    if (req.board_id == nullptr || req.firmware_version == nullptr ||
        req.repo == nullptr) {
        return CodecError::MissingField;
    }

    JsonDocument doc;
    doc["board_id"]         = req.board_id;
    doc["hardware_model"]   = req.hardware_model ? req.hardware_model : "";
    doc["firmware_version"] = req.firmware_version;
    doc["repo"]             = req.repo;

    // Selects the broker profile server-side. Sent only when the link is known,
    // so an unset value keeps the server's own default rather than forcing one.
    if (req.rede != LinkType::None) {
        doc["rede"] = toString(req.rede);
    }

    const size_t written = serializeJson(doc, out, cap);
    if (written == 0 || written >= cap) return CodecError::BufferTooSmall;
    return CodecError::Ok;
}

CodecError parseResponse(const char* json, Provisioning& out) {
    if (json == nullptr) return CodecError::MalformedJson;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        return CodecError::MalformedJson;
    }

    out = Provisioning{};

    if (!copyField(doc["device_id"] | static_cast<const char*>(nullptr),
                   out.device_id, sizeof(out.device_id))) {
        return CodecError::MissingField;
    }

    JsonObjectConst mqtt = doc["mqtt"];
    if (mqtt.isNull()) return CodecError::MissingField;

    if (!copyField(mqtt["host"] | static_cast<const char*>(nullptr),
                   out.mqtt_host, sizeof(out.mqtt_host))) {
        return CodecError::MissingField;
    }

    const int port = mqtt["port"] | 0;
    if (port <= 0 || port > 65535) return CodecError::BadValue;
    out.mqtt_port = static_cast<uint16_t>(port);

    out.mqtt_tls = mqtt["tls"] | false;

    if (!copyField(mqtt["username"] | static_cast<const char*>(nullptr),
                   out.mqtt_user, sizeof(out.mqtt_user))) {
        return CodecError::MissingField;
    }
    if (!copyField(mqtt["password"] | static_cast<const char*>(nullptr),
                   out.mqtt_pass, sizeof(out.mqtt_pass))) {
        return CodecError::MissingField;
    }

    // Topics are authoritative when present: the server, not the firmware,
    // decides the naming scheme.
    JsonObjectConst topics = mqtt["topics"];
    if (!topics.isNull()) {
        copyField(topics["ping"]   | static_cast<const char*>(nullptr),
                  out.topic_ping,   sizeof(out.topic_ping));
        copyField(topics["update"] | static_cast<const char*>(nullptr),
                  out.topic_update, sizeof(out.topic_update));
        copyField(topics["config"] | static_cast<const char*>(nullptr),
                  out.topic_config, sizeof(out.topic_config));
    }

    if (!copyField(doc["api_base_url"] | static_cast<const char*>(nullptr),
                   out.api_base_url, sizeof(out.api_base_url))) {
        return CodecError::MissingField;
    }

    return CodecError::Ok;
}

ProvisionOutcome fromHttpStatus(int status) {
    if (status == 200) return ProvisionOutcome::Granted;
    if (status == 400) return ProvisionOutcome::BadRequest;
    if (status == 403) return ProvisionOutcome::NotReleased;
    if (status == 404) return ProvisionOutcome::UnknownBoard;
    if (status == 409) return ProvisionOutcome::Concurrent;
    if (status == 429) return ProvisionOutcome::RateLimited;
    if (status >= 500) return ProvisionOutcome::ServerError;
    return ProvisionOutcome::TransportError;
}

bool isTransient(ProvisionOutcome o) {
    switch (o) {
        case ProvisionOutcome::Concurrent:
        case ProvisionOutcome::RateLimited:
        case ProvisionOutcome::ServerError:
        case ProvisionOutcome::TransportError:
            return true;
        default:
            // 400/403/404 need a human to act: fix the firmware or unlock the
            // board in the panel. Retrying faster will not change the answer.
            return false;
    }
}

const char* toString(ProvisionOutcome o) {
    switch (o) {
        case ProvisionOutcome::Granted:        return "granted";
        case ProvisionOutcome::BadRequest:     return "bad_request";
        case ProvisionOutcome::NotReleased:    return "not_released";
        case ProvisionOutcome::UnknownBoard:   return "unknown_board";
        case ProvisionOutcome::Concurrent:     return "concurrent";
        case ProvisionOutcome::RateLimited:    return "rate_limited";
        case ProvisionOutcome::ServerError:    return "server_error";
        case ProvisionOutcome::TransportError: return "transport_error";
    }
    return "unknown";
}

const char* toString(CodecError e) {
    switch (e) {
        case CodecError::Ok:             return "ok";
        case CodecError::BufferTooSmall: return "buffer_too_small";
        case CodecError::MalformedJson:  return "malformed_json";
        case CodecError::MissingField:   return "missing_field";
        case CodecError::FieldTooLong:   return "field_too_long";
        case CodecError::BadValue:       return "bad_value";
    }
    return "unknown";
}

}  // namespace provisioning
}  // namespace campodata
