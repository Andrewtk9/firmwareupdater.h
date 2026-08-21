#pragma once

#include "campodata/Types.h"

namespace campodata {

// Request body of POST /api/v1/provisioning, spec section 4.1.
//
// The spec contradicts itself here: section 4.1 lists four fields while figure 2
// step 3 shows only {board_id, firmware_version, repo}. We send the superset.
//
// `rede` is not in the spec at all, but the deployed server requires it:
// ProvisionamentoProperties.brokerDoPerfil() picks the broker profile from it,
// and anything other than "gprs" falls through to the wifi profile - 8883 with
// TLS, which the cellular stack cannot open. Omitting it silently hands every
// GPRS device credentials it can never use.
struct ProvisionRequest {
    const char* board_id         = nullptr;
    const char* hardware_model   = nullptr;
    const char* firmware_version = nullptr;
    const char* repo             = nullptr;
    LinkType    rede             = LinkType::None;  // None = let the server default
};

// What the server's HTTP status means for the state machine.
enum class ProvisionOutcome : uint8_t {
    Granted,        // 200, credentials in hand
    BadRequest,     // 400: deployed server returns this for a missing board_id
    NotReleased,    // 403: known board, not released or already locked
    UnknownBoard,   // 404: board_id not registered at all
    Concurrent,     // 409: another provisioning in flight
    RateLimited,    // 429
    ServerError,    // 5xx
    TransportError, // never reached the server
};

namespace provisioning {

// Serialises the request. `out` needs ~256 bytes for realistic inputs.
CodecError buildRequest(const ProvisionRequest& req, char* out, size_t cap);

// Parses the 200 response into NVS-ready values.
//
// Topics are optional in practice: the spec always sends them, but a server
// that omits them leaves the device to fall back on its configured scheme.
CodecError parseResponse(const char* json, Provisioning& out);

ProvisionOutcome fromHttpStatus(int status);

// 403/404 mean the board is not going to be released by retrying harder, so
// these back off long. 409/429/5xx are transient.
bool isTransient(ProvisionOutcome);

const char* toString(ProvisionOutcome);
const char* toString(CodecError);

}  // namespace provisioning
}  // namespace campodata
