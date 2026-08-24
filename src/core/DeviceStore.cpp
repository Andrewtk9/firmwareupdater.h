#include "core/DeviceStore.h"

#include <string.h>

namespace campodata {
namespace {

bool copyTo(char* dst, size_t cap, const char* src) {
    if (src == nullptr) return false;
    const size_t len = strlen(src);
    if (len >= cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

}  // namespace

// ---------------------------------------------------------------- identity --

bool DeviceStore::isProvisioned() const {
    uint8_t flag = 0;
    if (!_nvs.getU8(nvskey::kProvisioned, flag)) return false;

    // The flag alone is not enough: a device id is what the topics and the REST
    // calls are keyed on, so treat a flag without an id as not provisioned.
    char id[kMaxIdLen] = {};
    return flag != 0 && _nvs.getString(nvskey::kDeviceId, id, sizeof(id));
}

bool DeviceStore::deviceId(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kDeviceId, out, cap);
}

bool DeviceStore::saveProvisioning(const Provisioning& p) {
    if (p.device_id[0] == '\0' || p.mqtt_host[0] == '\0' ||
        p.mqtt_user[0] == '\0' || p.mqtt_pass[0] == '\0') {
        return false;
    }

    bool ok = true;
    ok = _nvs.setString(nvskey::kDeviceId, p.device_id) && ok;
    ok = _nvs.setString(nvskey::kMqttHost, p.mqtt_host) && ok;
    ok = _nvs.setU16(nvskey::kMqttPort, p.mqtt_port) && ok;
    ok = _nvs.setString(nvskey::kMqttUser, p.mqtt_user) && ok;
    ok = _nvs.setString(nvskey::kMqttPass, p.mqtt_pass) && ok;

    if (p.api_base_url[0] != '\0') {
        ok = _nvs.setString(nvskey::kApiBaseUrl, p.api_base_url) && ok;
    }
    if (p.topic_ping[0] != '\0') {
        ok = _nvs.setString(nvskey::kTopicPing, p.topic_ping) && ok;
    }
    if (p.topic_update[0] != '\0') {
        ok = _nvs.setString(nvskey::kTopicUpdate, p.topic_update) && ok;
    }
    if (p.topic_config[0] != '\0') {
        ok = _nvs.setString(nvskey::kTopicConfig, p.topic_config) && ok;
    }

    if (!ok) {
        // Never leave a half-written credential set behind: a device that
        // believes it is provisioned but cannot authenticate is worse than one
        // that simply tries again.
        _nvs.setU8(nvskey::kProvisioned, 0);
        _nvs.commit();
        return false;
    }

    ok = _nvs.setU8(nvskey::kProvisioned, 1) && ok;

    // The endpoint is no longer needed, so stop carrying it.
    _nvs.erase(nvskey::kBootstrapUrl);

    return _nvs.commit() && ok;
}

// --------------------------------------------------------------- bootstrap --

bool DeviceStore::bootstrapUrl(char* out, size_t cap, const char* fallback) const {
    if (out == nullptr || cap == 0) return false;

    if (_nvs.getString(nvskey::kBootstrapUrl, out, cap)) return true;

    out[0] = '\0';
    return copyTo(out, cap, fallback);
}

bool DeviceStore::seedBootstrapUrl(const char* url) {
    if (url == nullptr || url[0] == '\0') return false;
    return _nvs.setString(nvskey::kBootstrapUrl, url) && _nvs.commit();
}

bool DeviceStore::hasBootstrapUrl() const {
    char scratch[kMaxUrlLen] = {};
    return _nvs.getString(nvskey::kBootstrapUrl, scratch, sizeof(scratch));
}

bool DeviceStore::forgetBootstrapUrl() {
    return _nvs.erase(nvskey::kBootstrapUrl) && _nvs.commit();
}

// ------------------------------------------------------------------ broker --

bool DeviceStore::mqttHost(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kMqttHost, out, cap);
}

bool DeviceStore::mqttPort(uint16_t& out) const {
    return _nvs.getU16(nvskey::kMqttPort, out);
}

bool DeviceStore::mqttUser(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kMqttUser, out, cap);
}

bool DeviceStore::mqttPass(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kMqttPass, out, cap);
}

bool DeviceStore::apiBaseUrl(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kApiBaseUrl, out, cap);
}

bool DeviceStore::topics(char* ping, size_t ping_cap,
                         char* update, size_t update_cap,
                         char* config, size_t config_cap) const {
    const bool a = _nvs.getString(nvskey::kTopicPing, ping, ping_cap);
    const bool b = _nvs.getString(nvskey::kTopicUpdate, update, update_cap);
    const bool c = _nvs.getString(nvskey::kTopicConfig, config, config_cap);
    return a && b && c;
}

// ----------------------------------------------------------------- version --

bool DeviceStore::firmwareVersion(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kFwVersion, out, cap);
}

bool DeviceStore::firmwareRepo(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kFwRepo, out, cap);
}

