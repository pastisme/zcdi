#include "doctest.h"

#include <cstddef>
#include <cstdint>

#include "mcd212/mcd212.h"
#include "support/mcd212_field.h"
#include "support/private_access.h"

using zcdi::mamecdi::Mcd212;
using zcdi::mamecdi::Mcd212SaveState;
using zcdi_test::priv;

ZCDI_PRIVATE_MEMBER(Clut, Mcd212SaveState, clut_, uint32_t[256]);
ZCDI_PRIVATE_MEMBER(Icm, Mcd212SaveState, icm_, uint8_t[2]);
ZCDI_PRIVATE_MEMBER(IcmCs, Mcd212SaveState, icm_cs_, bool);
ZCDI_PRIVATE_MEMBER(IcmEv, Mcd212SaveState, icm_ev_, bool);
ZCDI_PRIVATE_MEMBER(TransparencyControl, Mcd212SaveState, transparency_control_, uint32_t);
ZCDI_PRIVATE_MEMBER(TransparentColor, Mcd212SaveState, transparent_color_, uint32_t[2]);
ZCDI_PRIVATE_MEMBER(MaskColor, Mcd212SaveState, mask_color_, uint32_t[2]);
ZCDI_PRIVATE_MEMBER(PixelHold, Mcd212SaveState, pixel_hold_, uint32_t[2]);
ZCDI_PRIVATE_MEMBER(BackdropColor, Mcd212SaveState, backdrop_color_, uint8_t);
ZCDI_PRIVATE_MEMBER(CursorPosition, Mcd212SaveState, cursor_position_, uint32_t);
ZCDI_PRIVATE_MEMBER(CursorControl, Mcd212SaveState, cursor_control_, uint32_t);
ZCDI_PRIVATE_MEMBER(CursorPattern, Mcd212SaveState, cursor_pattern_, uint16_t[16]);
ZCDI_PRIVATE_MEMBER(ClutPixel, Mcd212, clut_pixel, uint32_t(int, uint8_t, uint8_t) const);
ZCDI_PRIVATE_MEMBER(PlaneTransparent, Mcd212, plane_transparent, bool(int, uint32_t, int) const);
ZCDI_PRIVATE_MEMBER(ApplyPixelHold, Mcd212, apply_pixel_hold, void(int, uint32_t*, bool*, int) const);
ZCDI_PRIVATE_MEMBER(DecodePlaneLine, Mcd212, decode_plane_line,
                    void(int, const uint8_t*, std::size_t, uint32_t*, bool*));
ZCDI_PRIVATE_MEMBER(PlaneParked, Mcd212, plane_parked, bool(int) const);
ZCDI_PRIVATE_MEMBER(DecodeDoubleResLine, Mcd212, decode_double_res_line,
                    void(int, const uint8_t*, std::size_t, uint32_t*, bool*));
ZCDI_PRIVATE_MEMBER(Vsr, Mcd212SaveState, vsr_, uint16_t[2]);
ZCDI_PRIVATE_MEMBER(Ddr, Mcd212SaveState, ddr_, uint16_t[2]);
ZCDI_PRIVATE_MEMBER(Dcr, Mcd212SaveState, dcr_, uint16_t[2]);
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

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("CM0=4 dual CLUT7 obeys ICM CS and CM1=4 is reserved") {
    Mcd212 mcd;
    priv<Clut>(mcd)[0x01] = 0x00112233u;
    priv<Clut>(mcd)[0x81] = 0x00445566u;
    priv<IcmCs>(mcd) = false;
    CHECK(priv<ClutPixel>(mcd, 0, 0x81, 0x4) == 0x00112233u);
    priv<IcmCs>(mcd) = true;
    CHECK(priv<ClutPixel>(mcd, 0, 0x81, 0x4) == 0x00445566u);
    CHECK(priv<ClutPixel>(mcd, 1, 0x81, 0x4) == 0);
}

TEST_CASE("CM1=1 is RGB555 and never standalone CLUT8") {
    Mcd212 mcd;
    priv<Clut>(mcd)[0x42] = 0x00abcdefu;
    CHECK(priv<ClutPixel>(mcd, 0, 0x42, 0x1) == 0x00abcdefu);
    CHECK(priv<ClutPixel>(mcd, 1, 0x42, 0x1) == 0x00101010u);
}

