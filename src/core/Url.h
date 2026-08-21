#pragma once

#include "campodata/Types.h"

namespace campodata {
namespace url {

enum class Scheme : uint8_t { Unknown, Http, Https };

Scheme scheme(const char* url);

// Whether a link can actually fetch this URL.
//
// The cellular stack has no TLS, and the deployed server builds every download
// URL from one configured base - so a GPRS device can be handed an https:// URL
// it can never open. Catching that here turns a silent stall into a reported
// abort reason.
bool fetchable(const char* url, bool link_supports_tls);

// Joins a base and a path without doubling or dropping the separator.
bool join(const char* base, const char* path, char* out, size_t cap);

// Formats a path template holding a single %s.
bool formatPath(const char* base, const char* path_template, const char* arg,
                char* out, size_t cap);

}  // namespace url
}  // namespace campodata
