#pragma once
#include <cstdint>
#include <vector>

namespace zcdi {
namespace mamecdi {

// Register values, as Mcd212SaveState and plane memory hold them (names after same_cdi).
namespace mcd212 {

// MMIO byte offsets from 0x4fffe0; the "1" registers are path 0.
enum MmioOffset : uint32_t {
    kCsr2W = 0x00,
    kCsr2R = 0x01,
    kDcr2 = 0x02,
    kVsr2 = 0x04,
    kDdr2 = 0x08,
    kDca2 = 0x0a,
    kCsr1W = 0x10,
    kCsr1R = 0x11,
    kDcr1 = 0x12,
    kVsr1 = 0x14,
    kDdr1 = 0x18,
    kDca1 = 0x1a,
};

// CSR1R/CSR2R.
enum CsrRead : uint8_t {
    kCsr2rIt2 = 0x02,
    kCsr2rIt1 = 0x04,
    kCsr1rPa = 0x20, // parity (odd/even field)
    kCsr1rDa = 0x80, // display active
};

// CSR1W/CSR2W. DI masks the IRQ1 pin only; IT1/IT2 still latch (mcd212.sv:203-204).
enum CsrWrite : uint16_t {
    kCsr1wSt = 0x0002,
    kCsrWDi = 0x8000,
};

// DCR1/DCR2, bits 0-5 being VSR's high address bits. SM, FD and CF are DCR1 only.
enum Dcr : uint16_t {
    kDcrDc = 0x0100, // DCA enable (mcd212.sv:499-513)
    kDcrIc = 0x0200, // ICA enable
    kDcrCm = 0x0800,
    kDcr1Sm = 0x1000, // scan mode: interlace
    kDcr1Fd = 0x2000,
    kDcr1Cf = 0x4000,
    kDcrDe = 0x8000,
};

// DDR file type (bits 8-9) and Pixel Repeat factor (bits 10-11).
enum DdrShift : int {
    kDdrFtShift = 8,
    kDdrMtShift = 10,
};
enum FileType : uint8_t {
    kDdrFtBitmap = 0x0,
    kDdrFtRle = 0x2,
    kDdrFtMosaic = 0x3, // Pixel Repeat
};

// ICA/DCA instructions, each the first opcode of its range (same_cdi process_ica).
enum Command : uint8_t {
    kCmdStop = 0x00,
    kCmdNop = 0x10,
    kCmdReloadDcp = 0x20,
    kCmdReloadDcpStop = 0x30,
    kCmdReloadVsr = 0x40,
    kCmdReloadVsrStop = 0x50,
    kCmdInterrupt = 0x60,
    kCmdUndefined = 0x70,
    kCmdReloadDisplayParams = 0x78,
    kCmdSetRegister = 0x80, // the opcode is the register number
};

// ICA start in plane memory; even interlaced fields skip one instruction.
enum IcaStart : uint32_t {
    kIcaStart = 0x400u,
    kIcaStartEvenField = 0x404u,
};
constexpr int kIcaMaxCommands = 1920; // 3840 16-bit words / 2 words-per-command
constexpr int kDcaBudgetBytes = 64; // real hardware budgets exactly 64 bytes/line

// Registers set by ICA/DCA instructions (same_cdi set_register).
enum Register : uint8_t {
    kRegClut = 0x80, // 0x80-0xbf: 64 entries of the selected bank
    kRegImageCodingMethod = 0xc0,
    kRegTransparencyControl = 0xc1,
    kRegPlaneOrder = 0xc2,
    kRegClutBank = 0xc3,
    kRegTransparentColorA = 0xc4,
    kRegTransparentColorB = 0xc6,
    kRegMaskColorA = 0xc7,
    kRegMaskColorB = 0xc9,
    kRegDyuvAbsStartA = 0xca,
    kRegDyuvAbsStartB = 0xcb,
    kRegCursorPosition = 0xcd,
    kRegCursorControl = 0xce,
    kRegCursorPattern = 0xcf,
    kRegRegionControl = 0xd0, // 0xd0-0xd7
    kRegBackdropColor = 0xd8,
    kRegPixelHoldA = 0xd9,
    kRegPixelHoldB = 0xda,
    kRegWeightFactorA = 0xdb,
    kRegWeightFactorB = 0xdc,
};

constexpr int kClutBankSize = 64;
constexpr uint8_t kClutPlaneB = 0x80; // CLUT index of bank 2, plane B's half
// CLUT entries and color keys hold 6 bits per channel.
constexpr uint32_t kColorBits = 0x00fcfcfcu;

// Image Coding Method flags (same_cdi ICM_*).
enum Icm : uint32_t {
    kIcmEv = 0x040000u, // External Video
    kIcmNr = 0x080000u, // number of region flags (same_cdi ICM_NM)
    kIcmCs = 0x400000u, // CLUT select
};

// A plane's coding method, Mcd212SaveState::icm_.
enum CodingMethod : uint8_t {
    kIcmOff = 0x0,
    kIcmClut8 = 0x1,
    kIcmRgb555 = 0x1, // plane B, with plane A off
    kIcmClut7 = 0x3,
    kIcmClut77 = 0x4,
    kIcmDyuv = 0x5,
    kIcmClut4 = 0xb, // CLUT4, or RL3 with run-length decode
};

// Transparency Control: a 4-bit condition per plane, plane B's at bit 8 (same_cdi TCR_*).
constexpr int kTcrPlaneBShift = 8;
enum TransparencyControl : uint32_t {
    kTcrAlways = 0x0,
    kTcrKey = 0x1,
    kTcrRgb = 0x2, // RGB555 T bit
    kTcrRf0 = 0x3,
    kTcrRf1 = 0x4,
    kTcrRf0Key = 0x5,
    kTcrRf1Key = 0x6,
    kTcrCondition = 0x07,
    kTcrInvert = 0x08, // 8-15 invert 0-7
    kTcrNotKey = 0x9,
    kTcrField = 0x0f,
    kTcrDisableMx = 0x800000u,
};

constexpr uint8_t kPorBa = 0x01; // Plane Order: B in front of A
constexpr uint32_t kPixelHoldEnable = 0x800000u;

// Region Control fields (same_cdi RC_X/RC_WF/RC_RF_BIT/RC_OP).
enum RegionControl : uint32_t {
    kRcX = 0x0003ffu, // Double Resolution column
    kRcWf = 0x00fc00u,
    kRcOp = 0xf00000u,
};
enum RegionControlShift : int {
    kRcWfShift = 10,
    kRcRfBit = 16,
    kRcOpShift = 20,
};

// Region Control operations (Green Book V.5.10.2); the others do nothing.
enum RegionOp : uint32_t {
    kRcOpEnd = 0,
    kRcOpWfA = 4,
    kRcOpWfB = 6,
    kRcOpResetRf = 8,
    kRcOpSetRf = 9,
    kRcOpWfAResetRf = 12,
    kRcOpWfASetRf = 13,
    kRcOpWfBResetRf = 14,
    kRcOpWfBSetRf = 15,
};

// Cursor Control, per mcd212.sv's cursor_control_register (same_cdi's
// CURCNT_CON 0x280000 is not contiguous). Periods are in units of 12 frames.
enum CursorControl : uint32_t {
    kCurcntColor = 0x00000fu, // a 4bpp palette index
    kCurcntCuw = 0x008000u, // 1 Double Resolution pixel per pattern bit, else 2
    kCurcntCofMask = 0x070000u, // "off"/complementary period
    kCurcntConMask = 0x380000u, // "on" period
    kCurcntBlkc = 0x400000u, // blink type (0=on/off, 1=normal/complementary)
    kCurcntEn = 0x800000u,
};
enum CursorControlShift : int {
    kCurcntCofShift = 16,
    kCurcntConShift = 19,
};
constexpr int kCursorBlinkSubPeriodFrames = 12; // mcd212.sv CURSOR_BLINK_PERIOD

// Fixed palette for Backdrop Color and kCurcntColor, XRGB (same_cdi s_4bpp_color).
constexpr uint32_t k4bppColor[16] = {
    0x101010, 0x10107a, 0x107a10, 0x107a7a, 0x7a1010, 0x7a107a, 0x7a7a10, 0x7a7a7a,
    0x101010, 0x1010e6, 0x10e610, 0x10e6e6, 0xe61010, 0xe610e6, 0xe6e610, 0xe6e6e6,
};

// DYUV delta per 4-bit code (same_cdi s_dyuv_deltas).
constexpr uint8_t kDyuvDeltas[16] = {0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255};

} // namespace mcd212

struct Mcd212SaveState {
    // Normal Resolution pixels per line; the framebuffer is Double Resolution.
    static constexpr int kLineWidth = 384;
    static constexpr int kFbWidth = 2 * kLineWidth;
    static constexpr int kFbHeight = 280;

