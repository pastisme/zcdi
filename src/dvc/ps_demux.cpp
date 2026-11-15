#include "dvc/ps_demux.h"

namespace zcdi {
namespace dvc {

using namespace mpeg;

void PsDemux::reset() {
    state = kScan;
    shift = 0xffffffff;
    remaining = 0;
    ts_bytes = 0;
}

bool PsDemux::scan(uint8_t b) {
    shift = (shift << 8) | b;
    return (shift & kStartCodePrefixMask) == kStartCodePrefix;
}

void PsDemux::begin(uint8_t code) {
    shift = 0xffffffff;
    if (code == kPackStart) { // 5-byte SCR + 3-byte mux rate
        remaining = 8;
        state = kPack;
    } else if (code >= kSystemHeader) { // and PES packets
        stream = code;
        state = kLengthHigh;
    }
}

uint8_t PsDemux::step(uint8_t b, uint8_t wanted) {
    switch (state) {
        case kLengthHigh:
            remaining = uint16_t(b << 8);
            state = kLengthLow;
            return kNone;
        case kLengthLow:
            remaining |= b;
            state = remaining == 0 ? kScan : stream == wanted ? kPesHeader : kSkip;
            return kNone;
        default: break;
    }

    // Inside a packet: every byte counts against its length.
    const uint8_t current = state;
    if (--remaining == 0) state = kScan;
    const bool more = state != kScan;
    switch (current) {
        case kPack: {
            const int index = 7 - int(remaining); // remaining is 7 after the first byte
            if (index >= 5) return kNone; // mux rate
            ts_accum = (ts_accum << 8) | b;
            if (index != 4) return kNone;
            scr = timestamp();
            return kScr;
        }
        case kPesHeader:
            if (b == kPesStuffing) return kNone;
            if ((b & kPesStdBufferMask) == kPesStdBuffer) {
                if (more) state = kPesStd;
            } else if ((b & kPesPtsMask) == kPesPts) { // the PTS comes first
                has_dts = (b & kPesHasDts) != 0;
                ts_accum = b;
                ts_bytes = has_dts ? 9 : 4;
                if (more) state = kPesTimestamp;
            } else if (more) { // 0x0f: no timestamp
                state = kPayload;
            }
            return kNone;
        case kPesStd:
            if (more) state = kPesHeader;
            return kNone;
        case kPesTimestamp: {
            ts_accum = (ts_accum << 8) | b;
            --ts_bytes;
            uint8_t events = ts_bytes == (has_dts ? 5 : 0) ? kPts : kNone;
            if (ts_bytes == 0) {
                events |= kLastTimestamp;
                if (more) state = kPayload;
            }
            return events;
        }
        case kPayload: return kPayloadByte;
        default: // kSkip
            return kNone;
    }
}

int64_t PsDemux::timestamp() const {
    const uint64_t t = ts_accum & 0xffffffffffull;
    return int64_t(((t >> 33) & 0x7) << 30 | ((t >> 17) & 0x7fff) << 15 | ((t >> 1) & 0x7fff));
}

} // namespace dvc
} // namespace zcdi
