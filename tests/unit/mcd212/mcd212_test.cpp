#include "doctest.h"

#include <cstddef>
#include <cstdint>

#include "mcd212/mcd212.h"
#include "support/mcd212_field.h"
#include "support/mcd212_regs.h"
#include "support/private_access.h"

using zcdi::mamecdi::Mcd212;
using zcdi_test::load_register;
using zcdi_test::priv;
using zcdi_test::write_mmio16;
using namespace zcdi::mamecdi::mcd212;

// The decoders and the mixer, tested on their own.
ZCDI_PRIVATE_MEMBER(ClutPixel, Mcd212, clut_pixel, uint32_t(int, uint8_t, uint8_t) const);
ZCDI_PRIVATE_MEMBER(PlaneTransparent, Mcd212, plane_transparent, bool(int, uint32_t, int) const);
ZCDI_PRIVATE_MEMBER(ApplyPixelHold, Mcd212, apply_pixel_hold, void(int, uint32_t*, bool*, int) const);
ZCDI_PRIVATE_MEMBER(DecodePlaneLine, Mcd212, decode_plane_line,
                    void(int, const uint8_t*, std::size_t, uint32_t*, bool*));
ZCDI_PRIVATE_MEMBER(PlaneParked, Mcd212, plane_parked, bool(int) const);
ZCDI_PRIVATE_MEMBER(DecodeDoubleResLine, Mcd212, decode_double_res_line,
                    void(int, const uint8_t*, std::size_t, uint32_t*, bool*));
ZCDI_PRIVATE_MEMBER(CompositePixel, Mcd212, composite_pixel,
                    uint32_t(uint32_t, bool, uint32_t, bool, uint8_t, uint8_t) const);

namespace {

// Local copies of private statics, checked against fb_width()/fb_height().
constexpr int kLineWidth = 384;
constexpr int kFbWidth = 2 * kLineWidth;
constexpr int kFbHeight = 280;  // one field; the buffer holds two, woven

uint32_t fb_at(const Mcd212& mcd, int x, int y) {
    return mcd.framebuffer()[static_cast<std::size_t>(y) * kFbWidth + x];
}

// CLUT entry `index` of `bank` (64 entries each), loaded through plane A.
void load_clut(Mcd212& mcd, int bank, int index, uint32_t color) {
    load_register(mcd, 0, kRegClutBank, uint32_t(bank));
    load_register(mcd, 0, uint8_t(kRegClut + index), color);
}

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("CM0=4 dual CLUT7 obeys ICM CS and CM1=4 is reserved") {
    Mcd212 mcd;
    load_clut(mcd, 0, 0x01, 0x00102030u);
    load_clut(mcd, 2, 0x01, 0x00405060u);
    load_register(mcd, 0, kRegImageCodingMethod, 0);
    CHECK(priv<ClutPixel>(mcd, 0, 0x81, 0x4) == 0x00102030u);
    load_register(mcd, 0, kRegImageCodingMethod, kIcmCs);
    CHECK(priv<ClutPixel>(mcd, 0, 0x81, 0x4) == 0x00405060u);
    CHECK(priv<ClutPixel>(mcd, 1, 0x81, 0x4) == 0);
}

TEST_CASE("CM1=1 is RGB555 and never standalone CLUT8") {
    Mcd212 mcd;
    load_clut(mcd, 1, 0x02, 0x00a8ccecu);
    CHECK(priv<ClutPixel>(mcd, 0, 0x42, 0x1) == 0x00a8ccecu);
    CHECK(priv<ClutPixel>(mcd, 1, 0x42, 0x1) == 0x00101010u);
}

TEST_CASE("a Color-Key mask bit makes its RGB bit don't-care") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegImageCodingMethod, kIcmClut8);
    load_register(mcd, 0, kRegTransparencyControl, kTcrKey);  // plane A: Color Key true
    load_register(mcd, 0, kRegTransparentColorA, 0x00102030u);
    load_register(mcd, 0, kRegMaskColorA, 0x00040000u);       // ignore one red bit
    CHECK(priv<PlaneTransparent>(mcd, 0, 0x00142030u, 0));
    CHECK(priv<PlaneTransparent>(mcd, 0, 0x00102030u, 0));
}

TEST_CASE("pixel hold repeats colour and opacity together") {
    Mcd212 mcd;
    REQUIRE(mcd.fb_width() == kFbWidth);
    uint32_t row[kLineWidth] = {};
    bool opaque[kLineWidth] = {};
    row[0] = 0x11u;
    row[1] = 0x22u;
    row[2] = 0x33u;
    row[3] = 0x44u;
    opaque[0] = true;
    opaque[2] = true;
    load_register(mcd, 0, kRegPixelHoldA, kPixelHoldEnable | 2);
    priv<ApplyPixelHold>(mcd, 0, row, opaque, kLineWidth);
    CHECK(row[0] == 0x11u);
    CHECK(row[1] == 0x11u);
    CHECK(row[2] == 0x33u);
    CHECK(row[3] == 0x33u);
    for (int x = 0; x < 4; ++x) CHECK(opaque[x]);
}

