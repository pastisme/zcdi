#include "chips/mcd212/mcd212.h"

#include "core/util.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace zcdi {
namespace chips {

using namespace mcd212;

namespace {
// Each code repeated `repeat` times (Pixel Repeat; 1 = none), wrapping with `mask`.
void fetch_codes(uint8_t* dst, int count, const uint8_t* ram, std::size_t start, std::size_t mask, int repeat) {
    if (repeat == 1 && (start & mask) + count <= mask + 1) {
        std::memcpy(dst, ram + (start & mask), std::size_t(count)); // the usual case: no repeat, no wrap
        return;
    }
    std::size_t addr = start;
    for (int i = 0; i < count; i += repeat) {
        const uint8_t code = ram[addr++ & mask];
        for (int k = 0; k < repeat && i + k < count; ++k)
            dst[i + k] = code;
    }
}

// Pixel Repeat factor from DDR bits 10-11.
int pixel_repeat(uint16_t ddr) { return 2 << ((ddr >> kDdrMtShift) & 0x3); }

// Each term truncated on its own.
struct DyuvTerms {
    int16_t v_to_r[256], u_to_g[256], v_to_g[256], u_to_b[256];
};

constexpr DyuvTerms make_dyuv_terms() {
    DyuvTerms t{};
    for (int i = 0; i < 256; ++i) {
        const int c = i - 128;
        t.v_to_r[i] = int16_t((351 * c) / 256);
        t.u_to_g[i] = int16_t((86 * c) / 256);
        t.v_to_g[i] = int16_t((179 * c) / 256);
        t.u_to_b[i] = int16_t((444 * c) / 256);
    }
    return t;
}

constexpr DyuvTerms kDyuvTerms = make_dyuv_terms();

// clamp_u8() over -222..477 (Y plus the terms above), indexed by value + 256.
struct DyuvClamp {
    uint8_t of[768];
};

constexpr DyuvClamp make_dyuv_clamp() {
    DyuvClamp c{};
    for (int i = 0; i < 768; ++i)
        c.of[i] = clamp_u8(i - 256);
    return c;
}

constexpr DyuvClamp kDyuvClamp = make_dyuv_clamp();

uint32_t dyuv_to_rgb(uint8_t y, uint8_t u, uint8_t v) {
    const uint32_t r = kDyuvClamp.of[256 + y + kDyuvTerms.v_to_r[v]];
    const uint32_t g = kDyuvClamp.of[256 + y - kDyuvTerms.u_to_g[u] - kDyuvTerms.v_to_g[v]];
    const uint32_t b = kDyuvClamp.of[256 + y + kDyuvTerms.u_to_b[u]];
    return (r << 16) | (g << 8) | b;
}
} // namespace

void Mcd212::decode_dyuv_line(const uint8_t* src, uint32_t* row, uint32_t dyuv_abs_start) const {
    // Byte: U or V delta (high nibble), Y delta (low). Reseeded every line.
    uint8_t y = uint8_t(dyuv_abs_start >> 16);
    uint8_t u = uint8_t(dyuv_abs_start >> 8);
    uint8_t v = uint8_t(dyuv_abs_start);

    for (int x = 0; x < kLineWidth; x += 2) {
        uint8_t b0 = src[x];
        uint8_t b1 = src[x + 1];
        uint8_t u1 = uint8_t(u + kDyuvDeltas[b0 >> 4]);
        uint8_t y0 = uint8_t(y + kDyuvDeltas[b0 & 0xf]);
        uint8_t v1 = uint8_t(v + kDyuvDeltas[b1 >> 4]);
        uint8_t y1 = uint8_t(y0 + kDyuvDeltas[b1 & 0xf]);
        uint8_t u0 = uint8_t((unsigned(u) + u1) >> 1);
        uint8_t v0 = uint8_t((unsigned(v) + v1) >> 1);

        row[x] = dyuv_to_rgb(y0, u0, v0);
        row[x + 1] = dyuv_to_rgb(y1, u1, v1);

        y = y1;
        u = u1;
        v = v1;
    }
}

// Entry (byte & mask) | bank.
Mcd212::ClutIndex Mcd212::clut_index(int path, uint8_t icm) const {
    // CM1=1 is RGB555 (Green Book V.4.6.1), handled before this.
    if (icm == kIcmClut8) return path == 0 ? ClutIndex{.valid = true, .mask = 0xff, .bank = 0x00} : ClutIndex{};
    // CLUT7: plane B's palette is in banks 2-3, where its bank register points.
    if (icm == kIcmClut7) return {.valid = true, .mask = 0x7f, .bank = uint8_t(path == 1 ? kClutPlaneB : 0x00)};
    // Plane A's dual CLUT7, CS picks the half; plane B's CM1=4 is reserved.
    if (icm == kIcmClut77) {
        return path == 0 ? ClutIndex{.valid = true, .mask = 0x7f, .bank = uint8_t(icm_cs_ ? kClutPlaneB : 0x00)}
                         : ClutIndex{.valid = true, .mask = 0x00, .bank = 0x00};
    }
    return {}; // reserved (CLUT4 is decoded elsewhere)
}

inline uint32_t Mcd212::clut_color(const ClutIndex& index, uint8_t byte) const {
    return index.valid ? clut_[(byte & index.mask) | index.bank] : kStudioBlack;
}

uint32_t Mcd212::clut_pixel(int path, uint8_t byte, uint8_t icm) const {
    return clut_color(clut_index(path, icm), byte);
}

void Mcd212::apply_pixel_hold(int path, uint32_t* row, bool* opaque, int width) const {
    const uint32_t control = pixel_hold_[path & 1];
    if ((control & kPixelHoldEnable) == 0) return;
    const int factor = int(control & 0xffu);
    // 0 is reserved (Green Book V.5.11.1.1); treat it as off, not a whole-line latch.
    if (factor <= 1) return;

    for (int x = 0; x < width; x += factor) {
        const uint32_t held_color = row[x];
        const bool held_opaque = opaque[x];
        const int end = (x + factor < width) ? x + factor : width;
        for (int i = x + 1; i < end; ++i) {
            row[i] = held_color;
            opaque[i] = held_opaque;
        }
    }
}

void Mcd212::render_line_rle(int path, uint32_t* row, bool* present, const uint8_t* plane_ram, std::size_t plane_mask) {
    // RL7: bit 7 set = run, next byte its
    // length (0 = to end of line); clear = one pixel.
    const ClutIndex index = clut_index(path, icm_[path]);
    uint32_t vsr = vsr_address(path);
    // The line ends at its active pixels; the next line's data follows.
    const int line_end = active_width_;
    int x = 0;
    while (x < line_end) {
        uint8_t byte = plane_ram[vsr & plane_mask];
        ++vsr;
        if (byte & 0x80) {
            uint8_t length = plane_ram[vsr & plane_mask];
            ++vsr;
            uint32_t entry = clut_color(index, byte & 0x7f);
            int count = length ? length : (line_end - x);
            int end = x + count;
            if (end > line_end) end = line_end;
            for (int i = x; i < end; ++i) {
                row[i] = entry;
                present[i] = true;
            }
            x = end;
        } else {
            uint32_t entry = clut_color(index, byte);
            row[x] = entry;
            present[x] = true;
            ++x;
        }
    }
    for (; x < kLineWidth; ++x) {
        row[x] = kStudioBlack;
        present[x] = false;
    }
    set_vsr_address(path, vsr);
}

void Mcd212::render_line_rgb555(uint32_t* row, bool* t_bit, const uint8_t* plane_a, const uint8_t* plane_b,
                                std::size_t plane_size) {
    // Green Book V.4.4.4: store 0 = T:R4..0:G4..3, store 1 = G2..0:B4..0.
    const std::size_t mask = plane_size - 1;
    const uint32_t addr_a = vsr_address(0);
    const uint32_t addr_b = vsr_address(1);

    // Pixel Repeat (V.5.11.1.2) from plane B's DDR, the same for both stores.
    const uint8_t rms_b = uint8_t((ddr_[1] >> kDdrFtShift) & 0x3);
    const int repeat_factor = rms_b == kDdrFtMosaic ? pixel_repeat(ddr_[1]) : 1;
    uint8_t upper_codes[kLineWidth], lower_codes[kLineWidth];
    fetch_codes(upper_codes, kLineWidth, plane_a, addr_a, mask, repeat_factor);
    fetch_codes(lower_codes, kLineWidth, plane_b, addr_b, mask, repeat_factor);
    for (int x = 0; x < kLineWidth; ++x) {
        const uint8_t upper = upper_codes[x];
        const uint8_t lower = lower_codes[x];
        const bool transparency_bit = (upper & 0x80) != 0;
        const uint8_t r5 = (upper >> 2) & 0x1f;
        const uint8_t g5 = uint8_t((upper & 0x03) << 3) | (lower >> 5);
        const uint8_t b5 = lower & 0x1f;
        row[x] = (uint32_t(r5) << 19) | (uint32_t(g5) << 11) | (uint32_t(b5) << 3);
        t_bit[x] = transparency_bit;
    }
    set_vsr_address(0, addr_a + active_width_ / repeat_factor);
    set_vsr_address(1, addr_b + active_width_ / repeat_factor);
}

// No pixels this line: coding OFF or VSR 0 (all 22 bits).
bool Mcd212::plane_parked(int path) const {
    return !(dcr_[path] & kDcrIc) || icm_[path] == kIcmOff || vsr_address(path) == 0;
}

void Mcd212::decode_plane_line(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row,
                               bool* present) {
    const uint16_t ddr = ddr_[path];
    uint8_t icm = icm_[path];

    if (plane_parked(path)) {
        const bool shown = (dcr_[path] & kDcrIc) != 0;
        for (int i = 0; i < kLineWidth; ++i) {
            row[i] = kStudioBlack;
            present[i] = shown;
        }
        return;
    }

    std::size_t mask = plane_size - 1;

    // Neither RLE nor Pixel Repeat: bitmap.
    const uint8_t rms = uint8_t((ddr >> kDdrFtShift) & 0x3);
    if (rms == kDdrFtRle) {
        render_line_rle(path, row, present, plane_ram, mask);
        return;
    }

    const uint32_t byte_off = vsr_address(path);
    uint8_t line_bytes[kLineWidth];
    // Pixel Repeat repeats codes, not decoded RGB, and not for DYUV (V.5.11.1.2).
    const bool repeat = rms == kDdrFtMosaic && icm != kIcmDyuv && icm != kIcmClut4;
    const int repeat_factor = repeat ? pixel_repeat(ddr) : 1;
    fetch_codes(line_bytes, kLineWidth, plane_ram, byte_off, mask, repeat_factor);
    if (icm == kIcmDyuv) {
        decode_dyuv_line(line_bytes, row, dyuv_abs_start_[path]);
    } else {
        const ClutIndex index = clut_index(path, icm);
        for (int i = 0; i < kLineWidth; ++i)
            row[i] = clut_color(index, line_bytes[i]);
    }
    std::fill(present, present + kLineWidth, true);
    // VSR advances by the bytes fetched for the active pixels.
    set_vsr_address(path, byte_off + active_width_ / repeat_factor);
}

void Mcd212::preview_plane(int path, uint32_t start, int lines, const uint8_t* plane_ram, std::size_t plane_size,
                           uint32_t* out) const {
    Mcd212 chip = *this;
    chip.set_vsr_address(path, start);
    for (int y = 0; y < lines; ++y) {
        uint32_t* row = out + static_cast<std::size_t>(y) * kFbWidth;
        if (chip.icm_[path] == kIcmClut4) {
            bool present[kFbWidth];
            chip.decode_double_res_line(path, plane_ram, plane_size, row, present);
            continue;
        }
        uint32_t lores[kLineWidth];
        bool present[kLineWidth];
        chip.decode_plane_line(path, plane_ram, plane_size, lores, present);
        for (int x = 0; x < kFbWidth; ++x)
            row[x] = lores[x / 2];
    }
}

// CLUT4/RL3 (Green Book Figure V.34): two pixels per byte, high nibble first;
// Double Resolution regardless of DCR's CM bit.
void Mcd212::decode_double_res_line(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row,
                                    bool* present) {
    const uint16_t ddr = ddr_[path];
    if (plane_parked(path)) {
        const bool shown = (dcr_[path] & kDcrIc) != 0;
        for (int x = 0; x < kFbWidth; ++x) {
            row[x] = kStudioBlack;
            present[x] = shown;
        }
        return;
    }
    uint32_t vsr = vsr_address(path);
    const std::size_t mask = plane_size - 1;
    const uint8_t rms = uint8_t((ddr >> kDdrFtShift) & 0x3);
    const bool run_length = rms == kDdrFtRle;
    // One byte per active Normal Resolution pixel.
    uint8_t codes[kLineWidth] = {};
    const int units = active_width_;
    if (run_length) {
        int x = 0;
        while (x < units) {
            const uint8_t byte = plane_ram[vsr & mask];
            ++vsr;
            if (byte & 0x80) {
                const uint8_t length = plane_ram[vsr & mask];
                ++vsr;
                const int end = std::min(units, x + (length ? int(length) : units - x));
                std::fill(codes + x, codes + end, uint8_t(byte & 0x7f));
                x = end;
            } else {
                codes[x++] = byte;
            }
        }
    } else {
        const int repeat_factor = rms == kDdrFtMosaic ? pixel_repeat(ddr) : 1;
        fetch_codes(codes, units, plane_ram, vsr, mask, repeat_factor);
        vsr += uint32_t(units / repeat_factor);
    }
    const uint8_t bank = path == 1 ? kClutPlaneB : 0x00;
    const uint8_t pixel_mask = run_length ? 0x7 : 0xf;
    for (int x = 0; x < kLineWidth; ++x) {
        if (x >= units) {
            for (int k = 2 * x; k < 2 * x + 2; ++k) {
                row[k] = kStudioBlack;
                present[k] = false;
            }
            continue;
        }
        row[2 * x] = clut_[((codes[x] >> 4) & pixel_mask) | bank];
        row[2 * x + 1] = clut_[(codes[x] & pixel_mask) | bank];
        present[2 * x] = true;
        present[2 * x + 1] = true;
    }
    set_vsr_address(path, vsr);
}

} // namespace chips
} // namespace zcdi
