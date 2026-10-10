#pragma once
// VMPEG register byte offsets (from 0xe00000, as in vmpeg.cpp) and big-endian access.
#include <cstdint>

#include "vmpeg/vmpeg.h"

namespace zcdi_test {

constexpr uint32_t kFmaCmdOffset = 0x1800 * 2;              // 0xe03000
constexpr uint32_t kFmaStreamWOffset = 0x1804 * 2;          // 0xe03008
constexpr uint32_t kFmaDclkOffset = 0x1808 * 2;             // 0xe03010, high word then low
constexpr uint32_t kFmaIsrOffset = 0x180d * 2;              // 0xe0301a, cleared by reading its odd byte
constexpr uint32_t kFmvDisplayTimecodeOffset = 0x2004 * 2;  // 0xe04008
constexpr uint32_t kFmvDecoderTimecodeOffset = 0x202c * 2;  // 0xe04058
constexpr uint32_t kFmvSysStsOffset = 0x202f * 2;           // 0xe0405e
constexpr uint32_t kFmvIsrOffset = 0x2031 * 2;              // 0xe04062
constexpr uint32_t kFmvTcntOffset = 0x2032 * 2;             // 0xe04064
constexpr uint32_t kFmvGenDecCmdOffset = 0x2044 * 2;        // 0xe04088
constexpr uint32_t kFmvGenVdiCmdOffset = 0x2046 * 2;        // 0xe0408c
constexpr uint32_t kFmvGenSyscrOffset = 0x204c * 2;         // 0xe04098
constexpr uint32_t kFmvDtsOffset = 0x2050 * 2;              // 0xe040a0
constexpr uint32_t kFmvPicsInFifoOffset = 0x2052 * 2;       // 0xe040a4
constexpr uint32_t kFmvTrldOffset = 0x2057 * 2;             // 0xe040ae
constexpr uint32_t kFmvSyscmdOffset = 0x2060 * 2;           // 0xe040c0
constexpr uint32_t kFmvVidcmdOffset = 0x2061 * 2;           // 0xe040c2
constexpr uint32_t kFmvStreamNumOffset = 0x2062 * 2;        // 0xe040c4
constexpr uint32_t kFmvSysScrOffset = 0x2063 * 2;           // 0xe040c6

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
