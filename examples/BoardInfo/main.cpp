// Prints what the library detects about this board and whether the OTA
// rollback window is actually open.
//
// Run this first on any new hardware. If rollback reports "bypassed", the
// strong verifyRollbackLater() override did not get linked and automatic
// rollback will silently never happen.

#include <Arduino.h>

#include "campodata/Types.h"
#include "core/HardwareModel.h"
#include "platform/esp32/Esp32Hardware.h"
#include "platform/esp32/Esp32OtaSink.h"
#include "platform/esp32/RollbackGuard.h"

using namespace campodata;

// verifyRollbackLater() comes from FirmwareUpdater.cpp, which the library
// always links. Defining it here too would be a duplicate symbol.

void setup() {
    Serial.begin(115200);
    delay(300);  // let the USB CDC enumerate before the first print

    HardwareInfo hw;
    if (!platform::probeHardware(hw)) {
        Serial.println(F("hardware probe failed"));
        return;
    }

    char model[kMaxModelLen] = {};
    hardware::classify(hw, model, sizeof(model));

    char board[13] = {};
    hardware::boardId(hw.mac, board, sizeof(board));

    Serial.println();
    Serial.println(F("--- campodata firmware updater ---"));
    Serial.printf("board_id       : %s\n", board);
    Serial.printf("hardware_model : %s\n", model);
    Serial.printf("chip           : %s rev %u, %u core(s)\n",
                  hw.chip, hw.revision, hw.cores);
    Serial.printf("flash          : %u bytes\n", hw.flash_bytes);
    Serial.printf("psram          : %u bytes%s\n", hw.psram_bytes,
                  hw.psram_bytes ? "" : "  (none - this is a WROOM/esp32dev)");
    Serial.printf("build board    : %s\n", hw.board_build);

    Serial.println();
    if (hw.ota_capable) {
        Serial.printf("ota slot       : %u bytes at 0x%06X\n",
                      hw.ota.size_bytes, hw.ota.offset);
        Serial.printf("               : the real cap - the fleet hardcodes 2000000\n");
    } else {
        Serial.println(F("ota slot       : NONE - partition table has no second app slot"));
    }
    Serial.printf("factory boot   : %s\n", hw.running_is_factory ? "yes" : "no");

    Esp32OtaSink sink;
    Serial.printf("pending verify : %s\n", sink.pendingVerify() ? "yes" : "no");

    const auto guard = rollback::check(sink.pendingVerify());
    Serial.printf("rollback       : supported=%s state=%s\n",
                  rollback::supported() ? "yes" : "no",
                  rollback::toString(guard));

    char running[kMaxVersionLen] = {};
    if (Esp32OtaSink::runningVersion(running, sizeof(running))) {
        Serial.printf("running image  : %s\n", running);
    }
    Serial.println(F("----------------------------------"));
}

void loop() {
    delay(1000);
}
