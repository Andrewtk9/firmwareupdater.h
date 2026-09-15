#include "core/ChunkedDecoder.h"

namespace campodata {
namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

void ChunkedDecoder::reset() {
    _phase      = Phase::Size;
    _remaining  = 0;
    _size_digit = false;
    _line_empty = true;
}

void ChunkedDecoder::feed(char c) {
    switch (_phase) {
    case Phase::Size: {
        if (c == '\r' || c == ' ' || c == '\t') return;
        if (c == '\n') {
            endSizeLine();
            return;
        }
        if (c == ';') {
            _phase = _size_digit ? Phase::Extension : Phase::Invalid;
            return;
        }
        const int v = hexValue(c);
        // A size that does not fit 32 bits is not a body this device can hold.
        if (v < 0 || _remaining > (UINT32_MAX >> 4)) {
            _phase = Phase::Invalid;
            return;
        }
        _remaining  = (_remaining << 4) | static_cast<uint32_t>(v);
        _size_digit = true;
        return;
    }

    case Phase::Extension:
        if (c == '\n') endSizeLine();
        return;

    case Phase::DataEnd:
        if (c == '\r') return;
        // Anything but CRLF here means the declared size was wrong.
        _phase      = (c == '\n') ? Phase::Size : Phase::Invalid;
        _remaining  = 0;
        _size_digit = false;
        return;

    case Phase::Trailer:
        if (c == '\r') return;
        if (c == '\n') {
            if (_line_empty) _phase = Phase::Done;
            _line_empty = true;
            return;
        }
        _line_empty = false;
        return;

    case Phase::Data:
    case Phase::Done:
    case Phase::Invalid:
        return;
    }
}

void ChunkedDecoder::endSizeLine() {
    if (!_size_digit) {
        _phase = Phase::Invalid;
        return;
    }
    if (_remaining == 0) {
        _phase      = Phase::Trailer;
        _line_empty = true;
    } else {
        _phase = Phase::Data;
    }
}

void ChunkedDecoder::consumeData(uint32_t n) {
    if (_phase != Phase::Data) return;
    _remaining = (n < _remaining) ? _remaining - n : 0;
    if (_remaining == 0) _phase = Phase::DataEnd;
}

}  // namespace campodata
