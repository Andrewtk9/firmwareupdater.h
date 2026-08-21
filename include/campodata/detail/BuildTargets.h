#pragma once

// Which platform layer is in play. Every file under src/platform/ guards its
// whole body with one of these, so a build-filter mistake yields an empty
// translation unit instead of a wall of errors.
//
// Under PlatformIO mixed mode (framework = arduino, espidf) both ARDUINO and
// ESP_PLATFORM are defined; Arduino wins, which is correct because the Arduino
// networking stack is still the one linked.

#if defined(ARDUINO)
    #define FWUP_TARGET_ARDUINO 1
    #define FWUP_TARGET_IDF     0
    #define FWUP_TARGET_NATIVE  0
#elif defined(ESP_PLATFORM)
    #define FWUP_TARGET_ARDUINO 0
    #define FWUP_TARGET_IDF     1
    #define FWUP_TARGET_NATIVE  0
#else
    #define FWUP_TARGET_ARDUINO 0
    #define FWUP_TARGET_IDF     0
    #define FWUP_TARGET_NATIVE  1
#endif

// True for anything running on real silicon, where the ESP-IDF C APIs are
// linkable. Most of the platform layer is shared between the two frameworks
// because those APIs ship inside the Arduino framework too.
#define FWUP_TARGET_ESP32 (FWUP_TARGET_ARDUINO || FWUP_TARGET_IDF)