    // Path-paired registers: [0] is path 0 (CSR1W, DCR1, ...), [1] path 1.
    uint16_t csrw_[2] = {};
    uint8_t csr1r_ = 0, csr2r_ = 0;
    uint16_t dcr_[2] = {};
    uint16_t vsr_[2] = {};
    uint16_t ddr_[2] = {}; // low 6 bits: DCP's high address bits
    // Persistent DCP (set by RELOAD DCP); DCA reloads from it each frame.
    uint16_t dcp_[2] = {};
    // DCA's per-line byte cursor, up to 22 bits (registers show the low 16).
    uint32_t dca_[2] = {};

    uint32_t clut_[256] = {}; // XRGB, 6 bits/channel; shared by both paths
    uint8_t clut_bank_[2] = {0, 0}; // kRegClutBank, per path
    uint8_t icm_[2] = {0, 0}; // mcd212::CodingMethod; kRegImageCodingMethod bits 0-3 / 8-11
    bool icm_nr_ = false; // kIcmNr
    bool icm_cs_ = false; // kIcmCs
    bool icm_ev_ = false; // kIcmEv
    uint32_t dyuv_abs_start_[2] = {0, 0}; // kRegDyuvAbsStartA/B: Y<<16|U<<8|V

    uint32_t transparency_control_ = 0; // mcd212::TransparencyControl
    uint8_t plane_order_ = 0; // only kPorBa is used
    uint32_t transparent_color_[2] = {0, 0}; // kRegTransparentColorA/B
    uint32_t mask_color_[2] = {0, 0}; // kRegMaskColorA/B
    uint8_t backdrop_color_ = 0; // a 4bpp palette index, not RGB
    uint32_t pixel_hold_[2] = {}; // kRegPixelHoldA/B
    // kRegWeightFactorA/B: only the x=0 seed for weight_factor_line_.
    uint8_t weight_factor_[2] = {0, 0};
    uint32_t region_control_[8] = {}; // mcd212::RegionControl, shared by both paths
    // NR as of the last Region Control / weight write; the region arrays use it.
    bool region_arrays_nr_ = false;