// Green Book Figure V.34: two Double Resolution pixels per byte, high nibble
// first; RL3 is its run-length form, with 3-bit pixels.
TEST_CASE("CLUT4 and RL3 decode two pixels per byte") {
    static uint8_t plane[0x80000] = {};
    Mcd212 mcd;
    for (int i = 0; i < 16; ++i) load_clut(mcd, 0, i, 0x00040404u * uint32_t(i));
    load_register(mcd, 0, kRegImageCodingMethod, kIcmClut4);
    write_mmio16(mcd, kDcr1, kDcrIc);
    write_mmio16(mcd, kVsr1, 0x100);
    uint32_t row[kFbWidth] = {};
    bool opaque[kFbWidth] = {};

    SUBCASE("CLUT4 bitmap") {
        plane[0x100] = 0x12;
        plane[0x101] = 0xef;
        priv<DecodeDoubleResLine>(mcd, 0, plane, sizeof(plane), row, opaque);
        CHECK(row[0] == 0x00040404u);
        CHECK(row[1] == 0x00080808u);
        CHECK(row[2] == 0x00383838u);
        CHECK(row[3] == 0x003c3c3cu);
        CHECK(mcd.state().vsr_[0] == 0x100 + kLineWidth);  // one byte per Normal Resolution pixel
    }
    SUBCASE("RL3 runs and literals") {
        write_mmio16(mcd, kDdr1, 0x0200);  // run-length
        plane[0x100] = 0x9a;       // run of 0x1a ...
        plane[0x101] = 0x02;       // ... twice
        plane[0x102] = 0x3c;       // literal: bit 3 of the low pixel is dropped
        plane[0x103] = 0x80;       // run of 0 to end of line
        plane[0x104] = 0x00;
        priv<DecodeDoubleResLine>(mcd, 0, plane, sizeof(plane), row, opaque);
        CHECK(row[0] == 0x00040404u);
        CHECK(row[1] == 0x00080808u);
        CHECK(row[2] == 0x00040404u);
        CHECK(row[3] == 0x00080808u);
        CHECK(row[4] == 0x000c0c0cu);
        CHECK(row[5] == 0x00101010u);
        CHECK(row[kFbWidth - 1] == 0);
        CHECK(mcd.state().vsr_[0] == 0x105);
    }
}

// Green Book V.5.9.1: one +16 black-level pedestal over the summed terms.
TEST_CASE("two opaque planes mix with one black-level pedestal") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, 0);  // mixing enabled
    // ICF' 63 is a factor of 1: weighted_a = 150-16 = 134, weighted_b =
    // 100-16 = 84; sum = 134 + 84 + 16 = 234 = 0xea per channel.
    CHECK(priv<CompositePixel>(mcd, 0x00969696u, true, 0x00646464u, true, uint8_t(63), uint8_t(63)) ==
          0x00eaeaeau);
}

// Green Book: "transparent areas...are equivalent to black areas".
TEST_CASE("a transparent plane contributes nothing to the mix") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, 0);
    // weighted_a = 134; b's term is gated to 0 despite color_b being bright;
    // sum = 134 + 0 + 16 = 150 = 0x96 per channel.
    CHECK(priv<CompositePixel>(mcd, 0x00969696u, true, 0x00ffffffu, false, uint8_t(63), uint8_t(63)) ==
          0x00969696u);
}

// Green Book V.5.9: no per-plane clamp, only the mixed result is clipped.
TEST_CASE("below-black components contribute negatively to the mix") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, 0);
    CHECK(priv<CompositePixel>(mcd, 0x00202020u, true, 0x00020202u, true, uint8_t(63), uint8_t(63)) ==
          0x00121212u);
    // Full ICF shows a lone plane's decoded color unchanged, below black too.
    CHECK(priv<CompositePixel>(mcd, 0x00080808u, true, 0, false, uint8_t(63), uint8_t(63)) == 0x00080808u);
    // Clipped at 0, not wrapped.
    CHECK(priv<CompositePixel>(mcd, 0x00000000u, true, 0x00000000u, true, uint8_t(63), uint8_t(63)) == 0);
}

