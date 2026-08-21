#include "campodata/Types.h"

namespace campodata {

// Spellings are wire format, not debug output: the spec's flow diagrams show
// these exact strings in ota_state and reason. Changing one changes the API.
const char* toString(OtaState s) {
    switch (s) {
        case OtaState::Idle:          return "idle";
        case OtaState::Aborted:       return "aborted";
        case OtaState::PendingUser:   return "pending_user";
        case OtaState::Downloading:   return "downloading";
        case OtaState::Verifying:     return "verifying";
        case OtaState::Applying:      return "applying";
        case OtaState::PendingReboot: return "pending_reboot";
        case OtaState::Confirming:    return "confirming";
        case OtaState::Failed:        return "failed";
    }
    return "idle";
}

const char* toString(AbortReason r) {
    switch (r) {
        case AbortReason::None:           return "";
        case AbortReason::Gprs:           return "gprs";
        case AbortReason::NoWifi:         return "no_wifi";
        case AbortReason::Checksum:       return "checksum";
        case AbortReason::Network:        return "network";
        case AbortReason::Stall:          return "stall";
        case AbortReason::Flash:          return "flash";
        case AbortReason::NoSpace:        return "no_space";
        case AbortReason::NoOtaPartition: return "no_ota_partition";
        case AbortReason::SizeMismatch:   return "size_mismatch";
        case AbortReason::ServerReject:   return "server_reject";
        case AbortReason::UserWindow:     return "user_window";
        case AbortReason::Downgrade:      return "downgrade";
        case AbortReason::AppVeto:        return "app_veto";
    }
    return "";
}

const char* toString(ConfirmStatus s) {
    switch (s) {
        case ConfirmStatus::Success:    return "success";
        case ConfirmStatus::Failed:     return "failed";
        case ConfirmStatus::RolledBack: return "rolled_back";
    }
    return "failed";
}

const char* toString(LinkType t) {
    switch (t) {
        case LinkType::Wifi: return "wifi";
        case LinkType::Gprs: return "gprs";
        case LinkType::None: return "none";
    }
    return "none";
}

const char* toString(DeviceState s) {
    switch (s) {
        case DeviceState::Boot:            return "boot";
        case DeviceState::Provisioning:    return "provisioning";
        case DeviceState::Operation:       return "operation";
        case DeviceState::EvaluatingOrder: return "evaluating_order";
        case DeviceState::AwaitingButton:  return "awaiting_button";
        case DeviceState::Downloading:     return "downloading";
        case DeviceState::Verifying:       return "verifying";
        case DeviceState::Applying:        return "applying";
        case DeviceState::PendingReboot:   return "pending_reboot";
        case DeviceState::Confirming:      return "confirming";
        case DeviceState::RollingBack:     return "rolling_back";
        case DeviceState::Failed:          return "failed";
    }
    return "boot";
}

const char* toString(ConfigError e) {
    switch (e) {
        case ConfigError::Ok:                         return "ok";
        case ConfigError::MissingFirmwareVersion:     return "missing_firmware_version";
        case ConfigError::MissingRepo:                return "missing_repo";
        case ConfigError::GprsPinsUnset:              return "gprs_pins_unset";
        case ConfigError::SlugRequiredForScheme:      return "slug_required_for_scheme";
        case ConfigError::TlsWithoutCaAndNotInsecure: return "tls_without_ca";
        case ConfigError::OtaOnGprsWithoutOptIn:      return "ota_on_gprs_without_opt_in";
        case ConfigError::BothModeMissingWifiCreds:   return "both_mode_missing_wifi_creds";
    }
    return "unknown";
}

const char* toString(OtaSinkError e) {
    switch (e) {
        case OtaSinkError::Ok:               return "ok";
        case OtaSinkError::NoPartition:      return "no_partition";
        case OtaSinkError::TooLarge:         return "too_large";
        case OtaSinkError::BeginFailed:      return "begin_failed";
        case OtaSinkError::WriteFailed:      return "write_failed";
        case OtaSinkError::ChecksumMismatch: return "checksum_mismatch";
        case OtaSinkError::EndFailed:        return "end_failed";
        case OtaSinkError::SetBootFailed:    return "set_boot_failed";
        case OtaSinkError::NotOpen:          return "not_open";
        case OtaSinkError::AlreadyOpen:      return "already_open";
    }
    return "unknown";
}

}  // namespace campodata