TEST_CASE("a Color-Key mask bit makes its RGB bit don't-care") {
    Mcd212 mcd;
    priv<Icm>(mcd)[0] = 0x1;
    priv<TransparencyControl>(mcd) = 0x000001u;  // TA: Color Key true
    priv<TransparentColor>(mcd)[0] = 0x00102030u;
    priv<MaskColor>(mcd)[0] = 0x00040000u;       // ignore one red bit
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
    priv<PixelHold>(mcd)[0] = 0x800002u;
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
    for (int i = 0; i < 16; ++i) priv<Clut>(mcd)[i] = 0x00010101u * uint32_t(i);
    priv<Icm>(mcd)[0] = 0x0b;
    priv<Dcr>(mcd)[0] = 0x0200;  // IC
    priv<Vsr>(mcd)[0] = 0x100;
    uint32_t row[kFbWidth] = {};
    bool opaque[kFbWidth] = {};

    SUBCASE("CLUT4 bitmap") {
        plane[0x100] = 0x12;
        plane[0x101] = 0xef;
        priv<DecodeDoubleResLine>(mcd, 0, plane, sizeof(plane), row, opaque);
        CHECK(row[0] == 0x00010101u);
        CHECK(row[1] == 0x00020202u);
        CHECK(row[2] == 0x000e0e0eu);
        CHECK(row[3] == 0x000f0f0fu);
        CHECK(priv<Vsr>(mcd)[0] == 0x100 + kLineWidth);  // one byte per Normal Resolution pixel
    }
    SUBCASE("RL3 runs and literals") {
        priv<Ddr>(mcd)[0] = 0x0200;  // run-length
        plane[0x100] = 0x9a;       // run of 0x1a ...
        plane[0x101] = 0x02;       // ... twice
        plane[0x102] = 0x3c;       // literal: bit 3 of the low pixel is dropped
        plane[0x103] = 0x80;       // run of 0 to end of line
        plane[0x104] = 0x00;
        priv<DecodeDoubleResLine>(mcd, 0, plane, sizeof(plane), row, opaque);
        CHECK(row[0] == 0x00010101u);
        CHECK(row[1] == 0x00020202u);
        CHECK(row[2] == 0x00010101u);
        CHECK(row[3] == 0x00020202u);
        CHECK(row[4] == 0x00030303u);
        CHECK(row[5] == 0x00040404u);
        CHECK(row[kFbWidth - 1] == 0);
        CHECK(priv<Vsr>(mcd)[0] == 0x105);
    }
}

// Green Book V.5.9.1: one +16 black-level pedestal over the summed terms.
TEST_CASE("two opaque planes mix with one black-level pedestal") {
    Mcd212 mcd;
    priv<TransparencyControl>(mcd) = 0;  // mixing enabled
    // ICF' 63 is a factor of 1: weighted_a = 150-16 = 134, weighted_b =
    // 100-16 = 84; sum = 134 + 84 + 16 = 234 = 0xea per channel.
    CHECK(priv<CompositePixel>(mcd, 0x00969696u, true, 0x00646464u, true, uint8_t(63), uint8_t(63)) ==
          0x00eaeaeau);
}

// Green Book: "transparent areas...are equivalent to black areas".
TEST_CASE("a transparent plane contributes nothing to the mix") {
    Mcd212 mcd;
    priv<TransparencyControl>(mcd) = 0;
    // weighted_a = 134; b's term is gated to 0 despite color_b being bright;
    // sum = 134 + 0 + 16 = 150 = 0x96 per channel.
    CHECK(priv<CompositePixel>(mcd, 0x00969696u, true, 0x00ffffffu, false, uint8_t(63), uint8_t(63)) ==
          0x00969696u);
}

