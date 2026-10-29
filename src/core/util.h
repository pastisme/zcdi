#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace zcdi {

inline std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    return std::string(s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1));
}

// Packed BCD (Binary-coded decimal), two digits (0-99).
constexpr uint8_t to_bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
constexpr int from_bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0xf); }

// Frames (75 per second) as packed BCD minutes, seconds, frames; minutes
// wrap at 100 when `wrap_minutes`.
constexpr void to_bcd_msf(int frames, uint8_t& m, uint8_t& s, uint8_t& f, bool wrap_minutes = false) {
    const int minutes = frames / (60 * 75);
    m = to_bcd(wrap_minutes ? minutes % 100 : minutes);
    s = to_bcd(frames / 75 % 60);
    f = to_bcd(frames % 75);
}

// Saturates to an S16 sample, or to a U8 colour component.
constexpr int16_t clamp16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }
constexpr uint8_t clamp_u8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// A 16-bit register written through two byte lanes.
constexpr uint16_t set_hi(uint16_t reg, uint8_t v) { return uint16_t((reg & 0x00ff) | (v << 8)); }
constexpr uint16_t set_lo(uint16_t reg, uint8_t v) { return uint16_t((reg & 0xff00) | v); }

// One byte lane of a wider register, `shift` bits up.
template <class T>
constexpr T set_byte(T reg, int shift, uint8_t v) {
    return T((reg & ~(T(0xff) << shift)) | (T(v) << shift));
}

} // namespace zcdi
