#include "platform/esp32/Esp32OtaSink.h"

#include "core/Log.h"

#if FWUP_TARGET_ESP32

#include <esp_app_format.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <string.h>

#include "core/Hex.h"

namespace campodata {

Esp32OtaSink::~Esp32OtaSink() {
    abort();
}

bool Esp32OtaSink::probe(OtaSlotInfo& out) const {
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next == nullptr) {
        out = OtaSlotInfo{};
        return false;
    }

    out.size_bytes = next->size;
    out.offset     = next->address;
    out.subtype    = next->subtype;
    out.valid      = true;
    return true;
}

OtaSinkError Esp32OtaSink::begin(uint32_t expected_size) {
    if (_open) return OtaSinkError::AlreadyOpen;

    _target = esp_ota_get_next_update_partition(nullptr);
    if (_target == nullptr) return OtaSinkError::NoPartition;

    // Reject before spending a single byte of the user's data plan. The fleet's
    // hardcoded 2000000 cap is larger than the real 0x1E0000 slot, so oversized
    // images used to download fully and only then fail to flash.
    if (expected_size > 0 && expected_size > _target->size) return OtaSinkError::TooLarge;

    // OTA_SIZE_UNKNOWN selects sequential-write mode, where erase happens one
    // sector at a time inside esp_ota_write. Passing the exact size instead
    // makes esp_ota_begin erase the whole slot up front - roughly 375 sectors
    // at ~40 ms each - which blows straight past the 5 s task watchdog.
    const esp_err_t err = esp_ota_begin(_target, OTA_SIZE_UNKNOWN, &_handle);
    if (err != ESP_OK) {
        FWUP_LOGE("ota", "esp_ota_begin falhou: %s", esp_err_to_name(err));
        return OtaSinkError::BeginFailed;
    }

    if (!_sha.begin()) {
        esp_ota_abort(_handle);
        _handle = 0;
        return OtaSinkError::BeginFailed;
    }

    _written  = 0;
    _expected = expected_size;
    _open     = true;
    return OtaSinkError::Ok;
}

OtaSinkError Esp32OtaSink::write(const uint8_t* data, size_t len) {
    if (!_open) {
        FWUP_LOGE("ota", "escrita com a particao fechada, em %u bytes", (unsigned)_written);
        return OtaSinkError::NotOpen;
    }
    if (len == 0) return OtaSinkError::Ok;
    if (data == nullptr) return OtaSinkError::WriteFailed;

    if (_target != nullptr && _written + len > _target->size) {
        FWUP_LOGE("ota", "bloco excede a particao: %u + %u > %u",
                  (unsigned)_written, (unsigned)len, (unsigned)_target->size);
        return OtaSinkError::TooLarge;
    }

    // O erro do IDF entra no log: sem ele, "abortado: flash" nao distingue
    // particao errada de timeout do controlador, e a investigacao vira palpite.
    const esp_err_t err = esp_ota_write(_handle, data, len);
    if (err != ESP_OK) {
        FWUP_LOGE("ota", "esp_ota_write falhou em %u bytes, bloco de %u: %s",
                  (unsigned)_written, (unsigned)len, esp_err_to_name(err));
        return OtaSinkError::WriteFailed;
    }
    // O digest e alimentado em streaming pelo mbedtls, que no ESP32 usa o
    // acelerador de hardware - o mesmo periferico que o TLS da sessao MQTT e o
    // do proprio download usam. Uma falha aqui aparecia como se fosse da flash.
    if (!_sha.update(data, len)) {
        FWUP_LOGE("ota", "sha256 falhou em %u bytes, bloco de %u",
                  (unsigned)_written, (unsigned)len);
        return OtaSinkError::WriteFailed;
    }

    _written += static_cast<uint32_t>(len);
    return OtaSinkError::Ok;
}

OtaSinkError Esp32OtaSink::finish(const uint8_t expected_sha256[kSha256Bytes]) {
    if (!_open) return OtaSinkError::NotOpen;

    if (_expected > 0 && _written != _expected) {
        abort();
        return OtaSinkError::ChecksumMismatch;
    }

    uint8_t actual[kSha256Bytes] = {};
    if (!_sha.finish(actual)) {
        abort();
        return OtaSinkError::EndFailed;
    }

    // Verify before the image can ever become bootable.
    if (expected_sha256 != nullptr &&
        !hex::equal(actual, expected_sha256, kSha256Bytes)) {
        abort();
        return OtaSinkError::ChecksumMismatch;
    }

    if (esp_ota_end(_handle) != ESP_OK) {
        _handle = 0;
        _open   = false;
        return OtaSinkError::EndFailed;
    }
    _handle = 0;
    _open   = false;

    if (esp_ota_set_boot_partition(_target) != ESP_OK) return OtaSinkError::SetBootFailed;

    return OtaSinkError::Ok;
}

void Esp32OtaSink::abort() {
    if (_open) {
        esp_ota_abort(_handle);
        _handle = 0;
        _open   = false;
    }
    _sha.reset();
    _written  = 0;
    _expected = 0;
}

bool Esp32OtaSink::pendingVerify() const {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return false;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool Esp32OtaSink::markValid() {
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool Esp32OtaSink::markInvalidAndReboot() {
    // Does not return when it succeeds.
    return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK;
}

bool Esp32OtaSink::runningVersion(char* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return false;

    esp_app_desc_t desc{};
    if (esp_ota_get_partition_description(running, &desc) != ESP_OK) return false;

    snprintf(out, cap, "%s", desc.version);
    return true;
}

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
