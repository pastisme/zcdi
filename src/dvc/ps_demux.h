#pragma once
#include <cstdint>

namespace zcdi {
namespace dvc {

// MPEG-1 system stream syntax.
namespace mpeg {

// MPEG-1 start codes (ISO 11172), each after the 00 00 01 prefix.
constexpr uint32_t kStartCodePrefix = 0x00000100;
constexpr uint32_t kStartCodePrefixMask = 0xffffff00;
enum StartCode : uint8_t {
    kPictureStart = 0x00,
    kSequenceHeader = 0xb3,
    kSequenceEnd = 0xb7,
    kProgramEnd = 0xb9, // ISO 11172 end code
    kPackStart = 0xba,
    kSystemHeader = 0xbb, // this and above carry a 16-bit length
    kAudioStream = 0xc0, // + stream number
    kVideoStream = 0xe0, // + stream number
};

// PES header bytes before the payload (ISO 11172-1 2.4.3.3).
enum PesHeader : uint8_t {
    kPesHasDts = 0x10,
    kPesPts = 0x20, // 0010 PTS or 0011 PTS + DTS
    kPesStdBuffer = 0x40, // P-STD buffer size, 2 bytes
    kPesStdBufferMask = 0xc0,
    kPesPtsMask = 0xe0,
    kPesStuffing = 0xff,
};

} // namespace mpeg

// MPEG-1 pack and PES parsing for one channel. The
// channels handle start codes themselves (scan(), then begin()).
struct PsDemux {
    enum : uint8_t {
        kScan = 0,
        kLengthHigh,
        kLengthLow,
        kSkip,
        kPesHeader,
        kPesStd,
        kPesTimestamp,
        kPayload,
        kPack,
    };
    // step() results, OR'd.
    enum : uint8_t { kNone = 0, kScr = 1, kPts = 2, kLastTimestamp = 4, kPayloadByte = 8 };

    uint8_t state = kScan;
    uint8_t stream = 0; // id of the packet being read
    uint32_t shift = 0xffffffff;
    uint16_t remaining = 0;
    uint8_t ts_bytes = 0; // timestamp bytes still to read
    bool has_dts = false; // this PES carries PTS and DTS
    uint64_t ts_accum = 0;
    int64_t scr = 0; // last pack SCR, 90 kHz

    // Back to kScan, start code search restarted.
    void reset();
    // In kScan: true once `b` completes a start code.
    bool scan(uint8_t b);
    // Acts on start code `code`: a pack header, or a packet with a 16-bit length.
    void begin(uint8_t code);
    // Any state but kScan; only packets of stream `wanted` are parsed.
    uint8_t step(uint8_t b, uint8_t wanted);
    // The 33-bit, 90 kHz timestamp in the last 5 bytes read.
    int64_t timestamp() const;
};

} // namespace dvc
} // namespace zcdi
