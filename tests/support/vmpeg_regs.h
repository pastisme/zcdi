#pragma once
// VMPEG register byte offsets (from 0xe00000) and big-endian access.
#include <cstdint>

#include "dvc/dvc.h"

namespace zcdi_test {

constexpr uint32_t kFmaCmdOffset = zcdi::dvc::vmpeg::kFmaCmd * 2;              // 0xe03000
constexpr uint32_t kFmaStreamWOffset = zcdi::dvc::vmpeg::kFmaStreamW * 2;          // 0xe03008
constexpr uint32_t kFmaDclkOffset = zcdi::dvc::vmpeg::kFmaDclkH * 2;             // 0xe03010, high word then low
constexpr uint32_t kFmaIsrOffset = zcdi::dvc::vmpeg::kFmaIsr * 2;              // 0xe0301a, cleared by reading its odd byte
constexpr uint32_t kFmvImageWidthOffset = zcdi::dvc::vmpeg::kFmvImageWidth * 2;       // 0xe04002
constexpr uint32_t kFmvImageHeightOffset = zcdi::dvc::vmpeg::kFmvImageHeight * 2;      // 0xe04004
constexpr uint32_t kFmvDecoderTimecodeOffset = zcdi::dvc::vmpeg::kFmvDecoderTimecodeHi * 2;  // 0xe04008
constexpr uint32_t kFmvPicRtOffset = zcdi::dvc::vmpeg::kFmvPicRt * 2;            // 0xe04056
constexpr uint32_t kFmvDisplayTimecodeOffset = zcdi::dvc::vmpeg::kFmvDisplayTimecodeHi * 2;  // 0xe04058
constexpr uint32_t kFmvSysStsOffset = zcdi::dvc::vmpeg::kFmvSysSts * 2;           // 0xe0405e
constexpr uint32_t kFmvIsrOffset = zcdi::dvc::vmpeg::kFmvIsr * 2;              // 0xe04062
constexpr uint32_t kFmvTcntOffset = zcdi::dvc::vmpeg::kFmvTcnt * 2;             // 0xe04064
constexpr uint32_t kFmvScrposXOffset = zcdi::dvc::vmpeg::kFmvScrposX * 2;          // 0xe04076
constexpr uint32_t kFmvGenDecCmdOffset = zcdi::dvc::vmpeg::kFmvGenDecCmd * 2;        // 0xe04088
constexpr uint32_t kFmvGenVdiCmdOffset = zcdi::dvc::vmpeg::kFmvGenVdiCmd * 2;        // 0xe0408c
constexpr uint32_t kFmvGenSyscrOffset = zcdi::dvc::vmpeg::kFmvGenSyscr * 2;         // 0xe04098
constexpr uint32_t kFmvDtsOffset = zcdi::dvc::vmpeg::kFmvDts * 2;              // 0xe040a0
constexpr uint32_t kFmvPicsInFifoOffset = zcdi::dvc::vmpeg::kFmvPicsInFifo * 2;       // 0xe040a4
constexpr uint32_t kFmvPicRateOffset = zcdi::dvc::vmpeg::kFmvPicRate * 2;          // 0xe040a8
constexpr uint32_t kFmvFrameRateOffset = zcdi::dvc::vmpeg::kFmvFrameRate * 2;        // 0xe040ac
constexpr uint32_t kFmvTrldOffset = zcdi::dvc::vmpeg::kFmvTrld * 2;             // 0xe040ae
constexpr uint32_t kFmvSyscmdOffset = zcdi::dvc::vmpeg::kFmvSyscmd * 2;           // 0xe040c0
constexpr uint32_t kFmvVidcmdOffset = zcdi::dvc::vmpeg::kFmvVidcmd * 2;           // 0xe040c2
constexpr uint32_t kFmvStreamNumOffset = zcdi::dvc::vmpeg::kFmvStreamNum * 2;        // 0xe040c4
constexpr uint32_t kFmvSysScrOffset = zcdi::dvc::vmpeg::kFmvSysScr * 2;           // 0xe040c6

// Dvc or Dvc.
template <class Device>
inline void write16(Device& v, uint32_t offset, uint16_t value) {
    v.write_byte(offset, uint8_t(value >> 8));
    v.write_byte(offset + 1, uint8_t(value));
}

template <class Device>
inline uint16_t read16(Device& v, uint32_t offset) {
    const uint8_t hi = v.read_byte(offset);
    return uint16_t(hi << 8 | v.read_byte(offset + 1));
}

// High word first: reading DCLK-H latches DCLK-L.
template <class Device>
inline uint32_t read32(Device& v, uint32_t offset) {
    const uint32_t hi = read16(v, offset);
    return hi << 16 | read16(v, offset + 2);
}

}  // namespace zcdi_test
