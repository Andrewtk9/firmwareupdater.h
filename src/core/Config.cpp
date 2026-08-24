#include "campodata/Config.h"

namespace campodata {

Config Config::defaults() {
    return Config{};
}

ConfigError Config::validate() const {
    if (firmware_version == nullptr || firmware_version[0] == '\0') {
        return ConfigError::MissingFirmwareVersion;
    }
    // The order carries a repo and the device refuses one that is not its own,
    // so without this it cannot tell its own firmware from another project's.
    if (repo == nullptr || repo[0] == '\0') {
        return ConfigError::MissingRepo;
    }

    if (topics.scheme == MqttTopicScheme::SlugEnvRet &&
        (topics.project_slug == nullptr || topics.project_slug[0] == '\0')) {
        return ConfigError::SlugRequiredForScheme;
    }

    const bool needs_gprs = (link_mode == LinkMode::Gprs || link_mode == LinkMode::Both);
    if (needs_gprs && (gprs.apn == nullptr || gprs.pin_tx < 0 || gprs.pin_rx < 0)) {
        return ConfigError::GprsPinsUnset;
    }

    // Downloading over cellular costs the customer data and, on this platform,
    // runs without TLS. Two independent opt-ins so it can never happen by
    // accident.
    if (ota.link_policy == OtaLinkPolicy::AnyLink && !ota.allow_ota_on_gprs) {
        return ConfigError::OtaOnGprsWithoutOptIn;
    }

    return ConfigError::Ok;
}

}  // namespace campodata
