#include "core/Url.h"

#include <stdio.h>
#include <string.h>

namespace campodata {
namespace url {
namespace {

bool startsWithNoCase(const char* s, const char* prefix) {
    while (*prefix != '\0') {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

Scheme scheme(const char* u) {
    if (u == nullptr) return Scheme::Unknown;
    if (startsWithNoCase(u, "https://")) return Scheme::Https;
    if (startsWithNoCase(u, "http://")) return Scheme::Http;
    return Scheme::Unknown;
}

bool fetchable(const char* u, bool link_supports_tls) {
    switch (scheme(u)) {
        case Scheme::Http:  return true;
        case Scheme::Https: return link_supports_tls;
        default:            return false;
    }
}

bool join(const char* base, const char* path, char* out, size_t cap) {
    if (base == nullptr || path == nullptr || out == nullptr) return false;

    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') --base_len;

    const char* sep = (path[0] == '/') ? "" : "/";
    const int n = snprintf(out, cap, "%.*s%s%s",
                           static_cast<int>(base_len), base, sep, path);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool formatPath(const char* base, const char* path_template, const char* arg,
                char* out, size_t cap) {
    if (path_template == nullptr || arg == nullptr) return false;

    char path[kMaxUrlLen];
    const int n = snprintf(path, sizeof(path), path_template, arg);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return false;

    return join(base, path, out, cap);
}

}  // namespace url
}  // namespace campodata