    uint32_t cursor_position_ = 0; // kRegCursorPosition
    uint32_t cursor_control_ = 0; // mcd212::CursorControl
    uint16_t cursor_pattern_[16] = {};

    int cursor_blink_framecnt_ = 0; // 0-11, frames within one 12-frame sub-period
    int cursor_blink_periodcnt_ = 0; // sub-periods elapsed in the current phase
    bool cursor_blink_state_ = false; // true = off/complementary phase

    int dvc_frame_width_ = 0;
    int dvc_frame_height_ = 0;

    bool irq1_ = false;
    // Interlace (DCR1.SM): video_timing.sv parity == 0, the field after an odd one.
    bool even_field_ = false;
};

class Mcd212 : private Mcd212SaveState {
public:
    Mcd212();

    // offset = addr - 0x4fffe0 (0..0x1f). A CSR2R read acknowledges IRQ1.
    uint8_t read_byte(uint32_t offset);
    void write_byte(uint32_t offset, uint8_t value);

    // One field: begin_field(), end_of_line() for lines 0..lines-1 from
    // vsync, end_field(). Plane buffers hold at least `plane_size` bytes.
    // `lines` is the non-interlaced count; returns this field's (even interlaced fields have one more).
    int begin_field(int lines);
    void end_of_line(int line, const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size);
    // DVC overlay, cursor and field parity; the framebuffer is complete.
    void end_field();

