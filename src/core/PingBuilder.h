#pragma once

#include "campodata/Types.h"

namespace campodata {

// Lets a project add its own fields without the library knowing about them.
// Function pointer plus context, not std::function: no heap, deterministic.
using PingExtender = void (*)(void* json_object, void* ctx);

// Builds the /ping payload of spec section 6.
//
// Single-link devices emit exactly the spec's nine fields. The extra link
// fields appear only in Both mode, where the spec's single `link` value cannot
// express "MQTT is on cellular but Wi-Fi is up and OTA could run".
class PingBuilder {
public:
    void setExtender(PingExtender fn, void* ctx) { _extender = fn; _ctx = ctx; }

    // `strict` emits only the spec's section 6 fields, extender included.
    // `dual_link` gates the link detail, which only means anything when both
    // links exist and is skipped in strict mode anyway.
    CodecError build(const PingSnapshot& snap, bool dual_link, bool strict,
                     char* out, size_t cap) const;

    // Last Will payload, spec section 5.
    static const char* willPayload() { return "{\"status\":\"offline\"}"; }

    // The spec's table marks /ping as retain = no, so the will follows it.
    static constexpr bool  kWillRetain = false;
    static constexpr uint8_t kWillQos  = 1;

private:
    PingExtender _extender = nullptr;
    void*        _ctx      = nullptr;
};

}  // namespace campodata
