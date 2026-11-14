#pragma once
// VMPEG register byte offsets (from 0xe00000) and big-endian access.
#include <cstdint>

#include "vmpeg/vmpeg.h"

namespace zcdi_test {

constexpr uint32_t kFmaCmdOffset = zcdi::mamecdi::vmpeg::kFmaCmd * 2;              // 0xe03000
constexpr uint32_t kFmaStreamWOffset = zcdi::mamecdi::vmpeg::kFmaStreamW * 2;          // 0xe03008
constexpr uint32_t kFmaDclkOffset = zcdi::mamecdi::vmpeg::kFmaDclkH * 2;             // 0xe03010, high word then low
constexpr uint32_t kFmaIsrOffset = zcdi::mamecdi::vmpeg::kFmaIsr * 2;              // 0xe0301a, cleared by reading its odd byte
constexpr uint32_t kFmvImageWidthOffset = zcdi::mamecdi::vmpeg::kFmvImageWidth * 2;       // 0xe04002
constexpr uint32_t kFmvImageHeightOffset = zcdi::mamecdi::vmpeg::kFmvImageHeight * 2;      // 0xe04004
constexpr uint32_t kFmvDecoderTimecodeOffset = zcdi::mamecdi::vmpeg::kFmvDecoderTimecodeHi * 2;  // 0xe04008
constexpr uint32_t kFmvPicRtOffset = zcdi::mamecdi::vmpeg::kFmvPicRt * 2;            // 0xe04056
constexpr uint32_t kFmvDisplayTimecodeOffset = zcdi::mamecdi::vmpeg::kFmvDisplayTimecodeHi * 2;  // 0xe04058
constexpr uint32_t kFmvSysStsOffset = zcdi::mamecdi::vmpeg::kFmvSysSts * 2;           // 0xe0405e
constexpr uint32_t kFmvIsrOffset = zcdi::mamecdi::vmpeg::kFmvIsr * 2;              // 0xe04062
constexpr uint32_t kFmvTcntOffset = zcdi::mamecdi::vmpeg::kFmvTcnt * 2;             // 0xe04064
constexpr uint32_t kFmvScrposXOffset = zcdi::mamecdi::vmpeg::kFmvScrposX * 2;          // 0xe04076
constexpr uint32_t kFmvGenDecCmdOffset = zcdi::mamecdi::vmpeg::kFmvGenDecCmd * 2;        // 0xe04088
constexpr uint32_t kFmvGenVdiCmdOffset = zcdi::mamecdi::vmpeg::kFmvGenVdiCmd * 2;        // 0xe0408c
constexpr uint32_t kFmvGenSyscrOffset = zcdi::mamecdi::vmpeg::kFmvGenSyscr * 2;         // 0xe04098
constexpr uint32_t kFmvDtsOffset = zcdi::mamecdi::vmpeg::kFmvDts * 2;              // 0xe040a0
constexpr uint32_t kFmvPicsInFifoOffset = zcdi::mamecdi::vmpeg::kFmvPicsInFifo * 2;       // 0xe040a4
constexpr uint32_t kFmvPicRateOffset = zcdi::mamecdi::vmpeg::kFmvPicRate * 2;          // 0xe040a8
constexpr uint32_t kFmvFrameRateOffset = zcdi::mamecdi::vmpeg::kFmvFrameRate * 2;        // 0xe040ac
constexpr uint32_t kFmvTrldOffset = zcdi::mamecdi::vmpeg::kFmvTrld * 2;             // 0xe040ae
constexpr uint32_t kFmvSyscmdOffset = zcdi::mamecdi::vmpeg::kFmvSyscmd * 2;           // 0xe040c0
constexpr uint32_t kFmvVidcmdOffset = zcdi::mamecdi::vmpeg::kFmvVidcmd * 2;           // 0xe040c2
constexpr uint32_t kFmvStreamNumOffset = zcdi::mamecdi::vmpeg::kFmvStreamNum * 2;        // 0xe040c4
constexpr uint32_t kFmvSysScrOffset = zcdi::mamecdi::vmpeg::kFmvSysScr * 2;           // 0xe040c6

inline void write16(zcdi::mamecdi::VmpegDevice& v, uint32_t offset, uint16_t value) {
    v.write_byte(offset, uint8_t(value >> 8));
    v.write_byte(offset + 1, uint8_t(value));
}

inline uint16_t read16(zcdi::mamecdi::VmpegDevice& v, uint32_t offset) {
    const uint8_t hi = v.read_byte(offset);
    return uint16_t(hi << 8 | v.read_byte(offset + 1));
}

// High word first: reading DCLK-H latches DCLK-L.
inline uint32_t read32(zcdi::mamecdi::VmpegDevice& v, uint32_t offset) {
    const uint32_t hi = read16(v, offset);
    return hi << 16 | read16(v, offset + 2);
}

}  // namespace zcdi_test