    bool irq1_asserted() const { return irq1_; }
    // For debuggers (MachineInspector).
    const Mcd212SaveState& state() const { return *this; }
    // The field line shown as framebuffer row 0; set by begin_field().
    int first_active_line() const { return first_active_line_; }
    // For debuggers: one plane decoded alone from `start`, `lines` rows of
    // kFbWidth pixels, on a copy of the chip; no DCA, transparency or mixing.
    void preview_plane(int path, uint32_t start, int lines, const uint8_t* plane_ram, std::size_t plane_size,
                       uint32_t* out) const;

    // Interlaced fields are woven: odd ones on even rows, even ones on odd rows.
    const uint32_t* framebuffer() const { return out_.data(); }
    int fb_width() const { return kFbWidth; }
    int fb_height() const { return kOutHeight; }
    // The visible part of the framebuffer, from its top-left: 768 or 720
    // pixels by 280 or 240 lines, doubled while woven. Updated at the start of each field.
    int active_width() const { return 2 * active_width_; }
    int output_height() const { return weaving_ ? 2 * active_height_ : active_height_; }
    // Lines per field.
    int active_height() const { return active_height_; }
    // Width / height of the active area as shown on a TV.
    double display_aspect() const { return display_aspect_; }

    // RGBA8888, not owned; must stay valid through the next end_field().
    // width or height <= 0 disables the overlay.
    void set_dvc_frame(const uint8_t* rgba, int width, int height) {
        dvc_frame_ = rgba;
        dvc_frame_width_ = width;
        dvc_frame_height_ = height;
    }

    // The DVC frame pointer is not saved; a load clears it (see rebind_dvc_frame).
    template <class T>
    void state_io(T& archive);

    // After a load: re-points the DVC overlay at `rgba` (`size` bytes) if
    // the saved state had a frame bound and it still fits, else disables it.
    void rebind_dvc_frame(const uint8_t* rgba, size_t size) {
        size_t need = size_t(dvc_frame_width_ > 0 ? dvc_frame_width_ : 0) *
                      size_t(dvc_frame_height_ > 0 ? dvc_frame_height_ : 0) * 4;
        if (rgba && need > 0 && need <= size) {
            dvc_frame_ = rgba;
        } else {
            set_dvc_frame(nullptr, 0, 0);
        }
    }

    // With a DVC cartridge, EV backdrop pixels show its frame player (cditop.sv vsd).
    void set_dvc_attached(bool attached) { dvc_attached_ = attached; }

