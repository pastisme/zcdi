#include "mcd212/mcd212.h"

#include "core/state_archive.h"
#include "core/util.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

namespace zcdi {
namespace mamecdi {

namespace {
// Register byte offsets relative to 0x4fffe0.
constexpr uint32_t kCsr2W = 0x00, kCsr2R = 0x01, kDcr2 = 0x02, kVsr2 = 0x04, kDdr2 = 0x08, kDca2 = 0x0a;
constexpr uint32_t kCsr1W = 0x10, kCsr1R = 0x11, kDcr1 = 0x12, kVsr1 = 0x14, kDdr1 = 0x18, kDca1 = 0x1a;

constexpr uint8_t kCsr1rDa = 0x80; // display active
constexpr uint8_t kCsr1rPa = 0x20; // parity (odd/even field)
constexpr uint8_t kCsr2rIt2 = 0x02;
constexpr uint8_t kCsr2rIt1 = 0x04;

constexpr uint8_t kIcmClut4 = 0x0b; // CLUT4, or RL3 with run-length decode
constexpr uint8_t kIcmDyuv = 0x05;

// What a plane shows where it has no pixels.
constexpr uint32_t kStudioBlack = 0x00101010u;

// 1920 (30 MHz) or 1792 (28 MHz) clocks per line, 4 per pixel (datasheet
// Table 5-4); a 4:3 screen is 52.4 us by 287.5/242.5 lines (Green Book V.2.4.1).
double tv_display_aspect(int pixels, int lines, bool crystal_30mhz, bool hz60) {
    const double line_us = hz60 ? 63.556 : 64.0;
    const double active_us = pixels * 4 / (crystal_30mhz ? 1920.0 : 1792.0) * line_us;
    const double screen_lines = hz60 ? 242.5 : 287.5;
    return 4.0 / 3.0 * (active_us / 52.4) * (screen_lines / lines);
}

// A plane pixel that shows nothing; real colors only use the low 24 bits.
constexpr uint32_t kNoPixel = 0xff000000u;

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
// Active area (datasheet tables 5-4/5-6, video_timing.sv): 240 lines with
// FD or ST, else 280; 384 pixels with CF and not ST, else 360.
constexpr uint16_t kCsr1wSt = 0x0002;
constexpr uint16_t kDcr1Sm = 0x1000; // scan mode: interlace
constexpr uint16_t kDcr1Fd = 0x2000;
constexpr uint16_t kDcr1Cf = 0x4000;

// DCR IC (ICA enable) and DC (DCA enable), mcd212.sv:499-513.
constexpr uint16_t kDcrIc = 0x0200;
constexpr uint16_t kDcrDc = 0x0100;

// CSR1W/CSR2W DI: masks the IRQ1 pin only; IT1/IT2 still latch (mcd212.sv:203-204).
constexpr uint16_t kCsrWDi = 0x8000;

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// A 22-bit address held as 16 low bits plus the low 6 bits of another
// register (VSR + DCR, DCP + DDR).
uint32_t join_address(uint16_t high_reg, uint16_t low_reg) { return (uint32_t(high_reg & 0x3f) << 16) | low_reg; }

void split_address(uint16_t& high_reg, uint16_t& low_reg, uint32_t address) {
    low_reg = uint16_t(address & 0xffffu);
    high_reg = uint16_t((high_reg & 0xffc0) | ((address >> 16) & 0x3f));
}

// Pixel Repeat factor from DDR bits 10-11.
int pixel_repeat(uint16_t ddr) { return 2 << ((ddr >> 10) & 0x3); }

// ICA's plain RELOAD VSR only moves the walker. The low 19 bits are a word
// index, so the byte offset is the masked value with bit 0 cleared.
uint32_t reload_vsr_jump_target(uint32_t cmd) { return (cmd & 0x0007ffffu) & ~1u; }

// RELOAD DISPLAY PARAMETERS (same_cdi set_display_parameters): bits 0-3 to
// DDR bits 8-11 (file type), bit 4 to DCR bit 11.
void apply_display_parameters(uint16_t& ddr_reg, uint16_t& dcr_reg, uint8_t value) {
    ddr_reg = uint16_t((ddr_reg & 0xf0ffu) | (uint16_t(value & 0x0fu) << 8));
    dcr_reg = uint16_t((dcr_reg & 0xf7ffu) | (uint16_t(value & 0x10u) << 7));
}
} // namespace

Mcd212::Mcd212()
    : fb_(static_cast<std::size_t>(kFbWidth) * kFbHeight, 0),
      out_(static_cast<std::size_t>(kFbWidth) * kOutHeight, 0),
      backdrop_mask_(static_cast<std::size_t>(kFbWidth) * kFbHeight, 0) {}

template <class T>
void Mcd212::state_io(T& archive) {
    archive.begin_section(state_tag("MCD "));
    archive.io_struct(static_cast<Mcd212SaveState&>(*this));
    // While woven, the next field fills only its own rows; the others hold the last field.
    archive.io(woven_last_);
    if (woven_last_) {
        const int held = even_field_ ? 0 : 1;
        for (int y = 0; y < kFbHeight; ++y) {
            archive.io_span(
                std::span<uint32_t>(out_.data() + static_cast<std::size_t>(2 * y + held) * kFbWidth, kFbWidth));
        }
    }
    archive.end_section();
    if constexpr (T::kLoading) archive.check(loaded());
}
ZCDI_STATE_IO_INSTANTIATE(Mcd212);

// fb_ and backdrop_mask_ aren't saved: every field rewrites every line.
bool Mcd212::loaded() {
    std::fill(fb_.begin(), fb_.end(), 0u);
    if (!woven_last_) std::fill(out_.begin(), out_.end(), 0u);
    std::fill(backdrop_mask_.begin(), backdrop_mask_.end(), uint8_t{0});
    dvc_frame_ = nullptr;
    region_arrays_dirty_ = true; // rebuilt from the loaded Region Control
    return clut_bank_[0] <= 3 && clut_bank_[1] <= 3;
}

uint8_t Mcd212::read_byte(uint32_t offset) {
    switch (offset) {
        case kCsr2R: {
            uint8_t v = csr2r_;
            csr2r_ &= ~(kCsr2rIt1 | kCsr2rIt2); // read acknowledges the interrupt
            irq1_ = false;
            return v;
        }
        case kCsr1R: return csr1r_;
        case kDcr2: return uint8_t(dcr_[1] >> 8);
        case kDcr2 + 1: return uint8_t(dcr_[1]);
        case kVsr2: return uint8_t(vsr_[1] >> 8);
        case kVsr2 + 1: return uint8_t(vsr_[1]);
        case kDdr2: return uint8_t(ddr_[1] >> 8);
        case kDdr2 + 1: return uint8_t(ddr_[1]);
        case kDca2: return uint8_t(dca_[1] >> 8);
        case kDca2 + 1: return uint8_t(dca_[1]);
        case kDcr1: return uint8_t(dcr_[0] >> 8);
        case kDcr1 + 1: return uint8_t(dcr_[0]);
        case kVsr1: return uint8_t(vsr_[0] >> 8);
        case kVsr1 + 1: return uint8_t(vsr_[0]);
        case kDdr1: return uint8_t(ddr_[0] >> 8);
        case kDdr1 + 1: return uint8_t(ddr_[0]);
        case kDca1: return uint8_t(dca_[0] >> 8);
        case kDca1 + 1: return uint8_t(dca_[0]);
        default: return 0; // unmapped in this window
    }
}

void Mcd212::write_byte(uint32_t offset, uint8_t v) {
    switch (offset) {
        case kCsr2W: csrw_[1] = set_hi(csrw_[1], v); return;
        case kCsr2W + 1: csrw_[1] = set_lo(csrw_[1], v); return;
        case kDcr2: dcr_[1] = set_hi(dcr_[1], v); return;
        case kDcr2 + 1: dcr_[1] = set_lo(dcr_[1], v); return;
        case kVsr2: vsr_[1] = set_hi(vsr_[1], v); return;
        case kVsr2 + 1: vsr_[1] = set_lo(vsr_[1], v); return;
        case kDdr2: ddr_[1] = set_hi(ddr_[1], v); return;
        case kDdr2 + 1: ddr_[1] = set_lo(ddr_[1], v); return;
        // MMIO reaches only DCA's low 16 bits; RELOAD DCP sets the rest.
        case kDca2: dca_[1] = set_byte(dca_[1], 8, v); return;
        case kDca2 + 1: dca_[1] = set_byte(dca_[1], 0, v); return;
        case kCsr1W: csrw_[0] = set_hi(csrw_[0], v); return;
        case kCsr1W + 1: csrw_[0] = set_lo(csrw_[0], v); return;
        case kDcr1: dcr_[0] = set_hi(dcr_[0], v); return;
        case kDcr1 + 1: dcr_[0] = set_lo(dcr_[0], v); return;
        case kVsr1: vsr_[0] = set_hi(vsr_[0], v); return;
        case kVsr1 + 1: vsr_[0] = set_lo(vsr_[0], v); return;
        case kDdr1: ddr_[0] = set_hi(ddr_[0], v); return;
        case kDdr1 + 1: ddr_[0] = set_lo(ddr_[0], v); return;
        case kDca1: dca_[0] = set_byte(dca_[0], 8, v); return;
        case kDca1 + 1: dca_[0] = set_byte(dca_[0], 0, v); return;
        default: return; // unmapped in this window
    }
}

void Mcd212::set_register(int path, uint8_t reg, uint32_t value) {
    if (reg >= 0x80 && reg <= 0xbf) {
        // Into the 64-entry quarter this path's 0xc3 bank selects.
        std::size_t clut_index = std::size_t(clut_bank_[path & 1]) * 0x40 + (reg - 0x80);
        clut_[clut_index] = value & 0x00fcfcfcu;
        return;
    }
    if (reg >= 0xd0 && reg <= 0xd7) {
        // Either path may write Region Control (same_cdi).
        region_control_[reg - 0xd0] = value;
        invalidate_region_arrays();
        return;
    }
    switch (reg) {
        case 0xc0:
            if (path != 0) return;
            // Image Coding Method: both planes' modes plus NR/EV/CS.
            icm_[0] = uint8_t(value & 0x0f);
            icm_[1] = uint8_t((value >> 8) & 0x0f);
            icm_nr_ = (value & 0x080000u) != 0;
            icm_ev_ = (value & 0x040000u) != 0;
            icm_cs_ = (value & 0x400000u) != 0;
            return;
        case 0xc1:
            if (path == 0) transparency_control_ = value;
            return;
        case 0xc2:
            if (path == 0) plane_order_ = uint8_t(value & 0x07);
            return;
        case 0xc3:
            // Path 1 can only reach banks 2-3 (hardware, as same_cdi).
            clut_bank_[path & 1] = (path == 0) ? uint8_t(value & 0x03) : uint8_t(0x02 | (value & 0x01));
            return;
        case 0xc4:
            if (path == 0) transparent_color_[0] = value & 0x00fcfcfcu;
            return;
        case 0xc6:
            if (path == 1) transparent_color_[1] = value & 0x00fcfcfcu;
            return;
        case 0xc7:
            if (path == 0) mask_color_[0] = value & 0x00fcfcfcu;
            return;
        case 0xc9:
            if (path == 1) mask_color_[1] = value & 0x00fcfcfcu;
            return;
        case 0xca:
            if (path == 0) dyuv_abs_start_[0] = value;
            return;
        case 0xcb:
            if (path == 1) dyuv_abs_start_[1] = value;
            return;
        case 0xcd:
            if (path == 0) cursor_position_ = value;
            return;
        case 0xce:
            if (path == 0) cursor_control_ = value;
            return;
        case 0xcf:
            // Row index in bits 16-19.
            if (path == 0) cursor_pattern_[(value >> 16) & 0xf] = uint16_t(value & 0xffffu);
            return;
        case 0xd8:
            if (path == 0) backdrop_color_ = uint8_t(value & 0x0fu);
            return;
        case 0xd9:
            if (path == 0) pixel_hold_[0] = value;
            return;
        case 0xda:
            if (path == 1) pixel_hold_[1] = value;
            return;
        case 0xdb:
            if (path != 0) return;
            weight_factor_[0] = uint8_t(value & 0x3f); // ICF'(5-0), Green Book V.5.9
            invalidate_region_arrays();
            return;
        case 0xdc:
            if (path != 1) return;
            weight_factor_[1] = uint8_t(value & 0x3f);
            invalidate_region_arrays();
            return;
        default: return;
    }
}

uint32_t Mcd212::vsr_address(int path) const { return join_address(dcr_[path], vsr_[path]); }

void Mcd212::set_vsr_address(int path, uint32_t address) { split_address(dcr_[path], vsr_[path], address); }

// Region Control fields (same_cdi RC_X/RC_WF/RC_RF_BIT/RC_OP).
namespace {
constexpr uint32_t kRcX = 0x0003ffu;
constexpr uint32_t kRcWf = 0x00fc00u;
constexpr int kRcWfShift = 10;
constexpr int kRcRfBit = 16;
constexpr uint32_t kRcOp = 0xf00000u;
constexpr int kRcOpShift = 20;
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
            case 4: wfa = region_weight(region_idx); break;
            case 6: wfb = region_weight(region_idx); break;
            case 8: rf[flag] = false; break;
            case 9: rf[flag] = true; break;
            case 12:
                wfa = region_weight(region_idx);
                rf[flag] = false;
                break;
            case 13:
                wfa = region_weight(region_idx);
                rf[flag] = true;
                break;
            case 14:
                wfb = region_weight(region_idx);
                rf[flag] = false;
                break;
            case 15:
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
                    if (op == 0) break;
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
        if (op == 0 || rc_x < earliest || rc_x >= kFbWidth) break;
        fill(filled, rc_x);
        apply_op(op, int((region_control_[region_idx] >> kRcRfBit) & 1u), wfa, wfb, rf, region_idx);
        if (rc_x & 1) region_pairs_uniform_ = false;
        filled = rc_x;
        earliest = rc_x + 1;
    }
    fill(filled, kFbWidth);
}

void Mcd212::process_ica(int path, const uint8_t* plane_ram, std::size_t plane_size) {
    // ICA starts at byte 0x400, or 0x404 in even interlaced fields (ica_dca_ctrl.sv, mcd212.sv ica_parity).
    uint32_t byte_off = (dcr_[0] & kDcr1Sm) && even_field_ ? 0x404u : 0x400u;
    constexpr int kMaxCommands = 1920; // 3840 16-bit words / 2 words-per-command

    for (int i = 0; i < kMaxCommands; ++i) {
        if (byte_off + 4 > plane_size) break;
        uint32_t cmd = read_be32(plane_ram + byte_off);
        uint8_t op = uint8_t(cmd >> 24);
        uint32_t arg = cmd & 0x00ffffffu;

        if (op <= 0x0f) break; // STOP
        if (op <= 0x1f) {
            byte_off += 4;
            continue;
        } // NOP
        if (op <= 0x2f) {
            split_address(ddr_[path], dcp_[path], arg & 0x3ffffcu);
            byte_off += 4;
            continue;
        }
        if (op <= 0x3f) {
            split_address(ddr_[path], dcp_[path], arg & 0x3ffffcu);
            break;
        } // RELOAD DCP + STOP
        if (op <= 0x4f) {
            byte_off = reload_vsr_jump_target(cmd);
            continue;
        } // RELOAD VSR: jump only, no register touched
        if (op <= 0x5f) {
            set_vsr_address(path, arg);
            break;
        } // RELOAD VSR + STOP: sets the real register
        if (op <= 0x6f) {
            // INTERRUPT: both paths' flags live in CSR2R (as MAME).
            csr2r_ |= (path == 0) ? kCsr2rIt1 : kCsr2rIt2;
            if (!(csrw_[path] & kCsrWDi)) irq1_ = true;
            byte_off += 4;
            continue;
        }
        if (op >= 0x78 && op <= 0x7f) { // RELOAD DISPLAY PARAMETERS
            apply_display_parameters(ddr_[path], dcr_[path], uint8_t(arg & 0x1fu));
            byte_off += 4;
            continue;
        }
        if (op >= 0x80) {
            set_register(path, op, arg);
            byte_off += 4;
            continue;
        }
        byte_off += 4; // 0x70-0x77: unused/undefined
    }
}

void Mcd212::process_dca(int path, const uint8_t* plane_ram, std::size_t plane_size) {
    // DCP can hold 22 bits; same_cdi masks into the installed VRAM.
    uint32_t byte_off = dca_[path] & (uint32_t(plane_size) - 1);
    constexpr int kBudgetBytes = 64; // real hardware budgets exactly 64 bytes/line
    int consumed = 0;
    bool processing = true;

    while (processing && consumed < kBudgetBytes) {
        if (byte_off + 4 > plane_size) break;
        uint32_t cmd = read_be32(plane_ram + byte_off);
        uint8_t op = uint8_t(cmd >> 24);
        uint32_t arg = cmd & 0x00ffffffu;
        byte_off += 4;
        consumed += 4; // STOP counts too (same_cdi)

        if (op <= 0x0f) {
            processing = false;
            continue;
        } // STOP
        if (op <= 0x1f) continue; // NOP
        if (op <= 0x2f) continue; // RELOAD DCP: meaningless mid-DCA, ignored
        if (op <= 0x3f) {
            // Jumps DCA at once, skipping the budget advance; narrower mask
            // than DCP's (same_cdi `m_dca = cmd & 0x7fffc; return;`).
            split_address(ddr_[path], dcp_[path], arg & 0x3ffffcu);
            dca_[path] = arg & 0x0007fffcu;
            return;
        }
        // In DCA both RELOAD VSR forms set VSR (same_cdi process_dca).
        if (op <= 0x4f) {
            set_vsr_address(path, arg);
            continue;
        } // RELOAD VSR
        if (op <= 0x5f) {
            set_vsr_address(path, arg);
            processing = false;
            continue;
        } // RELOAD VSR + STOP
        if (op <= 0x6f) {
            csr2r_ |= (path == 0) ? kCsr2rIt1 : kCsr2rIt2;
            if (!(csrw_[path] & kCsrWDi)) irq1_ = true;
            continue;
        }
        if (op >= 0x80) {
            set_register(path, op, arg);
            continue;
        }
        if (op >= 0x78) {
            apply_display_parameters(ddr_[path], dcr_[path], uint8_t(arg & 0x1fu));
            continue;
        } // RELOAD DISPLAY PARAMETERS
        // 0x70-0x77: unused/undefined, no-op
    }

    // The whole 64-byte budget is consumed wherever STOP fell (same_cdi).
    byte_off += (kBudgetBytes - consumed);
    dca_[path] = byte_off;
}

namespace {
// same_cdi s_dyuv_deltas.
constexpr uint8_t kDyuvDeltas[16] = {0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255};

// Fixed palette for Backdrop Color and CURCNT_COLOR (same_cdi s_4bpp_color).
constexpr uint32_t k4bppColor[16] = {
    0x101010, 0x10107a, 0x107a10, 0x107a7a, 0x7a1010, 0x7a107a, 0x7a7a10, 0x7a7a7a,
    0x101010, 0x1010e6, 0x10e610, 0x10e6e6, 0xe61010, 0xe610e6, 0xe6e610, 0xe6e6e6,
};

// CURCNT layout per mcd212.sv's cursor_control_register (same_cdi's
// CURCNT_CON 0x280000 is not contiguous).
constexpr uint32_t kCurcntBlkc = 0x400000u; // blink type (0=on/off, 1=normal/complementary)
constexpr uint32_t kCurcntConMask = 0x380000u; // "on" period, in units of 12 frames
constexpr int kCurcntConShift = 19;
constexpr uint32_t kCurcntCofMask = 0x070000u; // "off"/complementary period, in units of 12 frames
constexpr int kCurcntCofShift = 16;
constexpr int kCursorBlinkSubPeriodFrames = 12; // mcd212.sv CURSOR_BLINK_PERIOD

// same_cdi's m_dyuv_*_to_* tables: each term truncated on its own.
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

// Entry (byte & mask) | bank (same_cdi BYTE_TO_CLUT<Channel>).
Mcd212::ClutIndex Mcd212::clut_index(int path, uint8_t icm) const {
    // CM1=1 is RGB555 (Green Book V.4.6.1), handled before this.
    if (icm == 0x1) return path == 0 ? ClutIndex{.valid = true, .mask = 0xff, .bank = 0x00} : ClutIndex{};
    // CLUT7: plane B's palette is in banks 2-3, where its bank register points.
    if (icm == 0x3) return {.valid = true, .mask = 0x7f, .bank = uint8_t(path == 1 ? 0x80 : 0x00)};
    // Plane A's dual CLUT7, CS picks the half; plane B's CM1=4 is reserved.
    if (icm == 0x4) {
        return path == 0 ? ClutIndex{.valid = true, .mask = 0x7f, .bank = uint8_t(icm_cs_ ? 0x80 : 0x00)}
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

// same_cdi TCR_COND_*; codes 8-15 invert 0-7 (De Morgan covers 13/14).
Mcd212::TransparencyRule Mcd212::transparency_rule(int path) const {
    const uint32_t field = transparency_control_ >> (path == 0 ? 0 : 8);
    TransparencyRule rule;
    switch (field & 0x07) {
        case 0x0: rule.always = true; break; // Always
        case 0x1: rule.color_key = true; break; // Color Key
        case 0x3: rule.flag = 0; break; // RF0
        case 0x4: rule.flag = 1; break; // RF1
        case 0x5:
            rule.flag = 0;
            rule.color_key = true;
            break; // RF0 || Key
        case 0x6:
            rule.flag = 1;
            rule.color_key = true;
            break; // RF1 || Key
        default: break; // 2 (RGB555 T bit), 7 (unused): never matches
    }
    rule.invert = (field & 0x08) != 0;
    // Color Key is for CLUT planes only, bits 2-7, Mask Color bits don't-care
    // (Green Book V.5.7.2.2; mcd212.sv gates it on !plane_dyuv_active).
    const bool clut_coded = icm_[path] == 0x1 || icm_[path] == 0x3 || icm_[path] == 0x4 || icm_[path] == kIcmClut4;
    rule.color_key = rule.color_key && clut_coded;
    rule.key = transparent_color_[path];
    rule.compared_bits = ~mask_color_[path] & 0x00fcfcfcu;
    // Inverted Color Key on DYUV is undefined; mcd212.sv's "Jeopardy hack" keeps the plane visible.
    if ((field & 0x0f) == 0x9 && icm_[path] == kIcmDyuv) rule.invert = false;
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
    uint8_t field = uint8_t(transparency_control_ >> 8) & 0x0f;
    bool matches;
    switch (field & 0x07) {
        case 0x0: matches = true; break; // Always
        case 0x1: matches = false; break; // Color Key: unavailable for RGB555
        case 0x2: matches = transparency_bit; break; // T bit
        case 0x3: matches = region_flag_line_[0][x]; break; // RF0
        case 0x4: matches = region_flag_line_[1][x]; break; // RF1
        case 0x5: matches = region_flag_line_[0][x]; break; // RF0 || Color Key
        case 0x6: matches = region_flag_line_[1][x]; break; // RF1 || Color Key
        default: matches = false; break; // reserved
    }
    return matches != ((field & 0x08) != 0);
}

void Mcd212::apply_pixel_hold(int path, uint32_t* row, bool* opaque, int width) const {
    const uint32_t control = pixel_hold_[path & 1];
    if ((control & 0x800000u) == 0) return;
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
    // RL7 (same_cdi process_vsr DDR_FT_RLE): bit 7 set = run, next byte its
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
    const uint8_t rms_b = uint8_t((ddr_[1] >> 8) & 0x3);
    const int repeat_factor = rms_b == 0x3 ? pixel_repeat(ddr_[1]) : 1;
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
    return !(dcr_[path] & kDcrIc) || icm_[path] == 0 || vsr_address(path) == 0;
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

    // DDR file type (bits 8-9): 2 = RLE, 3 = Pixel Repeat, else bitmap.
    const uint8_t rms = uint8_t((ddr >> 8) & 0x3);
    if (rms == 0x2) {
        render_line_rle(path, row, present, plane_ram, mask);
        return;
    }

    const uint32_t byte_off = vsr_address(path);
    uint8_t line_bytes[kLineWidth];
    // Pixel Repeat repeats codes, not decoded RGB, and not for DYUV (V.5.11.1.2).
    const bool repeat = rms == 0x3 && icm != kIcmDyuv && icm != kIcmClut4;
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

// same_cdi mix_lines. The weights are this column's, from Region Control.
uint32_t Mcd212::composite_pixel(uint32_t color_a, bool opaque_a, uint32_t color_b, bool opaque_b, uint8_t weight_a,
                                 uint8_t weight_b) const {
    // ICF * (C' - 16), ICF = ICF'/63, rounded and signed: only the final sum
    // is clipped, no per-plane clamp (Green Book V.5.9/V.5.9.1).
    auto weighted_component = [](int c, uint8_t w) -> int {
        const int d = (c - 16) * int(w);
        return d >= 0 ? (d + 31) / 63 : -((31 - d) / 63);
    };

    bool mix_enabled = (transparency_control_ & 0x800000u) == 0;
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
    // else backdrop. Mixing with neither plane present shows the backdrop too (mcd212.sv).
    bool b_in_front = (plane_order_ & 0x01) != 0;
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

void Mcd212::render_line(int line, const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size) {
    update_region_arrays();
    uint32_t* row = fb_.data() + static_cast<std::size_t>(line) * kFbWidth;
    uint8_t* mask = backdrop_mask_.data() + static_cast<std::size_t>(line) * kFbWidth;
    uint32_t row_a[kFbWidth], row_b[kFbWidth];

    // RGB555: plane A Off, CM1 = 1, composited as plane B (V.4.4.8/V.4.6.1).
    if (icm_[0] == 0x0 && icm_[1] == 0x1) {
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
    uint32_t color = 0;
    for (int i = 0; i < width; ++i) {
        const int x = i * step;
        const uint32_t a = row_a[i], b = row_b[i];
        if (i == 0 || a != row_a[i - 1] || b != row_b[i - 1] || weight_a[x] != weight_a[x - step] ||
            weight_b[x] != weight_b[x - step]) {
            color = composite_pixel(a, a != kNoPixel, b, b != kNoPixel, weight_a[x], weight_b[x]);
        }
        const uint8_t backdrop = a == kNoPixel && b == kNoPixel;
        row[x] = color;
        mask[x] = backdrop;
        if (step == 2) {
            row[x + 1] = color;
            mask[x + 1] = backdrop;
        }
    }
}

void Mcd212::decode_plane_row(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row, int step) {
    bool opaque[kFbWidth];
    // Transparency per Double Resolution column after Pixel Hold, as
    // mcd212.sv: RC_X can split a Normal Resolution pixel.
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

// CLUT4/RL3 (Green Book Figure V.34): two pixels per byte, high nibble first;
// Double Resolution regardless of DCR's CM bit (mcd212.sv, clut_rle.sv).
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
    const uint8_t rms = uint8_t((ddr >> 8) & 0x3);
    const bool run_length = rms == 0x2;
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
        const int repeat_factor = rms == 0x3 ? pixel_repeat(ddr) : 1;
        fetch_codes(codes, units, plane_ram, vsr, mask, repeat_factor);
        vsr += uint32_t(units / repeat_factor);
    }
    const uint8_t bank = path == 1 ? 0x80 : 0x00;
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

// One frame of mcd212.sv's blink timing; CON or COF 0 disables blinking.
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
    if (!(cursor_control_ & 0x800000u)) return; // CURCNT_EN
    // Off phase: BLKC 0 hides the cursor, BLKC 1 complements its colour.
    if (cursor_blink_state_ && !(cursor_control_ & kCurcntBlkc)) return;
    int cursor_x = int(cursor_position_ & 0x3ffu);
    int cursor_y = int((cursor_position_ >> 12) & 0x3ffu);
    uint32_t color_index = cursor_control_ & 0xfu; // CURCNT_COLOR
    if (cursor_blink_state_) color_index ^= 0x7u; // complementary: flip R/G/B, keep Y
    uint32_t color = k4bppColor[color_index];
    // CURCNT_CUW: one Double Resolution pixel per pattern bit, else two.
    int px_per_bit = (cursor_control_ & 0x8000u) ? 1 : 2;
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
    }
}

void Mcd212::overlay_dvc_frame() {
    if (!dvc_attached_) return;
    // The frame player shows on every vsd pixel, black outside its window
    // (cditop.sv, frameplayer.sv); studio black, not the RTL's 0.
    for (std::size_t i = 0; i < fb_.size(); ++i) {
        if (backdrop_mask_[i]) fb_[i] = kStudioBlack;
    }
    if (!dvc_frame_ || dvc_frame_width_ <= 0 || dvc_frame_height_ <= 0) return;
    if (dvc_decwin_w_ == 0 || dvc_decwin_h_ == 0 || !dvc_fmv_show_) return;

    // Placement and the frame are in Normal Resolution pixels.
    const int height = std::min({int(dvc_decwin_h_), kFbHeight - dvc_scrpos_y_, dvc_frame_height_ - dvc_decoff_y_});
    const int width = std::min(int(dvc_decwin_w_), dvc_frame_width_ - dvc_decoff_x_);
    // VCD clock: a decoded pixel per 512/231 columns, else two (vmpeg.sv).
    const int columns = dvc_vcd_pixel_clock_ ? std::min((width * 512 + 230) / 231, 2 * (kLineWidth - dvc_scrpos_x_))
                                             : 2 * std::min(width, kLineWidth - dvc_scrpos_x_);
    for (int y = 0; y < height; ++y) {
        const std::size_t row = std::size_t(dvc_scrpos_y_ + y) * kFbWidth + 2 * std::size_t(dvc_scrpos_x_);
        const std::size_t src_y = std::size_t(dvc_decoff_y_) + y;
        const uint8_t* src = dvc_frame_ + (src_y * dvc_frame_width_ + dvc_decoff_x_) * 4;
        for (int c = 0; c < columns; ++c) {
            if (!backdrop_mask_[row + c]) continue;
            const uint8_t* p = src + (dvc_vcd_pixel_clock_ ? std::size_t(c * 231 / 512) : std::size_t(c / 2)) * 4;
            fb_[row + c] = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
        }
    }
}

int Mcd212::begin_field(int lines) {
    // video_timing.sv: the even field ends a line later (the half line).
    field_lines_ = even_field_ ? lines + 1 : lines;
    weaving_ = even_field_ || (dcr_[0] & kDcr1Sm);
    vertical_blank_started_ = false;
    const bool fd = (dcr_[0] & kDcr1Fd) != 0, st = (csrw_[0] & kCsr1wSt) != 0;
    active_height_ = (fd || st) ? 240 : kFbHeight;
    active_width_ = ((dcr_[0] & kDcr1Cf) && !st) ? kLineWidth : 360;
    display_aspect_ = tv_display_aspect(active_width_, active_height_, dcr_[0] & kDcr1Cf, fd);
    // video_timing.sv v_start: 18 at 60 Hz, else 46 (240 lines) or 26 (280).
    first_active_line_ = fd ? 18 : (st ? 46 : 26);
    // A 50 Hz picture doesn't fit a 262-line field.
    active_height_ = std::max(0, std::min(active_height_, field_lines_ - 1 - first_active_line_));
    csr1r_ &= ~kCsr1rDa; // vsync is in vertical blanking
    return field_lines_;
}

void Mcd212::end_of_line(int line, const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size) {
    const int y = line - first_active_line_;
    if (y >= 0 && y < active_height_) {
        render_line(y, plane_a, plane_b, plane_size);
        output_row(y);
        // mcd212.sv: vsd = backdrop_pixel && ev; EV can change per line.
        if (!icm_ev_) {
            std::fill_n(backdrop_mask_.begin() + static_cast<std::ptrdiff_t>(y) * kFbWidth, kFbWidth, uint8_t{0});
        }
        // DCA in the horizontal blanking after its line (mcd212.sv dca_read).
        if (dcr_[0] & kDcrDc) process_dca(0, plane_a, plane_size);
        if (dcr_[1] & kDcrDc) process_dca(1, plane_b, plane_size);
    }
    if (y + 1 == 0 && active_height_ > 0) csr1r_ |= kCsr1rDa; // the next line is active
    if (y + 1 == active_height_ || (line == field_lines_ - 1 && !vertical_blank_started_)) {
        start_vertical_blank(plane_a, plane_b, plane_size);
    }
}

// DA falls and the ICA runs as pixel output stops (mcd212.sv new_frame).
void Mcd212::start_vertical_blank(const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size) {
    vertical_blank_started_ = true;
    csr1r_ &= ~kCsr1rDa;
    if (dcr_[0] & kDcrIc) process_ica(0, plane_a, plane_size);
    if (dcr_[1] & kDcrIc) process_ica(1, plane_b, plane_size);
    // same_cdi ica_tick: `m_dca = get_dcp()`.
    dca_[0] = join_address(ddr_[0], dcp_[0]);
    dca_[1] = join_address(ddr_[1], dcp_[1]);
}

void Mcd212::end_field() {
    // Lines below the active area are blanking, cleared.
    const std::size_t from = static_cast<std::size_t>(active_height_) * kFbWidth;
    std::fill(fb_.begin() + std::ptrdiff_t(from), fb_.end(), 0u);
    std::fill(backdrop_mask_.begin() + std::ptrdiff_t(from), backdrop_mask_.end(), uint8_t{0});
    update_cursor_blink();
    // The cursor stays on top of FMV (mcd212.sv clears vsd under it).
    overlay_dvc_frame();
    draw_cursor();
    for (int y = 0; y < kFbHeight; ++y)
        output_row(y);
    woven_last_ = weaving_;
    // video_timing.sv: an odd field is followed by an even one only with SM;
    // PA (fake_parity) toggles, and is 1 after every even field.
    if (even_field_) {
        even_field_ = false;
        csr1r_ |= kCsr1rPa;
    } else {
        even_field_ = (dcr_[0] & kDcr1Sm) != 0;
        csr1r_ ^= kCsr1rPa;
    }
}

// A fresh weave fills both rows, so no stale field shows.
void Mcd212::output_row(int y) {
    const uint32_t* src = fb_.data() + static_cast<std::size_t>(y) * kFbWidth;
    auto put = [&](int row) { std::copy_n(src, kFbWidth, out_.data() + static_cast<std::size_t>(row) * kFbWidth); };
    if (!weaving_) {
        put(y);
        return;
    }
    const int parity = even_field_ ? 1 : 0;
    put(2 * y + parity);
    if (!woven_last_) put(2 * y + 1 - parity);
}

} // namespace mamecdi
} // namespace zcdi