// Green Book V.5.9: no per-plane clamp, only the mixed result is clipped.
TEST_CASE("below-black components contribute negatively to the mix") {
    Mcd212 mcd;
    priv<TransparencyControl>(mcd) = 0;
    CHECK(priv<CompositePixel>(mcd, 0x00202020u, true, 0x00020202u, true, uint8_t(63), uint8_t(63)) ==
          0x00121212u);
    // Full ICF shows a lone plane's decoded color unchanged, below black too.
    CHECK(priv<CompositePixel>(mcd, 0x00080808u, true, 0, false, uint8_t(63), uint8_t(63)) == 0x00080808u);
    // Clipped at 0, not wrapped.
    CHECK(priv<CompositePixel>(mcd, 0x00000000u, true, 0x00000000u, true, uint8_t(63), uint8_t(63)) == 0);
}

// mcd212.sv clears backdrop_pixel wherever a cursor pixel is visible. 16x16
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
        priv<IcmEv>(mcd) = true;
        priv<CursorControl>(mcd) = 0x800000u | (complementary ? 0x400000u : 0u) | kCursorBlink | 0xcu;
        priv<CursorPosition>(mcd) = (100u << 12) | 200u;  // X is in 768-wide units
        for (auto& row : priv<CursorPattern>(mcd)) row = 0xffffu;
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

// frameplayer.sv: black outside the latched window and before Show; cditop.sv
// shows it on every EV backdrop pixel while a DVC is attached.
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
        priv<BackdropColor>(mcd) = 0xc;
        mcd.set_dvc_attached(attached);
        mcd.set_dvc_frame(video, 32, 16);
        mcd.set_dvc_window(/*scrpos=*/10, 20, win_w, win_h, /*decoff=*/0, 0, show, /*vcd_pixel_clock=*/false);
        priv<IcmEv>(mcd) = true;
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

// mcd212.sv starts every pixel from the backdrop, mixing or not.
TEST_CASE("mixing with both planes transparent shows the backdrop") {
    Mcd212 mcd;
    priv<TransparencyControl>(mcd) = 0;  // mixing enabled
    priv<BackdropColor>(mcd) = 0xc;      // k4bppColor[0xc], red
    CHECK(priv<CompositePixel>(mcd, 0x00969696u, false, 0x00646464u, false, uint8_t(63), uint8_t(63)) ==
          0x00e61010u);
}

// mcd212.sv "Jeopardy hack": inverted Color Key keeps a DYUV plane visible.
TEST_CASE("inverted Color Key on a DYUV plane is opaque") {
    Mcd212 mcd;
    priv<TransparencyControl>(mcd) = 0x9;
    priv<TransparentColor>(mcd)[0] = 0x00ff0000u;
    priv<Icm>(mcd)[0] = 0x5;  // DYUV
    CHECK_FALSE(priv<PlaneTransparent>(mcd, 0, 0x00123456u, 0));
    priv<Icm>(mcd)[0] = 0x3;  // CLUT7: transparent unless the key matches
    CHECK(priv<PlaneTransparent>(mcd, 0, 0x00123456u, 0));
    CHECK_FALSE(priv<PlaneTransparent>(mcd, 0, 0x00ff0000u, 0));
}

// mcd212.sv: IC clear makes a plane transparent. same_cdi (Hotel Mario on a
// console): OFF or VSR 0 drives black that only Always or a region flag keys away.
TEST_CASE("a parked plane is black, transparent only with IC clear") {
    static uint8_t plane[0x80000] = {};
    Mcd212 mcd;
    priv<TransparencyControl>(mcd) = 0x1;  // plane A: Color Key
    priv<TransparentColor>(mcd)[0] = 0x00101010u;
    priv<Icm>(mcd)[0] = 0x3;               // CLUT7
    uint32_t row[kLineWidth];
    bool present[kLineWidth];
    priv<Dcr>(mcd)[0] = 0x0200;  // IC, VSR 0
    priv<DecodePlaneLine>(mcd, 0, plane, sizeof(plane), row, present);
    CHECK(row[0] == 0x00101010u);
    CHECK(present[0]);
    CHECK(priv<PlaneParked>(mcd, 0));
    priv<Dcr>(mcd)[0] = 0;  // IC clear
    priv<Vsr>(mcd)[0] = 0x100;
    priv<DecodePlaneLine>(mcd, 0, plane, sizeof(plane), row, present);
    CHECK_FALSE(present[0]);
}

}  // TEST_SUITE