// No DVC video under a visible cursor pixel. 16x16
// at (200,100), 2 pixels per pattern bit (CUW clear), CON=COF=1: 12-frame phases.
TEST_CASE("the cursor is drawn over DVC video") {
    constexpr uint32_t kVideoGreen = 0x0000c800u;
    constexpr uint32_t kCursorRed = 0x00e61010u;   // k4bppColor[0xc]
    constexpr uint32_t kCursorCyan = 0x0010e6e6u;  // k4bppColor[0xc ^ 0x7]
    constexpr uint32_t kCursorBlink = (1u << 19) | (1u << 16);

    static uint8_t planes[2][0x80000] = {};
    static uint8_t video[kLineWidth * kFbHeight * 4];
    for (std::size_t i = 0; i < sizeof(video); i += 4) {
        video[i] = 0x00; video[i + 1] = 0xc8; video[i + 2] = 0x00; video[i + 3] = 0xff;
    }

    {
        Mcd212 mcd;
        REQUIRE(mcd.fb_width() == kFbWidth);
        REQUIRE(mcd.fb_height() == 2 * kFbHeight);
    }
    auto run = [&](bool complementary, uint32_t& at_tick1, uint32_t& at_tick12) {
        Mcd212 mcd;
        mcd.set_dvc_attached(true);
        mcd.set_dvc_frame(video, kLineWidth, kFbHeight);
        // Full-screen window, shown.
        mcd.set_dvc_window(0, 0, kLineWidth, kFbHeight, 0, 0, /*show=*/true, /*vcd_pixel_clock=*/false);
        load_register(mcd, 0, kRegImageCodingMethod, kIcmEv);
        write_mmio16(mcd, kDcr1, kDcrDe);
        load_register(mcd, 0, kRegCursorControl, kCurcntEn | (complementary ? kCurcntBlkc : 0u) | kCursorBlink | 0xcu);
        load_register(mcd, 0, kRegCursorPosition, (100u << 12) | 200u);  // X is in 768-wide units
        for (uint32_t row = 0; row < 16; ++row) load_register(mcd, 0, kRegCursorPattern, row << 16 | 0xffffu);
        for (int t = 1; t <= 12; ++t) {
            zcdi_test::run_field(mcd, planes[0], planes[1], sizeof(planes[0]));
            if (t == 1) {
                at_tick1 = fb_at(mcd, 210, 105);
                CHECK(fb_at(mcd, 50, 50) == kVideoGreen);  // video shows outside the cursor
            }
            if (t == 12) at_tick12 = fb_at(mcd, 210, 105);
        }
    };

    uint32_t tick1 = 0, tick12 = 0;
    SUBCASE("on/off blink") {
        run(/*complementary=*/false, tick1, tick12);
        CHECK(tick1 == kCursorRed);    // cursor drawn over the video
        CHECK(tick12 == kVideoGreen);  // off phase shows the video
    }
    SUBCASE("complementary blink") {
        run(/*complementary=*/true, tick1, tick12);
        CHECK(tick1 == kCursorRed);
        CHECK(tick12 == kCursorCyan);  // R/G/B flipped, still on top
    }
}

// Black outside the latched window and before Show; the DVC video
// shows on every EV backdrop pixel while a DVC is attached.
TEST_CASE("DVC video needs a latched window and Show") {
    constexpr uint32_t kVideoGreen = 0x0000c800u;
    constexpr uint32_t kBlack = 0x00101010u;      // studio black
    constexpr uint32_t kBackdropRed = 0x00e61010u;  // k4bppColor[0xc]
    static uint8_t planes[2][0x80000] = {};
    static uint8_t video[32 * 16 * 4];
    for (std::size_t i = 0; i < sizeof(video); i += 4) {
        video[i] = 0x00; video[i + 1] = 0xc8; video[i + 2] = 0x00; video[i + 3] = 0xff;
    }
    auto pixel = [&](uint16_t win_w, uint16_t win_h, bool show, int x, int y, bool attached = true) {
        Mcd212 mcd;
        load_register(mcd, 0, kRegBackdropColor, 0xc);
        mcd.set_dvc_attached(attached);
        mcd.set_dvc_frame(video, 32, 16);
        mcd.set_dvc_window(/*scrpos=*/10, 20, win_w, win_h, /*decoff=*/0, 0, show, /*vcd_pixel_clock=*/false);
        load_register(mcd, 0, kRegImageCodingMethod, kIcmEv);
        write_mmio16(mcd, kDcr1, kDcrDe);
        zcdi_test::run_field(mcd, planes[0], planes[1], sizeof(planes[0]));
        return fb_at(mcd, x, y);
    };
    // Normal Resolution placement, so x is doubled in the framebuffer.
    CHECK(pixel(32, 16, true, 2 * 10, 20) == kVideoGreen);
    CHECK(pixel(32, 16, true, 2 * 10 - 1, 20) == kBlack);  // left of the window
    CHECK(pixel(8, 4, true, 2 * 18, 20) == kBlack);        // past the window width
    CHECK(pixel(32, 16, false, 2 * 10, 20) == kBlack);     // Hide
    CHECK(pixel(0, 0, true, 0, 0) == kBlack);              // window never latched
    CHECK(pixel(0, 0, true, 2 * 10, 20) == kBlack);
    // No cartridge: the backdrop stays.
    CHECK(pixel(32, 16, true, 2 * 10, 20, /*attached=*/false) == kBackdropRed);
}

