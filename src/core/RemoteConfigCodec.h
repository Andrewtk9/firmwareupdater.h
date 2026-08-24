#pragma once

#include "campodata/Types.h"

namespace campodata {
namespace remoteconfig {

// Parses the payload of /config/<id>, spec section 8.
//
// Absent fields keep whatever `out` already holds, so a partial document tunes
// only what it mentions instead of resetting the rest to defaults.
CodecError parse(const char* json, RemoteConfig& out);

// Serialises the confirm-side view, used when reporting applied_at.
CodecError buildAck(const RemoteConfig& cfg, const char* device_id,
                    char* out, size_t cap);

}  // namespace remoteconfig
}  // namespace campodata
