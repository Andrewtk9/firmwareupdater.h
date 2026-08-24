#include "core/TopicResolver.h"

#include <stdio.h>
#include <string.h>

namespace campodata {
namespace {

bool setTopic(char* dst, size_t cap, const char* value) {
    if (value == nullptr || value[0] == '\0') return false;
    const size_t len = strlen(value);
    if (len >= cap) return false;
    memcpy(dst, value, len + 1);
    return true;
}

bool formatTopic(char* dst, size_t cap, const char* prefix, const char* id) {
    const int n = snprintf(dst, cap, "%s%s", prefix, id);
    return n > 0 && static_cast<size_t>(n) < cap;
}

}  // namespace

bool TopicResolver::build(const TopicConfig& cfg, const char* device_id) {
    _ping[0] = _update[0] = _config[0] = '\0';

    // Explicit overrides short-circuit everything.
    const bool has_overrides =
        setTopic(_ping, sizeof(_ping), cfg.t_ping) &&
        setTopic(_update, sizeof(_update), cfg.t_update) &&
        setTopic(_config, sizeof(_config), cfg.t_config);
    if (has_overrides) return true;

    if (device_id == nullptr || device_id[0] == '\0') return false;

    if (cfg.scheme == MqttTopicScheme::SlugEnvRet) {
        if (cfg.project_slug == nullptr || cfg.project_slug[0] == '\0') return false;
        char prefix[64];
        if (snprintf(prefix, sizeof(prefix), "%s/ret/", cfg.project_slug) < 0) return false;
        if (!formatTopic(_ping, sizeof(_ping), prefix, device_id)) return false;

        if (snprintf(prefix, sizeof(prefix), "%s/env/", cfg.project_slug) < 0) return false;
        char base[kMaxTopicLen];
        if (!formatTopic(base, sizeof(base), prefix, device_id)) return false;
        if (snprintf(_update, sizeof(_update), "%s/update", base) < 0) return false;
        if (snprintf(_config, sizeof(_config), "%s/config", base) < 0) return false;
        return true;
    }

    return formatTopic(_ping, sizeof(_ping), kPdfPing, device_id) &&
           formatTopic(_update, sizeof(_update), kPdfUpdate, device_id) &&
           formatTopic(_config, sizeof(_config), kPdfConfig, device_id);
}

void TopicResolver::applyProvisioned(const Provisioning& p) {
    setTopic(_ping, sizeof(_ping), p.topic_ping);
    setTopic(_update, sizeof(_update), p.topic_update);
    setTopic(_config, sizeof(_config), p.topic_config);
}

bool TopicResolver::mayPublish(const char* topic) const {
    if (topic == nullptr || _ping[0] == '\0') return false;
    return strcmp(topic, _ping) == 0;
}

}  // namespace campodata