// With the VCD pixel clock each output tick averages its sample
// and the 3 before; the line starts from black, a window further right from its
// first pixel.
TEST_CASE("VCD pixel clock video is averaged over 4 ticks") {
    static uint8_t planes[2][0x80000] = {};
    static uint8_t video[32 * 16 * 4];
    auto fill = [&](bool step) {  // grey 200, or 0 left of pixel 16 with step
        for (std::size_t i = 0; i < sizeof(video); i += 4) {
            const uint8_t v = (step && (i / 4) % 32 < 16) ? 0 : 200;
            video[i] = v; video[i + 1] = v; video[i + 2] = v; video[i + 3] = 0xff;
        }
    };
    auto column = [&](uint16_t scrpos_x, int c) {
        Mcd212 mcd;
        mcd.set_dvc_attached(true);
        mcd.set_dvc_frame(video, 32, 16);
        mcd.set_dvc_window(scrpos_x, 20, 32, 16, 0, 0, /*show=*/true, /*vcd_pixel_clock=*/true);
        load_register(mcd, 0, kRegImageCodingMethod, kIcmEv);
        zcdi_test::run_field(mcd, planes[0], planes[1], sizeof(planes[0]));
        return fb_at(mcd, 2 * scrpos_x + c, 20);
    };
    // Sample 16 starts at tick 71 (71 * 231 / 1024); column c ends at tick 2c.
    fill(/*step=*/true);
    CHECK(column(10, 35) == 0x00000000u);
    CHECK(column(10, 36) == 0x00646464u);  // ticks 69-72: two of each
    CHECK(column(10, 37) == 0x00c8c8c8u);
    fill(/*step=*/false);
    CHECK(column(0, 0) == 0x00323232u);   // ticks -3..0: three black
    CHECK(column(0, 1) == 0x00969696u);
    CHECK(column(0, 2) == 0x00c8c8c8u);
    CHECK(column(10, 0) == 0x00c8c8c8u);  // offset wait holds the first pixel
}

// Every pixel starts from the backdrop, mixing or not.
TEST_CASE("mixing with both planes transparent shows the backdrop") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, 0);  // mixing enabled
    load_register(mcd, 0, kRegBackdropColor, 0xc);      // k4bppColor[0xc], red
    CHECK(priv<CompositePixel>(mcd, 0x00969696u, false, 0x00646464u, false, uint8_t(63), uint8_t(63)) ==
          0x00e61010u);
}

// Jeopardy needs it: inverted Color Key keeps a DYUV plane visible.
TEST_CASE("inverted Color Key on a DYUV plane is opaque") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, kTcrNotKey);
    load_register(mcd, 0, kRegTransparentColorA, 0x00fc0000u);
    load_register(mcd, 0, kRegImageCodingMethod, kIcmDyuv);
    CHECK_FALSE(priv<PlaneTransparent>(mcd, 0, 0x00123456u, 0));
    load_register(mcd, 0, kRegImageCodingMethod, kIcmClut7);  // transparent unless the key matches
    CHECK(priv<PlaneTransparent>(mcd, 0, 0x00123456u, 0));
    CHECK_FALSE(priv<PlaneTransparent>(mcd, 0, 0x00ff0000u, 0));
}

// IC clear makes a plane transparent. OFF or VSR 0 drives black that only Always or a region flag keys away.
TEST_CASE("a parked plane is black, transparent only with IC clear") {
    static uint8_t plane[0x80000] = {};
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, kTcrKey);  // plane A: Color Key
    load_register(mcd, 0, kRegTransparentColorA, 0x00101010u);
    load_register(mcd, 0, kRegImageCodingMethod, kIcmClut7);
    uint32_t row[kLineWidth];
    bool present[kLineWidth];
    write_mmio16(mcd, kDcr1, kDcrIc);  // VSR 0
    priv<DecodePlaneLine>(mcd, 0, plane, sizeof(plane), row, present);
    CHECK(row[0] == 0x00101010u);
    CHECK(present[0]);
    CHECK(priv<PlaneParked>(mcd, 0));
    write_mmio16(mcd, kDcr1, 0);  // IC clear
    write_mmio16(mcd, kVsr1, 0x100);
    priv<DecodePlaneLine>(mcd, 0, plane, sizeof(plane), row, present);
    CHECK_FALSE(present[0]);
}

}  // TEST_SUITE