    // FMV window (VmpegDevice::DisplayWindow), set every field with
    // set_dvc_frame(). decwin_w/h == 0: nothing is drawn.
    void set_dvc_window(uint16_t scrpos_x, uint16_t scrpos_y, uint16_t decwin_w, uint16_t decwin_h, uint16_t decoff_x,
                        uint16_t decoff_y, bool show, bool vcd_pixel_clock) {
        dvc_scrpos_x_ = scrpos_x;
        dvc_scrpos_y_ = scrpos_y;
        dvc_decwin_w_ = decwin_w;
        dvc_decwin_h_ = decwin_h;
        dvc_decoff_x_ = decoff_x;
        dvc_decoff_y_ = decoff_y;
        dvc_fmv_show_ = show;
        dvc_vcd_pixel_clock_ = vcd_pixel_clock;
    }

private:
    void start_vertical_blank(const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size);
    void process_ica(int path, const uint8_t* plane_ram, std::size_t plane_size);
    void process_dca(int path, const uint8_t* plane_ram, std::size_t plane_size);
    void set_register(int path, uint8_t reg, uint32_t value);
    // VSR as its 22-bit address: DCR bits 0-5 above the 16 VSR bits.
    uint32_t vsr_address(int path) const;
    void set_vsr_address(int path, uint32_t address);
    // Region arrays are rebuilt lazily, before the next line.
    void invalidate_region_arrays();
    void update_region_arrays();
    void fill_region_arrays();
    void render_line(int line, const uint8_t* plane_a, const uint8_t* plane_b, std::size_t plane_size);
    void render_line_rgb555(uint32_t* row, bool* t_bit, const uint8_t* plane_a, const uint8_t* plane_b,
                            std::size_t plane_size);
    void apply_pixel_hold(int path, uint32_t* row, bool* opaque, int width) const;
    // `step` 2 fills kLineWidth pixels, one per identical column pair.
    void decode_plane_row(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row, int step = 1);
    void composite_row(const uint32_t* row_a, const uint32_t* row_b, int width, int step, uint32_t* row,
                       uint8_t* mask) const;
    // The decoders set `present` false where the plane shows nothing;
    // transparency is decided later, in decode_plane_row.
    void decode_double_res_line(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row,
                                bool* present);
    bool plane_parked(int path) const;
    void decode_plane_line(int path, const uint8_t* plane_ram, std::size_t plane_size, uint32_t* row, bool* present);
    void render_line_rle(int path, uint32_t* row, bool* present, const uint8_t* plane_ram, std::size_t plane_mask);
    void decode_dyuv_line(const uint8_t* src, uint32_t* row, uint32_t dyuv_abs_start) const;
    struct ClutIndex {
        bool valid = false; // false: the plane shows flat gray
        uint8_t mask = 0;
        uint8_t bank = 0;
    };
    ClutIndex clut_index(int path, uint8_t icm) const;
    uint32_t clut_color(const ClutIndex& index, uint8_t byte) const;
    uint32_t clut_pixel(int path, uint8_t byte, uint8_t icm) const;
    // `x` is the Double Resolution column (for the region flags).
    bool plane_transparent(int path, uint32_t color, int x) const;
    // plane_transparent() split into the per-line part and the per-pixel test.
    struct TransparencyRule {
        bool always = false; // matches every pixel
        int flag = -1; // the region flag (RF0/RF1) that matches, or -1
        bool color_key = false; // pixels matching the Color Key match
        bool invert = false; // the condition's inverted form (8-15)
        uint32_t key = 0;
        uint32_t compared_bits = 0;
    };
    TransparencyRule transparency_rule(int path) const;
    bool is_transparent(const TransparencyRule& rule, uint32_t color, int x) const;
    bool rgb555_transparent(int x, bool transparency_bit) const;
    uint32_t composite_pixel(uint32_t color_a, bool opaque_a, uint32_t color_b, bool opaque_b, uint8_t weight_a,
                             uint8_t weight_b) const;
    void update_cursor_blink();
    void draw_cursor();
    void overlay_dvc_frame();

    // Per Double Resolution column of the current line: [plane][x] / [flag][x].
    // Not saved: rebuilt from Region Control after a load.
    uint8_t weight_factor_line_[2][kFbWidth] = {};
    bool region_flag_line_[2][kFbWidth] = {};
    // Both columns of every Normal Resolution pixel match in the arrays
    // above; false only costs speed.
    bool region_pairs_uniform_ = true;
    bool region_arrays_dirty_ = false;

    const uint8_t* dvc_frame_ = nullptr;
    // FMV window, not saved: set_dvc_window() runs before every end_field().
    uint16_t dvc_scrpos_x_ = 0;
    uint16_t dvc_scrpos_y_ = 0;
    uint16_t dvc_decwin_w_ = 0;
    uint16_t dvc_decwin_h_ = 0;
    uint16_t dvc_decoff_x_ = 0;
    uint16_t dvc_decoff_y_ = 0;
    bool dvc_fmv_show_ = false;
    bool dvc_vcd_pixel_clock_ = false;
    bool dvc_attached_ = false;

    static constexpr int kOutHeight = 2 * kFbHeight;
    // The field as drawn; framebuffer() is out_, fed row by row. fb_ isn't saved;
    // out_ only for the rows the next woven field keeps.
    std::vector<uint32_t> fb_;
    std::vector<uint32_t> out_;
    bool weaving_ = false; // this field goes to alternate rows
    bool woven_last_ = false; // so did the previous one
    // In Normal Resolution pixels (= bytes fetched per line); not saved.
    int active_width_ = kLineWidth;
    int active_height_ = kFbHeight;
    // This field's timing, fixed at begin_field(); not saved (states are taken between fields).
    int field_lines_ = 312;
    int first_active_line_ = 26;
    bool vertical_blank_started_ = false;
    void output_row(int y);
    double display_aspect_ = 4.0 / 3.0;

    // 1 where neither plane is opaque and EV is set: where DVC video shows.
    // Bytes, not vector<bool>: written for every pixel.
    std::vector<uint8_t> backdrop_mask_;

    bool loaded();
};

} // namespace mamecdi
} // namespace zcdi
