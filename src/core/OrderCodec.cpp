#include "core/OrderCodec.h"

#include <ArduinoJson.h>
#include <stdlib.h>
#include <string.h>

#include "core/Hex.h"
#include "core/Iso8601.h"

namespace campodata {
namespace order {
namespace {

bool copyField(const char* src, char* dst, size_t cap) {
    if (src == nullptr) return false;
    const size_t len = strlen(src);
    if (len == 0 || len >= cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

// Reads one dotted component and advances past the separator.
long nextComponent(const char*& p) {
    if (p == nullptr || *p == '\0') return -1;
    char* end = nullptr;
    const long v = strtol(p, &end, 10);
    if (end == p) return -1;  // not numeric
    p = (*end == '.') ? end + 1 : end;
    return v;
}

}  // namespace

CodecError parse(const char* json, UpdateOrder& out) {
    if (json == nullptr) return CodecError::MalformedJson;

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        return CodecError::MalformedJson;
    }

    out = UpdateOrder{};

    if (!copyField(doc["update_id"] | static_cast<const char*>(nullptr),
                   out.update_id, sizeof(out.update_id))) {
        return CodecError::MissingField;
    }
    if (!copyField(doc["target_version"] | static_cast<const char*>(nullptr),
                   out.target_version, sizeof(out.target_version))) {
        return CodecError::MissingField;
    }
    if (!copyField(doc["url"] | static_cast<const char*>(nullptr),
                   out.url, sizeof(out.url))) {
        return CodecError::MissingField;
    }

    // repo is what lets the device refuse another project's firmware.
    copyField(doc["repo"] | static_cast<const char*>(nullptr),
              out.repo, sizeof(out.repo));

    const long long size = doc["size_bytes"] | 0LL;
    if (size <= 0 || size > 0x7FFFFFFFLL) return CodecError::BadValue;
    out.size_bytes = static_cast<uint32_t>(size);

    const char* sha = doc["sha256"] | static_cast<const char*>(nullptr);
    if (sha != nullptr && sha[0] != '\0') {
        if (!hex::decode(sha, out.sha256, kSha256Bytes)) return CodecError::BadValue;
        out.has_sha256 = true;
    }

    out.mandatory       = doc["mandatory"] | false;
    out.requires_button = doc["requires_button"] | true;

    const char* issued = doc["issued_at"] | static_cast<const char*>(nullptr);
    if (issued != nullptr) iso8601::parse(issued, out.issued_at);

    return CodecError::Ok;
}

int compareVersions(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return 0;

    while (*a != '\0' || *b != '\0') {
        const long va = nextComponent(a);
        const long vb = nextComponent(b);
        if (va < 0 && vb < 0) return 0;      // both ran out of numeric parts
        const long la = (va < 0) ? 0 : va;
        const long lb = (vb < 0) ? 0 : vb;
        if (la != lb) return (la < lb) ? -1 : 1;
        if (va < 0 && vb < 0) break;
    }
    return 0;
}

OrderRejection validate(const UpdateOrder& o,
                        const char* current_version,
                        const char* current_repo,
                        uint32_t slot_bytes,
                        const char* last_update_id,
                        bool allow_downgrade) {
    if (o.update_id[0] == '\0' || o.url[0] == '\0' ||
        o.target_version[0] == '\0') {
        return OrderRejection::MissingField;
    }

    // /update is a retained topic, so the broker replays this order on every
    // reconnect. Without dedupe a link flap re-runs the same update forever.
    if (last_update_id != nullptr && last_update_id[0] != '\0' &&
        strcmp(o.update_id, last_update_id) == 0) {
        return OrderRejection::Duplicate;
    }

    // Not in the spec, but a misrouted order would otherwise flash another
    // project's firmware onto this device.
    if (o.repo[0] != '\0' && current_repo != nullptr && current_repo[0] != '\0' &&
        strcmp(o.repo, current_repo) != 0) {
        return OrderRejection::WrongRepo;
    }

    if (current_version != nullptr && current_version[0] != '\0') {
        const int cmp = compareVersions(o.target_version, current_version);
        if (cmp == 0) return OrderRejection::SameVersion;
        // Anti-rollback eFuse is unavailable, so a valid older image would
        // replay happily. Refusing downgrades here is the only guard we have.
        if (cmp < 0 && !allow_downgrade) return OrderRejection::Downgrade;
    }

    if (slot_bytes > 0 && o.size_bytes > slot_bytes) return OrderRejection::TooLarge;

    if (!o.has_sha256) return OrderRejection::NoSha256;

    return OrderRejection::None;
}

const char* toString(OrderRejection r) {
    switch (r) {
        case OrderRejection::None:         return "none";
        case OrderRejection::Malformed:    return "malformed";
        case OrderRejection::MissingField: return "missing_field";
        case OrderRejection::WrongRepo:    return "wrong_repo";
        case OrderRejection::SameVersion:  return "same_version";
        case OrderRejection::Downgrade:    return "downgrade";
        case OrderRejection::TooLarge:     return "too_large";
        case OrderRejection::NoSha256:     return "no_sha256";
        case OrderRejection::Duplicate:    return "duplicate";
    }
    return "unknown";
}

}  // namespace order
}  // namespace campodata
