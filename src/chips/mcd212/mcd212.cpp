#include "chips/mcd212/mcd212.h"

#include "core/state_archive.h"
#include "core/util.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

namespace zcdi {
namespace chips {

using namespace mcd212;

namespace {
// 1920 (30 MHz) or 1792 (28 MHz) clocks per line, 4 per pixel (datasheet
// Table 5-4); a 4:3 screen is 52.4 us by 287.5/242.5 lines (Green Book V.2.4.1).
double tv_display_aspect(int pixels, int lines, bool crystal_30mhz, bool hz60) {
    const double line_us = hz60 ? 63.556 : 64.0;
    const double active_us = pixels * 4 / (crystal_30mhz ? 1920.0 : 1792.0) * line_us;
    const double screen_lines = hz60 ? 242.5 : 287.5;
    return 4.0 / 3.0 * (active_us / 52.4) * (screen_lines / lines);
}

uint32_t join_address(uint16_t high_reg, uint16_t low_reg) { return (uint32_t(high_reg & 0x3f) << 16) | low_reg; }

void split_address(uint16_t& high_reg, uint16_t& low_reg, uint32_t address) {
    low_reg = uint16_t(address & 0xffffu);
    high_reg = uint16_t((high_reg & 0xffc0) | ((address >> 16) & 0x3f));
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
    if constexpr (T::kLoading) rebuild_after_load();
}
ZCDI_STATE_IO_INSTANTIATE(Mcd212);

bool Mcd212SaveState::state_valid() const { return clut_bank_[0] <= 3 && clut_bank_[1] <= 3; }

// fb_ and backdrop_mask_ aren't saved: every field rewrites every line.
void Mcd212::rebuild_after_load() {
    std::fill(fb_.begin(), fb_.end(), 0u);
    if (!woven_last_) std::fill(out_.begin(), out_.end(), 0u);
    std::fill(backdrop_mask_.begin(), backdrop_mask_.end(), uint8_t{0});
    dvc_frame_ = nullptr;
    region_arrays_dirty_ = true; // rebuilt from the loaded Region Control
}

namespace {
constexpr uint32_t kPath0Mmio = kCsr1W - kCsr2W;
int mmio_path(uint32_t offset) { return (offset & kPath0Mmio) ? 0 : 1; }
uint32_t mmio_register(uint32_t offset) { return offset & ~(kPath0Mmio | 1u); }
} // namespace

uint16_t Mcd212::mmio_word(uint32_t offset) const {
    const int path = mmio_path(offset);
    switch (mmio_register(offset)) {
        case kCsr2W: return csrw_[path];
        case kDcr2: return dcr_[path];
        case kVsr2: return vsr_[path];
        case kDdr2: return ddr_[path];
        case kDca2: return uint16_t(dca_[path]);
        default: return 0; // unmapped in this window
    }
}

void Mcd212::set_mmio_word(uint32_t offset, uint16_t v) {
    const int path = mmio_path(offset);
    switch (mmio_register(offset)) {
        case kCsr2W: csrw_[path] = v; return;
        case kDcr2: dcr_[path] = v; return;
        case kVsr2: vsr_[path] = v; return;
        case kDdr2: ddr_[path] = v; return;
        // MMIO reaches only DCA's low 16 bits; RELOAD DCP sets the rest.
        case kDca2: dca_[path] = (dca_[path] & ~0xffffu) | v; return;
        default: return;
    }
}

uint8_t Mcd212::read_byte(uint32_t offset) {
    if (offset == kCsr2R) {
        const uint8_t v = csr2r_;
        csr2r_ &= ~(kCsr2rIt1 | kCsr2rIt2); // read acknowledges the interrupt
        irq1_ = false;
        return v;
    }
    if (offset == kCsr1R) return csr1r_;
    if (mmio_register(offset) == kCsr2W) return 0;
    return byte_lane(mmio_word(offset), offset);
}

void Mcd212::write_byte(uint32_t offset, uint8_t v) {
    set_mmio_word(offset, set_byte_lane(mmio_word(offset), offset, v));
}

void Mcd212::set_register(int path, uint8_t reg, uint32_t value) {
    if (reg >= kRegClut && reg < kRegImageCodingMethod) {
        // Into the 64-entry quarter this path's CLUT bank selects.
        std::size_t clut_index = std::size_t(clut_bank_[path & 1]) * kClutBankSize + (reg - kRegClut);
        clut_[clut_index] = value & kColorBits;
        return;
    }
    if (reg >= kRegRegionControl && reg < kRegBackdropColor) {
        // Either path may write Region Control.
        region_control_[reg - kRegRegionControl] = value;
        invalidate_region_arrays();
        return;
    }
    switch (reg) {
        case kRegImageCodingMethod:
            if (path != 0) return;
            // Both planes' modes (bits 0-3, 8-11) plus NR/EV/CS.
            icm_[0] = uint8_t(value & 0x0f);
            icm_[1] = uint8_t((value >> 8) & 0x0f);
            icm_nr_ = (value & kIcmNr) != 0;
            icm_ev_ = (value & kIcmEv) != 0;
            icm_cs_ = (value & kIcmCs) != 0;
            return;
        case kRegTransparencyControl:
            if (path == 0) transparency_control_ = value;
            return;
        case kRegPlaneOrder:
            if (path == 0) plane_order_ = uint8_t(value & 0x07);
            return;
        case kRegClutBank:
            // Path 1 can only reach banks 2-3.
            clut_bank_[path & 1] = (path == 0) ? uint8_t(value & 0x03) : uint8_t(0x02 | (value & 0x01));
            return;
        case kRegTransparentColorA:
            if (path == 0) transparent_color_[0] = value & kColorBits;
            return;
        case kRegTransparentColorB:
            if (path == 1) transparent_color_[1] = value & kColorBits;
            return;
        case kRegMaskColorA:
            if (path == 0) mask_color_[0] = value & kColorBits;
            return;
        case kRegMaskColorB:
            if (path == 1) mask_color_[1] = value & kColorBits;
            return;
        case kRegDyuvAbsStartA:
            if (path == 0) dyuv_abs_start_[0] = value;
            return;
        case kRegDyuvAbsStartB:
            if (path == 1) dyuv_abs_start_[1] = value;
            return;
        case kRegCursorPosition:
            if (path == 0) cursor_position_ = value;
            return;
        case kRegCursorControl:
            if (path == 0) cursor_control_ = value;
            return;
        case kRegCursorPattern:
            // Row index in bits 16-19.
            if (path == 0) cursor_pattern_[(value >> 16) & 0xf] = uint16_t(value & 0xffffu);
            return;
        case kRegBackdropColor:
            if (path == 0) backdrop_color_ = uint8_t(value & 0x0fu);
            return;
        case kRegPixelHoldA:
            if (path == 0) pixel_hold_[0] = value;
            return;
        case kRegPixelHoldB:
            if (path == 1) pixel_hold_[1] = value;
            return;
        case kRegWeightFactorA:
            if (path != 0) return;
            weight_factor_[0] = uint8_t(value & 0x3f); // ICF'(5-0), Green Book V.5.9
            invalidate_region_arrays();
            return;
        case kRegWeightFactorB:
            if (path != 1) return;
            weight_factor_[1] = uint8_t(value & 0x3f);
            invalidate_region_arrays();
            return;
        default: return;
    }
}

uint32_t Mcd212::vsr_address(int path) const { return join_address(dcr_[path], vsr_[path]); }

void Mcd212::set_vsr_address(int path, uint32_t address) { split_address(dcr_[path], vsr_[path], address); }

uint32_t Mcd212::dcp_address(int path) const { return join_address(ddr_[path], dcp_[path]); }

void Mcd212::set_dcp_address(int path, uint32_t address) { split_address(ddr_[path], dcp_[path], address); }

int Mcd212::begin_field(int lines) {
    // The even field ends a line later (the half line).
    field_lines_ = even_field_ ? lines + 1 : lines;
    weaving_ = even_field_ || (dcr_[0] & kDcr1Sm);
    vertical_blank_started_ = false;
    const bool fd = (dcr_[0] & kDcr1Fd) != 0, st = (csrw_[0] & kCsr1wSt) != 0;
    // Datasheet tables 5-4/5-6: 240 lines with FD or ST, else 280;
    // 384 pixels with CF and not ST, else 360.
    active_height_ = (fd || st) ? 240 : kFbHeight;
    active_width_ = ((dcr_[0] & kDcr1Cf) && !st) ? kLineWidth : 360;
    display_aspect_ = tv_display_aspect(active_width_, active_height_, dcr_[0] & kDcr1Cf, fd);
    // First active line: 18 at 60 Hz, else 46 (240 lines) or 26 (280).
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
        const bool display_enabled = (dcr_[0] & kDcrDe) != 0;
        // FMV still shows on backdrop pixels.
        if (!display_enabled) {
            std::fill_n(fb_.begin() + static_cast<std::ptrdiff_t>(y) * kFbWidth, kFbWidth, kStudioBlack);
        }
        output_row(y);
        // vsd = backdrop pixel and EV; EV can change per line.
        if (!icm_ev_) {
            std::fill_n(backdrop_mask_.begin() + static_cast<std::ptrdiff_t>(y) * kFbWidth, kFbWidth, uint8_t{0});
        }
        // DCA in the horizontal blanking after its line.
        if (display_enabled && (dcr_[0] & kDcrDc)) process_dca(0, plane_a, plane_size);
        if (display_enabled && (dcr_[1] & kDcrDc)) process_dca(1, plane_b, plane_size);
    }
    if (y + 1 == 0 && active_height_ > 0) csr1r_ |= kCsr1rDa; // the next line is active
    if (y + 1 == active_height_ || (line == field_lines_ - 1 && !vertical_blank_started_)) {
        start_vertical_blank(plane_a, plane_b, plane_size);
    }
}

// DA falls and the ICA runs as pixel output stops.
void Mcd212::start_vertical_blank(const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size) {
    vertical_blank_started_ = true;
    csr1r_ &= ~kCsr1rDa;
    if (dcr_[0] & kDcrIc) process_ica(0, plane_a, plane_size);
    if (dcr_[1] & kDcrIc) process_ica(1, plane_b, plane_size);
    // After the ICA, DCA restarts from DCP.
    dca_[0] = dcp_address(0);
    dca_[1] = dcp_address(1);
}

void Mcd212::end_field() {
    // Lines below the active area are blanking, cleared.
    const std::size_t from = static_cast<std::size_t>(active_height_) * kFbWidth;
    std::fill(fb_.begin() + std::ptrdiff_t(from), fb_.end(), 0u);
    std::fill(backdrop_mask_.begin() + std::ptrdiff_t(from), backdrop_mask_.end(), uint8_t{0});
    for (int y = active_height_; y < kFbHeight; ++y)
        output_row(y);
    update_cursor_blink();
    // Active rows went out line by line; these output the rows they change.
    // The cursor stays on top of FMV.
    overlay_dvc_frame();
    draw_cursor();
    woven_last_ = weaving_;
    // An odd field is followed by an even one only with SM;
    // PA toggles, and is 1 after every even field.
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

} // namespace chips
} // namespace zcdi
