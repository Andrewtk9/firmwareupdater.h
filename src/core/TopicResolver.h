#pragma once

#include "campodata/Config.h"
#include "campodata/Types.h"

namespace campodata {

// Owns the three topic strings for this device.
//
// Precedence: whatever provisioning delivered wins, then an explicit override
// in Config, then the scheme default. The server is the authority - the
// firmware only supplies a bootstrap guess so it can talk before it is
// provisioned.
class TopicResolver {
public:
    // Spec section 5 format. The leading slash is intentional: it is what the
    // spec writes, and it is what the ACL rows have to match.
    static constexpr const char* kPdfPing   = "/ping/";
    static constexpr const char* kPdfUpdate = "/update/";
    static constexpr const char* kPdfConfig = "/config/";

    // `device_id` may be empty before provisioning; topics are then empty too.
    bool build(const TopicConfig& cfg, const char* device_id);

    // Applies server-supplied topics. Empty strings are ignored so a partial
    // response leaves the computed defaults in place.
    void applyProvisioned(const Provisioning& p);

    const char* ping()   const { return _ping; }
    const char* update() const { return _update; }
    const char* config() const { return _config; }

    bool ready() const { return _ping[0] != '\0'; }

    // The device may only publish to its own ping topic. With no per-device ACL
    // on the broker yet, this is the only thing containing a misconfigured
    // device to its own namespace.
    bool mayPublish(const char* topic) const;

private:
    char _ping[kMaxTopicLen]   = {};
    char _update[kMaxTopicLen] = {};
    char _config[kMaxTopicLen] = {};
};

}  // namespace campodata
