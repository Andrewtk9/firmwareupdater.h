#pragma once

#include <stddef.h>
#include <stdint.h>

namespace campodata {

// Incremental decoder for HTTP/1.1 Transfer-Encoding: chunked.
//
// The deployed API sits behind nginx and answers every JSON body chunked, with
// no Content-Length. esp_http_client strips that framing on its own; the
// cellular client speaks HTTP by hand, and without this the size lines end up
// inside the JSON. The provisioning 200 then fails to parse - after the server,
// which hands credentials out exactly once, has already locked the board.
//
// Framing and payload are split so the caller can move payload in bulk: while
// inData() is true, up to dataRemaining() bytes go straight to the destination
// and are reported with consumeData(). Every other byte goes through feed().
class ChunkedDecoder {
public:
    void reset();

    // One framing byte. Ignored while inData(), done() or invalid().
    void feed(char c);

    bool     inData() const { return _phase == Phase::Data; }
    uint32_t dataRemaining() const { return _remaining; }
    void     consumeData(uint32_t n);

    // The zero-size chunk and its trailer have been read.
    bool done() const { return _phase == Phase::Done; }

    // Framing that is not chunked encoding. Sticky until reset().
    bool invalid() const { return _phase == Phase::Invalid; }

private:
    enum class Phase : uint8_t { Size, Extension, Data, DataEnd, Trailer, Done, Invalid };

    void endSizeLine();

    Phase    _phase      = Phase::Size;
    uint32_t _remaining  = 0;
    bool     _size_digit = false;
    bool     _line_empty = true;
};

}  // namespace campodata
