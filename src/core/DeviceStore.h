#pragma once

#include "campodata/Types.h"
#include "core/interfaces/INvs.h"

namespace campodata {

// Everything the device remembers across reboots, over an INvs.
//
// The bootstrap URL has a deliberate lifecycle: seeded in production, read once
// to provision, then erased. A provisioned unit in the field no longer knows
// where the provisioning endpoint is, so it cannot disclose it - which a URL
// compiled into the image could never offer.
class DeviceStore {
public:
    explicit DeviceStore(INvs& nvs) : _nvs(nvs) {}

    // ------------------------------------------------------------ identity --

    bool isProvisioned() const;
    bool deviceId(char* out, size_t cap) const;

    // Writes credentials, topics and api_base_url, marks the device
    // provisioned, and erases the bootstrap URL. All or nothing: a partial
    // write would leave a device that believes it is provisioned but cannot
    // authenticate.
    bool saveProvisioning(const Provisioning& p);

    // ----------------------------------------------------------- bootstrap --

    // Where to POST the first provisioning request. `fallback` is the
    // compiled-in default and is used only when nothing was seeded.
    bool bootstrapUrl(char* out, size_t cap, const char* fallback) const;

    // Production seeding. Call once per unit before it ships.
    bool seedBootstrapUrl(const char* url);

    bool hasBootstrapUrl() const;
    bool forgetBootstrapUrl();

    // --------------------------------------------------------------- broker --

    bool mqttHost(char* out, size_t cap) const;
    bool mqttPort(uint16_t& out) const;
    bool mqttUser(char* out, size_t cap) const;
    bool mqttPass(char* out, size_t cap) const;
    bool apiBaseUrl(char* out, size_t cap) const;

    bool topics(char* ping, size_t ping_cap,
                char* update, size_t update_cap,
                char* config, size_t config_cap) const;

    // -------------------------------------------------------------- version --

    bool firmwareVersion(char* out, size_t cap) const;
    bool firmwareRepo(char* out, size_t cap) const;

    // Only after the confirm returns 200, per spec section 9.2.
    bool commitFirmware(const char* version, const char* repo);

    uint32_t configVersion() const;
    // Applies only when strictly newer: the config topic is retained and gets
    // replayed on every reconnect.
    bool     applyConfigVersion(uint32_t version);

    // ------------------------------------------------------ pending update --

    struct Pending {
        char     update_id[kMaxIdLen]        = {};
        char     version[kMaxVersionLen]     = {};
        char     repo[kMaxRepoLen]           = {};
        char     previous[kMaxVersionLen]    = {};
        uint32_t started_at_s                = 0;
        uint32_t deadline_s                  = 0;
    };

    bool hasPending() const;
    bool loadPending(Pending& out) const;
    bool savePending(const Pending& p);
    bool clearPending();

    // Dedupe key for the retained update topic.
    bool lastUpdateId(char* out, size_t cap) const;
    bool setLastUpdateId(const char* update_id);

    // --------------------------------------------------------------- state --

    OtaState    otaState() const;
    AbortReason abortReason() const;
    bool        setOtaState(OtaState state, AbortReason reason);

    ClockSource clockBasis() const;
    bool        setClockBasis(ClockSource source);

    bool rollbackGuardBypassed() const;
    bool setRollbackGuardBypassed(bool bypassed);

    // Wipes everything so the unit re-provisions from scratch.
    bool factoryReset();

    bool encrypted() const { return _nvs.encrypted(); }

private:
    INvs& _nvs;
};

}  // namespace campodata
