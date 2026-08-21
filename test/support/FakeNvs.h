#pragma once

// In-memory INvs for host tests. Only compiled into the native environment.

#include <map>
#include <string>

#include "core/interfaces/INvs.h"

namespace campodata {

class FakeNvs final : public INvs {
public:
    bool getString(const char* key, char* out, size_t cap) const override {
        if (!out || cap == 0) return false;
        out[0] = '\0';
        auto it = _strings.find(key);
        if (it == _strings.end() || it->second.size() >= cap) return false;
        memcpy(out, it->second.c_str(), it->second.size() + 1);
        return !it->second.empty();
    }

    bool setString(const char* key, const char* value) override {
        if (fail_writes) return false;
        _strings[key] = value ? value : "";
        return true;
    }

    bool getU32(const char* key, uint32_t& out) const override {
        auto it = _numbers.find(key);
        if (it == _numbers.end()) return false;
        out = static_cast<uint32_t>(it->second);
        return true;
    }
    bool setU32(const char* key, uint32_t value) override {
        if (fail_writes) return false;
        _numbers[key] = value;
        return true;
    }

    bool getU16(const char* key, uint16_t& out) const override {
        auto it = _numbers.find(key);
        if (it == _numbers.end()) return false;
        out = static_cast<uint16_t>(it->second);
        return true;
    }
    bool setU16(const char* key, uint16_t value) override {
        if (fail_writes) return false;
        _numbers[key] = value;
        return true;
    }

    bool getU8(const char* key, uint8_t& out) const override {
        auto it = _numbers.find(key);
        if (it == _numbers.end()) return false;
        out = static_cast<uint8_t>(it->second);
        return true;
    }
    bool setU8(const char* key, uint8_t value) override {
        if (fail_writes) return false;
        _numbers[key] = value;
        return true;
    }

    bool erase(const char* key) override {
        _strings.erase(key);
        _numbers.erase(key);
        return true;
    }

    bool eraseAll() override {
        _strings.clear();
        _numbers.clear();
        return true;
    }

    bool commit() override {
        ++commits;
        return true;
    }

    bool has(const char* key) const override {
        return _strings.count(key) > 0 || _numbers.count(key) > 0;
    }

    bool encrypted() const override { return encryption_on; }

    // Test knobs.
    bool fail_writes   = false;
    bool encryption_on = false;
    int  commits       = 0;

    size_t stringCount() const { return _strings.size(); }

private:
    std::map<std::string, std::string> _strings;
    std::map<std::string, uint32_t>    _numbers;
};

}  // namespace campodata