bool DeviceStore::commitFirmware(const char* version, const char* repo) {
    if (version == nullptr || version[0] == '\0') return false;

    bool ok = _nvs.setString(nvskey::kFwVersion, version);
    if (repo != nullptr && repo[0] != '\0') {
        ok = _nvs.setString(nvskey::kFwRepo, repo) && ok;
    }
    return _nvs.commit() && ok;
}

uint32_t DeviceStore::configVersion() const {
    uint32_t v = 0;
    return _nvs.getU32(nvskey::kConfigVersion, v) ? v : 0;
}

bool DeviceStore::applyConfigVersion(uint32_t version) {
    if (version <= configVersion()) return false;
    return _nvs.setU32(nvskey::kConfigVersion, version) && _nvs.commit();
}

// ---------------------------------------------------------- pending update --

bool DeviceStore::hasPending() const {
    char id[kMaxIdLen] = {};
    return _nvs.getString(nvskey::kPendUpdateId, id, sizeof(id));
}

bool DeviceStore::loadPending(Pending& out) const {
    out = Pending{};
    if (!_nvs.getString(nvskey::kPendUpdateId, out.update_id, sizeof(out.update_id))) {
        return false;
    }

    _nvs.getString(nvskey::kPendVersion, out.version, sizeof(out.version));
    _nvs.getString(nvskey::kPendRepo, out.repo, sizeof(out.repo));
    _nvs.getString(nvskey::kPendPrevVer, out.previous, sizeof(out.previous));
    _nvs.getU32(nvskey::kPendStartedAt, out.started_at_s);
    _nvs.getU32(nvskey::kPendDeadline, out.deadline_s);
    return true;
}

bool DeviceStore::savePending(const Pending& p) {
    if (p.update_id[0] == '\0') return false;

    bool ok = _nvs.setString(nvskey::kPendUpdateId, p.update_id);
    ok = _nvs.setString(nvskey::kPendVersion, p.version) && ok;
    ok = _nvs.setString(nvskey::kPendRepo, p.repo) && ok;
    ok = _nvs.setString(nvskey::kPendPrevVer, p.previous) && ok;
    ok = _nvs.setU32(nvskey::kPendStartedAt, p.started_at_s) && ok;
    ok = _nvs.setU32(nvskey::kPendDeadline, p.deadline_s) && ok;
    return _nvs.commit() && ok;
}

bool DeviceStore::clearPending() {
    _nvs.erase(nvskey::kPendUpdateId);
    _nvs.erase(nvskey::kPendVersion);
    _nvs.erase(nvskey::kPendRepo);
    _nvs.erase(nvskey::kPendPrevVer);
    _nvs.erase(nvskey::kPendStartedAt);
    _nvs.erase(nvskey::kPendDeadline);
    return _nvs.commit();
}

bool DeviceStore::lastUpdateId(char* out, size_t cap) const {
    return _nvs.getString(nvskey::kLastUpdateId, out, cap);
}

bool DeviceStore::setLastUpdateId(const char* update_id) {
    if (update_id == nullptr || update_id[0] == '\0') return false;
    return _nvs.setString(nvskey::kLastUpdateId, update_id) && _nvs.commit();
}

// ------------------------------------------------------------------- state --

OtaState DeviceStore::otaState() const {
    uint8_t v = 0;
    if (!_nvs.getU8(nvskey::kOtaState, v)) return OtaState::Idle;
    return static_cast<OtaState>(v);
}

AbortReason DeviceStore::abortReason() const {
    uint8_t v = 0;
    if (!_nvs.getU8(nvskey::kAbortReason, v)) return AbortReason::None;
    return static_cast<AbortReason>(v);
}

bool DeviceStore::setOtaState(OtaState state, AbortReason reason) {
    bool ok = _nvs.setU8(nvskey::kOtaState, static_cast<uint8_t>(state));
    ok = _nvs.setU8(nvskey::kAbortReason, static_cast<uint8_t>(reason)) && ok;
    return _nvs.commit() && ok;
}

ClockSource DeviceStore::clockBasis() const {
    uint8_t v = 0;
    if (!_nvs.getU8(nvskey::kClockBasis, v)) return ClockSource::None;
    return static_cast<ClockSource>(v);
}

bool DeviceStore::setClockBasis(ClockSource source) {
    return _nvs.setU8(nvskey::kClockBasis, static_cast<uint8_t>(source)) && _nvs.commit();
}

bool DeviceStore::rollbackGuardBypassed() const {
    uint8_t v = 0;
    return _nvs.getU8(nvskey::kGuardBypassed, v) && v != 0;
}

bool DeviceStore::setRollbackGuardBypassed(bool bypassed) {
    return _nvs.setU8(nvskey::kGuardBypassed, bypassed ? 1 : 0) && _nvs.commit();
}

bool DeviceStore::factoryReset() {
    return _nvs.eraseAll() && _nvs.commit();
}

}  // namespace campodata
