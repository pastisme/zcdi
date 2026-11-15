#include "chips/mcd212/mcd212.h"

#include "core/util.h"

#include <algorithm>
#include <cstddef>

namespace zcdi {
namespace chips {

using namespace mcd212;

namespace {
// A plane pixel that shows nothing; real colors only use the low 24 bits.
constexpr uint32_t kNoPixel = 0xff000000u;
} // namespace

void Mcd212::invalidate_region_arrays() {
    // Rebuilt when next used, with the NR bit as it is now.
    region_arrays_nr_ = icm_nr_;
    region_arrays_dirty_ = true;
}

void Mcd212::update_region_arrays() {
    if (!region_arrays_dirty_) return;
    region_arrays_dirty_ = false;
    fill_region_arrays();
}

void Mcd212::fill_region_arrays() {
    auto region_weight = [&](int idx) -> uint8_t { return uint8_t((region_control_[idx] & kRcWf) >> kRcWfShift); };
    auto region_op = [&](int idx) -> uint32_t { return (region_control_[idx] & kRcOp) >> kRcOpShift; };
    auto apply_op = [&](uint32_t op, int flag, uint8_t& wfa, uint8_t& wfb, bool* rf, int region_idx) {
        switch (op) {
            case kRcOpWfA: wfa = region_weight(region_idx); break;
            case kRcOpWfB: wfb = region_weight(region_idx); break;
            case kRcOpResetRf: rf[flag] = false; break;
            case kRcOpSetRf: rf[flag] = true; break;
            case kRcOpWfAResetRf:
                wfa = region_weight(region_idx);
                rf[flag] = false;
                break;
            case kRcOpWfASetRf:
                wfa = region_weight(region_idx);
                rf[flag] = true;
                break;
            case kRcOpWfBResetRf:
                wfb = region_weight(region_idx);
                rf[flag] = false;
                break;
            case kRcOpWfBSetRf:
                wfb = region_weight(region_idx);
                rf[flag] = true;
                break;
            default: break; // 0/1/2/3/5/7/10/11: end-of-line or unused
        }
    };

    uint8_t wfa = weight_factor_[0];
    uint8_t wfb = weight_factor_[1];
    bool rf[2] = {false, false};
    // Only an entry at an odd column splits a Normal Resolution pixel.
    region_pairs_uniform_ = true;

    if (region_arrays_nr_) {
        for (int x = 0; x < kFbWidth; ++x) {
            for (int flag = 0; flag < 2; ++flag) {
                for (int region = 0; region < 4; ++region) {
                    int region_idx = (flag << 2) + region;
                    uint32_t op = region_op(region_idx);
                    if (op == kRcOpEnd) break;
                    // RC_X is in Double Resolution units (Green Book V.5.10.2).
                    if (uint32_t(x) == (region_control_[region_idx] & kRcX)) {
                        apply_op(op, flag, wfa, wfb, rf, region_idx);
                        if (x & 1) region_pairs_uniform_ = false;
                    }
                }
            }
            weight_factor_line_[0][x] = wfa;
            weight_factor_line_[1][x] = wfb;
            region_flag_line_[0][x] = rf[0];
            region_flag_line_[1][x] = rf[1];
        }
        return;
    }

    // NR clear: one ordered pass; an entry behind the previous one, or op 0,
    // ends the list.
    auto fill = [&](int from, int to) {
        std::fill(weight_factor_line_[0] + from, weight_factor_line_[0] + to, wfa);
        std::fill(weight_factor_line_[1] + from, weight_factor_line_[1] + to, wfb);
        std::fill(region_flag_line_[0] + from, region_flag_line_[0] + to, rf[0]);
        std::fill(region_flag_line_[1] + from, region_flag_line_[1] + to, rf[1]);
    };
    int filled = 0; // columns before this one are final
    int earliest = 0; // the next entry can apply from this column on
    for (int region_idx = 0; region_idx < 8; ++region_idx) {
        const uint32_t op = region_op(region_idx);
        const int rc_x = int(region_control_[region_idx] & kRcX);
        if (op == kRcOpEnd || rc_x < earliest || rc_x >= kFbWidth) break;
        fill(filled, rc_x);
        apply_op(op, int((region_control_[region_idx] >> kRcRfBit) & 1u), wfa, wfb, rf, region_idx);
        if (rc_x & 1) region_pairs_uniform_ = false;
        filled = rc_x;
        earliest = rc_x + 1;
    }
    fill(filled, kFbWidth);
}

// De Morgan covers the inverted RF || Key conditions (13/14).
Mcd212::TransparencyRule Mcd212::transparency_rule(int path) const {
    const uint32_t field = transparency_control_ >> (path == 0 ? 0 : kTcrPlaneBShift);
    TransparencyRule rule;
    switch (field & kTcrCondition) {
        case kTcrAlways: rule.always = true; break;
        case kTcrKey: rule.color_key = true; break;
        case kTcrRf0: rule.flag = 0; break;
        case kTcrRf1: rule.flag = 1; break;
        case kTcrRf0Key:
            rule.flag = 0;
            rule.color_key = true;
            break;
        case kTcrRf1Key:
            rule.flag = 1;
            rule.color_key = true;
            break;
        default: break; // kTcrRgb, 7 (unused): never matches
    }
    rule.invert = (field & kTcrInvert) != 0;
    // Color Key is for CLUT planes only, bits 2-7, Mask Color bits don't-care
    // (Green Book V.5.7.2.2).
    const bool clut_coded =
        icm_[path] == kIcmClut8 || icm_[path] == kIcmClut7 || icm_[path] == kIcmClut77 || icm_[path] == kIcmClut4;
    rule.color_key = rule.color_key && clut_coded;
    rule.key = transparent_color_[path];
    rule.compared_bits = ~mask_color_[path] & kColorBits;
    // Inverted Color Key on DYUV is undefined; keeping the plane visible fixes Jeopardy.
    if ((field & kTcrField) == kTcrNotKey && icm_[path] == kIcmDyuv) rule.invert = false;
    return rule;
}

inline bool Mcd212::is_transparent(const TransparencyRule& rule, uint32_t color, int x) const {
    const bool matches = rule.always || (rule.flag >= 0 && region_flag_line_[rule.flag][x]) ||
                         (rule.color_key && ((color ^ rule.key) & rule.compared_bits) == 0);
    return matches != rule.invert;
}

bool Mcd212::plane_transparent(int path, uint32_t color, int x) const {
    return is_transparent(transparency_rule(path), color, x);
}

bool Mcd212::rgb555_transparent(int x, bool transparency_bit) const {
    // RGB555 is plane B only (V.4.6.1): TB, with the T bit instead of Color Key.
    uint8_t field = uint8_t(transparency_control_ >> kTcrPlaneBShift) & kTcrField;
    bool matches;
    switch (field & kTcrCondition) {
        case kTcrAlways: matches = true; break;
        case kTcrKey: matches = false; break; // unavailable for RGB555
        case kTcrRgb: matches = transparency_bit; break;
        case kTcrRf0: matches = region_flag_line_[0][x]; break;
        case kTcrRf1: matches = region_flag_line_[1][x]; break;
        case kTcrRf0Key: matches = region_flag_line_[0][x]; break;
        case kTcrRf1Key: matches = region_flag_line_[1][x]; break;
        default: matches = false; break; // reserved
    }
    return matches != ((field & kTcrInvert) != 0);
}

// The weights are this column's, from Region Control.
uint32_t Mcd212::composite_pixel(uint32_t color_a, bool opaque_a, uint32_t color_b, bool opaque_b, uint8_t weight_a,
                                 uint8_t weight_b) const {
    // ICF * (C' - 16), ICF = ICF'/63, rounded and signed: only the final sum
    // is clipped, no per-plane clamp (Green Book V.5.9/V.5.9.1).
    auto weighted_component = [](int c, uint8_t w) -> int {
        const int d = (c - 16) * int(w);
        return d >= 0 ? (d + 31) / 63 : -((31 - d) / 63);
    };

    bool mix_enabled = (transparency_control_ & kTcrDisableMx) == 0;
    if (mix_enabled && (opaque_a || opaque_b)) {
        // C(A)+C(B)-16: one +16 pedestal for the sum (V.5.9.1); a
        // transparent plane counts as black, whatever it decoded to.
        auto blend = [&](uint32_t a, uint32_t b, int shift) -> uint8_t {
            int ca = int((a >> shift) & 0xff);
            int cb = int((b >> shift) & 0xff);
            int sum = (opaque_a ? weighted_component(ca, weight_a) : 0) +
                      (opaque_b ? weighted_component(cb, weight_b) : 0) + 16;
            return clamp_u8(sum);
        };
        uint8_t r = blend(color_a, color_b, 16);
        uint8_t g = blend(color_a, color_b, 8);
        uint8_t b = blend(color_a, color_b, 0);
        return (r << 16) | (g << 8) | b;
    }

    // Mix disabled: front plane, else back plane, each with its own weight,
    // else backdrop. Mixing with neither plane present shows the backdrop too.
    bool b_in_front = (plane_order_ & kPorBa) != 0;
    uint32_t front_color = b_in_front ? color_b : color_a;
    bool front_opaque = b_in_front ? opaque_b : opaque_a;
    uint32_t back_color = b_in_front ? color_a : color_b;
    bool back_opaque = b_in_front ? opaque_a : opaque_b;
    uint8_t front_weight = b_in_front ? weight_b : weight_a;
    uint8_t back_weight = b_in_front ? weight_a : weight_b;

    auto solo = [&](uint32_t color, uint8_t w) -> uint32_t {
        if (w == 63) return color & 0x00ffffffu; // ICF 63 is a factor of 1: the color as it is
        auto ch = [&](int shift) -> uint8_t {
            return clamp_u8(weighted_component(int((color >> shift) & 0xff), w) + 16);
        };
        return (uint32_t(ch(16)) << 16) | (uint32_t(ch(8)) << 8) | uint32_t(ch(0));
    };

    if (front_opaque) return solo(front_color, front_weight);
    if (back_opaque) return solo(back_color, back_weight);
    return k4bppColor[backdrop_color_ & 0xf];
}

void Mcd212::render_line(int line, const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size) {
    update_region_arrays();
    uint32_t* row = fb_.data() + static_cast<std::size_t>(line) * kFbWidth;
    uint8_t* mask = backdrop_mask_.data() + static_cast<std::size_t>(line) * kFbWidth;
    uint32_t row_a[kFbWidth], row_b[kFbWidth];

    // RGB555: plane A Off, CM1 = 1, composited as plane B (V.4.4.8/V.4.6.1).
    if (icm_[0] == kIcmOff && icm_[1] == kIcmRgb555) {
        uint32_t lores[kLineWidth];
        bool t_bit[kLineWidth];
        render_line_rgb555(lores, t_bit, plane_a, plane_b, plane_size);
        apply_pixel_hold(1, lores, t_bit, kLineWidth);
        for (int x = 0; x < kFbWidth; ++x) {
            row_a[x] = kNoPixel;
            row_b[x] = rgb555_transparent(x, t_bit[x / 2]) ? kNoPixel : lores[x / 2];
        }
        composite_row(row_a, row_b, kFbWidth, 1, row, mask);
        return;
    }

    // No Double Resolution plane and uniform region pairs: both columns of
    // each pixel come out identical, so do one and copy it.
    const int step = (icm_[0] != kIcmClut4 && icm_[1] != kIcmClut4 && region_pairs_uniform_) ? 2 : 1;
    decode_plane_row(0, plane_a, plane_size, row_a, step);
    decode_plane_row(1, plane_b, plane_size, row_b, step);
    composite_row(row_a, row_b, kFbWidth / step, step, row, mask);
}

void Mcd212::composite_row(const uint32_t* row_a, const uint32_t* row_b, int width, int step, uint32_t* row,
                           uint8_t* mask) const {
    // Reuse the last result when a pixel's inputs match its neighbour's.
    const uint8_t* weight_a = weight_factor_line_[0];
    const uint8_t* weight_b = weight_factor_line_[1];
    // The frame player shows on every vsd pixel, studio black outside its window.
    const bool fmv_backdrop = dvc_attached_ && icm_ev_;
    uint32_t color = 0;
    for (int i = 0; i < width; ++i) {
        const int x = i * step;
        const uint32_t a = row_a[i], b = row_b[i];
        if (i == 0 || a != row_a[i - 1] || b != row_b[i - 1] || weight_a[x] != weight_a[x - step] ||
            weight_b[x] != weight_b[x - step]) {
            color = composite_pixel(a, a != kNoPixel, b, b != kNoPixel, weight_a[x], weight_b[x]);
        }
        const uint8_t backdrop = a == kNoPixel && b == kNoPixel;
        const uint32_t out = (backdrop && fmv_backdrop) ? kStudioBlack : color;
        row[x] = out;
        mask[x] = backdrop;
        if (step == 2) {
            row[x + 1] = out;
            mask[x + 1] = backdrop;
        }
    }
}

void Mcd212::decode_plane_row(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row, int step) {
    bool opaque[kFbWidth];
    // Transparency per Double Resolution column after Pixel Hold, as
    // RC_X can split a Normal Resolution pixel.
    int width = kFbWidth;
    if (icm_[path] == kIcmClut4) {
        decode_double_res_line(path, plane_ram, plane_size, row, opaque);
        apply_pixel_hold(path, row, opaque, kFbWidth);
    } else if (step == 2) {
        width = kLineWidth;
        decode_plane_line(path, plane_ram, plane_size, row, opaque);
        apply_pixel_hold(path, row, opaque, kLineWidth);
    } else {
        uint32_t lores[kLineWidth];
        bool lores_present[kLineWidth];
        decode_plane_line(path, plane_ram, plane_size, lores, lores_present);
        apply_pixel_hold(path, lores, lores_present, kLineWidth);
        for (int x = 0; x < kFbWidth; ++x) {
            row[x] = lores[x / 2];
            opaque[x] = lores_present[x / 2];
        }
    }
    TransparencyRule rule = transparency_rule(path);
    if (plane_parked(path)) rule.color_key = false; // no pixels to compare
    for (int i = 0; i < width; ++i) {
        if (!opaque[i] || is_transparent(rule, row[i], i * step)) row[i] = kNoPixel;
    }
}

// One frame of blink timing; CON or COF 0 disables blinking.
void Mcd212::update_cursor_blink() {
    int cof = int((cursor_control_ & kCurcntCofMask) >> kCurcntCofShift);
    int con = int((cursor_control_ & kCurcntConMask) >> kCurcntConShift);
    if (cof == 0 || con == 0) {
        cursor_blink_framecnt_ = 0;
        cursor_blink_periodcnt_ = 0;
        cursor_blink_state_ = false;
        return;
    }
    if (cursor_blink_framecnt_ == kCursorBlinkSubPeriodFrames - 1) {
        cursor_blink_framecnt_ = 0;
        if (cursor_blink_state_ && cursor_blink_periodcnt_ == cof - 1) {
            cursor_blink_state_ = false;
            cursor_blink_periodcnt_ = 0;
        } else if (!cursor_blink_state_ && cursor_blink_periodcnt_ == con - 1) {
            cursor_blink_state_ = true;
            cursor_blink_periodcnt_ = 0;
        } else {
            ++cursor_blink_periodcnt_;
        }
    } else {
        ++cursor_blink_framecnt_;
    }
}

// X is a Double Resolution column, Y a line, both from the active area's origin.
void Mcd212::draw_cursor() {
    if (!(cursor_control_ & kCurcntEn) || !(dcr_[0] & kDcrDe)) return;
    // Off phase: BLKC 0 hides the cursor, BLKC 1 complements its colour.
    if (cursor_blink_state_ && !(cursor_control_ & kCurcntBlkc)) return;
    int cursor_x = int(cursor_position_ & 0x3ffu);
    int cursor_y = int((cursor_position_ >> 12) & 0x3ffu);
    uint32_t color_index = cursor_control_ & kCurcntColor;
    if (cursor_blink_state_) color_index ^= 0x7u; // complementary: flip R/G/B, keep Y
    uint32_t color = k4bppColor[color_index];
    int px_per_bit = (cursor_control_ & kCurcntCuw) ? 1 : 2;
    int width = 16 * px_per_bit;

    for (int row = 0; row < 16; ++row) {
        int y = cursor_y + row;
        if (y < 0 || y >= kFbHeight) continue;
        uint32_t* line = fb_.data() + static_cast<std::size_t>(y) * kFbWidth;
        uint16_t pattern = cursor_pattern_[row];
        for (int i = 0; i < width; ++i) {
            int x = cursor_x + i;
            if (x < 0 || x >= kFbWidth) continue;
            if (pattern & (1u << (15 - (i / px_per_bit)))) line[x] = color;
        }
        output_row(y);
    }
}

void Mcd212::overlay_dvc_frame() {
    if (!dvc_attached_) return;
    if (!dvc_frame_ || dvc_frame_width_ <= 0 || dvc_frame_height_ <= 0) return;
    if (dvc_decwin_w_ == 0 || dvc_decwin_h_ == 0 || !dvc_fmv_show_) return;

    // Placement and the frame are in Normal Resolution pixels.
    const int height = std::min({int(dvc_decwin_h_), kFbHeight - dvc_scrpos_y_, dvc_frame_height_ - dvc_decoff_y_});
    const int width = std::min(int(dvc_decwin_w_), dvc_frame_width_ - dvc_decoff_x_);
    // VCD clock: a decoded pixel per 512/231 columns, else two.
    const bool vcd = dvc_vcd_pixel_clock_;
    const int columns = vcd ? std::min((width * 512 + 230) / 231, 2 * (kLineWidth - dvc_scrpos_x_))
                            : 2 * std::min(width, kLineWidth - dvc_scrpos_x_);
    for (int y = 0; y < height; ++y) {
        const std::size_t row = std::size_t(dvc_scrpos_y_ + y) * kFbWidth + 2 * std::size_t(dvc_scrpos_x_);
        const std::size_t src_y = std::size_t(dvc_decoff_y_) + y;
        const uint8_t* src = dvc_frame_ + (src_y * dvc_frame_width_ + dvc_decoff_x_) * 4;
        // Locals: stores through `out` would otherwise reload members per pixel.
        uint32_t* out = fb_.data() + row;
        const uint8_t* vsd = backdrop_mask_.data() + row;
        auto pixel = [src](std::size_t i) {
            const uint8_t* p = src + i * 4;
            return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
        };
        if (vcd) {
            const uint32_t lead = dvc_scrpos_x_ == 0 ? 0 : pixel(0);
            auto sample = [&](int t) { return t < 0 ? lead : pixel(std::size_t(t) * 231 / 1024); };
            for (int c = 0; c < columns; ++c) {
                if (!vsd[c]) continue;
                uint32_t r = 0, g = 0, b = 0;
                for (int t = 2 * c - 3; t <= 2 * c; ++t) {
                    const uint32_t p = sample(t);
                    r += (p >> 16) & 0xff;
                    g += (p >> 8) & 0xff;
                    b += p & 0xff;
                }
                out[c] = ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
            }
            continue;
        }
        for (int c = 0; c < columns; c += 2) {
            const uint32_t color = pixel(std::size_t(c / 2));
            if (vsd[c]) out[c] = color;
            if (vsd[c + 1]) out[c + 1] = color;
        }
    }
    for (int y = 0; y < height; ++y)
        output_row(dvc_scrpos_y_ + y);
}

} // namespace chips
} // namespace zcdi
