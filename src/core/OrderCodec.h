#pragma once

#include "campodata/Types.h"

namespace campodata {

// Why an order was refused before any byte was downloaded. Every one of these
// is cheaper to detect than to discover halfway through a 20-minute transfer.
enum class OrderRejection : uint8_t {
    None,
    Malformed,
    MissingField,
    WrongRepo,        // order targets another project
    SameVersion,      // already running it
    Downgrade,        // older than what is installed
    TooLarge,         // will not fit the real OTA slot
    NoSha256,         // no digest and the policy demands one
    Duplicate,        // already handled: retained topics redeliver constantly
};

namespace order {

// Parses the payload of /update/<id>, spec section 7.1.
CodecError parse(const char* json, UpdateOrder& out);

// Everything checkable before committing to a download.
//
// `current_version` and `current_repo` come from NVS, `slot_bytes` from the
// real partition, `last_update_id` from NVS as the dedupe key.
//
// The repo check is not in the spec, but without it a misrouted order flashes
// another project's firmware onto this device.
OrderRejection validate(const UpdateOrder& order,
                        const char* current_version,
                        const char* current_repo,
                        uint32_t slot_bytes,
                        const char* last_update_id,
                        bool allow_downgrade);

// Compares dotted numeric versions. Returns <0, 0 or >0. Non-numeric parts
// compare as equal so a build suffix never blocks an update.
int compareVersions(const char* a, const char* b);

const char* toString(OrderRejection);

}  // namespace order
}  // namespace campodata
