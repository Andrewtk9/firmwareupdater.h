#include "platform/esp32/Esp32Nvs.h"

#if FWUP_TARGET_ESP32

#include <esp_partition.h>
#include <nvs_flash.h>
#include <string.h>

namespace campodata {
namespace {

// Encryption is real only when a nvs_keys partition exists, because that is
// where the key lives - itself protected by flash encryption and an eFuse.
// Without it, values sit in plain text however the build was configured.
bool encryptionAvailable() {
    const esp_partition_t* keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, nullptr);
    return keys != nullptr;
}

}  // namespace

Esp32Nvs::~Esp32Nvs() {
    end();
}

bool Esp32Nvs::begin(const char* ns) {
    if (_open) return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Documented recovery: the partition is full or was written by a newer
        // NVS version. Erasing loses stored values, so the device falls back to
        // re-provisioning rather than refusing to boot.
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return false;

    if (nvs_open(ns, NVS_READWRITE, &_handle) != ESP_OK) return false;

    _open      = true;
    _encrypted = encryptionAvailable();
    return true;
}

void Esp32Nvs::end() {
    if (_open) {
        nvs_close(_handle);
        _handle = 0;
        _open   = false;
    }
}

bool Esp32Nvs::getString(const char* key, char* out, size_t cap) const {
    if (!_open || key == nullptr || out == nullptr || cap == 0) return false;

    size_t len = cap;
    if (nvs_get_str(_handle, key, out, &len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

bool Esp32Nvs::setString(const char* key, const char* value) {
    if (!_open || key == nullptr || value == nullptr) return false;
    return nvs_set_str(_handle, key, value) == ESP_OK;
}

bool Esp32Nvs::getU32(const char* key, uint32_t& out) const {
    if (!_open || key == nullptr) return false;
    return nvs_get_u32(_handle, key, &out) == ESP_OK;
}

bool Esp32Nvs::setU32(const char* key, uint32_t value) {
    if (!_open || key == nullptr) return false;
    return nvs_set_u32(_handle, key, value) == ESP_OK;
}

bool Esp32Nvs::getU16(const char* key, uint16_t& out) const {
    if (!_open || key == nullptr) return false;
    return nvs_get_u16(_handle, key, &out) == ESP_OK;
}

bool Esp32Nvs::setU16(const char* key, uint16_t value) {
    if (!_open || key == nullptr) return false;
    return nvs_set_u16(_handle, key, value) == ESP_OK;
}

bool Esp32Nvs::getU8(const char* key, uint8_t& out) const {
    if (!_open || key == nullptr) return false;
    return nvs_get_u8(_handle, key, &out) == ESP_OK;
}

bool Esp32Nvs::setU8(const char* key, uint8_t value) {
    if (!_open || key == nullptr) return false;
    return nvs_set_u8(_handle, key, value) == ESP_OK;
}

bool Esp32Nvs::erase(const char* key) {
    if (!_open || key == nullptr) return false;

    const esp_err_t err = nvs_erase_key(_handle, key);
    // Already gone is the desired end state, so treat it as success.
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

bool Esp32Nvs::eraseAll() {
    if (!_open) return false;
    return nvs_erase_all(_handle) == ESP_OK;
}

bool Esp32Nvs::commit() {
    if (!_open) return false;
    return nvs_commit(_handle) == ESP_OK;
}

bool Esp32Nvs::has(const char* key) const {
    if (!_open || key == nullptr) return false;

    size_t len = 0;
    if (nvs_get_str(_handle, key, nullptr, &len) == ESP_OK) return true;

    uint32_t scratch32 = 0;
    if (nvs_get_u32(_handle, key, &scratch32) == ESP_OK) return true;

    uint16_t scratch16 = 0;
    if (nvs_get_u16(_handle, key, &scratch16) == ESP_OK) return true;

    uint8_t scratch8 = 0;
    return nvs_get_u8(_handle, key, &scratch8) == ESP_OK;
}

}  // namespace campodata

#endif  // FWUP_TARGET_ESP32
