// MCD212 line decoding (Green Book V.4), Region Control (V.5.10) and the
// mixer's per-plane paths (V.5.9), each checked on its own.
#include "doctest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "chips/mcd212/mcd212.h"
#include "support/mcd212_field.h"
#include "support/mcd212_private.h"
#include "support/mcd212_regs.h"

using zcdi::chips::Mcd212;
using zcdi_test::load_clut;
using zcdi_test::load_register;
using zcdi_test::pixel;
using zcdi_test::run_field;
using zcdi_test::write_mmio16;
using namespace zcdi::chips::mcd212;

namespace {

constexpr int kLineWidth = 384, kFbWidth = 768;
constexpr uint32_t kLine = 0x1000;  // where plane A's line starts
// Transparency Control's "never": "always", inverted. 0, the power-on value, hides the plane.
constexpr uint32_t kNever = kTcrAlways | kTcrInvert;

struct Planes {
    std::vector<uint8_t> a = std::vector<uint8_t>(0x80000), b = std::vector<uint8_t>(0x80000);
};

// A distinct colour per CLUT entry, 6 bits per channel.
uint32_t clut_color(int n) { return uint32_t((n & 0x3f) << 2) | uint32_t(((n >> 6) & 3) << 10); }

void fill_clut(Mcd212& mcd) {
    for (int n = 0; n < 256; ++n) load_clut(mcd, n / 64, n % 64, clut_color(n));
}

uint32_t grey(int y) { return uint32_t(y) * 0x010101u; }

uint32_t vsr(const Mcd212& mcd, int path) {
    return uint32_t(mcd.state().dcr_[path] & 0x3f) << 16 | mcd.state().vsr_[path];
}

// Plane A on, coded `icm`, its line at `address`, file type and repeat in `ddr`.
Mcd212 plane_a(uint8_t icm, uint32_t address = kLine, uint16_t ddr = 0) {
    Mcd212 mcd;
    fill_clut(mcd);
    load_register(mcd, 0, kRegImageCodingMethod, icm);
    write_mmio16(mcd, kDcr1, uint16_t(kDcrIc | ((address >> 16) & 0x3f)));
    write_mmio16(mcd, kVsr1, uint16_t(address));
    write_mmio16(mcd, kDdr1, ddr);
    return mcd;
}

struct Line {
    uint32_t row[kLineWidth] = {};
    bool present[kLineWidth] = {};
};

Line decode(Mcd212& mcd, Planes& p) {
    Line line;
    call_private::decode_plane_line(mcd, 0, p.a.data(), p.a.size(), line.row, line.present);
    return line;
}

uint16_t ddr_file_type(uint8_t type, int repeat_code = 0) {
    return uint16_t(type << kDdrFtShift | repeat_code << kDdrMtShift);
}

// Region Control entry.
uint32_t rc(uint32_t op, int x, int weight = 0, int flag = 0) {
    return op << kRcOpShift | uint32_t(flag) << kRcRfBit | uint32_t(weight) << kRcWfShift | uint32_t(x);
}

void load_regions(Mcd212& mcd, std::initializer_list<uint32_t> entries) {
    int i = 0;
    for (uint32_t e : entries) load_register(mcd, 0, uint8_t(kRegRegionControl + i++), e);
}

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("a CLUT8 line is one byte per pixel and moves VSR on by the line") {
    Planes p;
    for (int i = 0; i < kLineWidth; ++i) p.a[kLine + i] = uint8_t(i * 7);
    Mcd212 mcd = plane_a(kIcmClut8);
    const Line line = decode(mcd, p);
    for (int i = 0; i < kLineWidth; ++i) {
        CAPTURE(i);
        CHECK(line.row[i] == clut_color(uint8_t(i * 7)));
        CHECK(line.present[i]);
    }
    CHECK(vsr(mcd, 0) == kLine + kLineWidth);
}

TEST_CASE("a line wraps at the end of plane memory") {
    Planes p;
    p.a[0x7fffe] = 1;
    p.a[0x7ffff] = 2;
    p.a[0] = 3;
    Mcd212 mcd = plane_a(kIcmClut8, 0x7fffe);
    const Line line = decode(mcd, p);
    CHECK(line.row[0] == clut_color(1));
    CHECK(line.row[1] == clut_color(2));
    CHECK(line.row[2] == clut_color(3));
}

// Green Book V.5.11.1.2: codes are repeated before decoding.
TEST_CASE("Pixel Repeat shows each code 2, 4, 8 or 16 times and fetches that many fewer") {
    Planes p;
    for (int i = 0; i < kLineWidth; ++i) p.a[kLine + i] = uint8_t(i + 1);
    for (int code = 0; code < 4; ++code) {
        const int factor = 2 << code;
        CAPTURE(factor);
        Mcd212 mcd = plane_a(kIcmClut8, kLine, ddr_file_type(kDdrFtMosaic, code));
        const Line line = decode(mcd, p);
        for (int i = 0; i < kLineWidth; ++i) REQUIRE(line.row[i] == clut_color(i / factor + 1));
        CHECK(vsr(mcd, 0) == kLine + uint32_t(kLineWidth / factor));
    }
}

TEST_CASE("DYUV: zero deltas hold the start value, and neutral chroma is grey") {
    Planes p;
    Mcd212 mcd = plane_a(kIcmDyuv);
    load_register(mcd, 0, kRegDyuvAbsStartA, 0x508080);
    const Line line = decode(mcd, p);
    for (int i = 0; i < kLineWidth; ++i) REQUIRE(line.row[i] == grey(0x50));
}

TEST_CASE("DYUV: Y deltas accumulate along the line, modulo 256") {
    Planes p;
    std::fill(p.a.begin() + kLine, p.a.begin() + kLine + kLineWidth, uint8_t(0x01));  // +1 per pixel
    Mcd212 up = plane_a(kIcmDyuv);
    load_register(up, 0, kRegDyuvAbsStartA, 0x108080);
    Line line = decode(up, p);
    for (int x = 0; x < 200; ++x) REQUIRE(line.row[x] == grey(0x10 + x + 1));

    std::fill(p.a.begin() + kLine, p.a.begin() + kLine + kLineWidth, uint8_t(0x0f));  // delta 255: -1
    Mcd212 down = plane_a(kIcmDyuv);
    load_register(down, 0, kRegDyuvAbsStartA, 0x028080);
    line = decode(down, p);
    CHECK(line.row[0] == grey(1));
    CHECK(line.row[1] == grey(0));
    CHECK(line.row[2] == grey(255));
}

TEST_CASE("DYUV: U moves blue against green, V red against green, and a pair's first pixel takes the mean") {
    Planes p;
    p.a[kLine] = 0x40;      // dU = 16
    p.a[kLine + 1] = 0x00;
    p.a[kLine + 2] = 0x00;
    p.a[kLine + 3] = 0x40;  // dV = 16
    Mcd212 mcd = plane_a(kIcmDyuv);
    load_register(mcd, 0, kRegDyuvAbsStartA, 0x808080);
    const Line line = decode(mcd, p);
    auto r = [&](int x) { return int(line.row[x] >> 16 & 0xff); };
    auto g = [&](int x) { return int(line.row[x] >> 8 & 0xff); };
    auto b = [&](int x) { return int(line.row[x] & 0xff); };

    CHECK(r(1) == 0x80);
    CHECK(b(1) > 0x80);
    CHECK(g(1) < 0x80);
    CHECK(b(0) > 0x80);
    CHECK(b(0) < b(1));  // U halfway

    CHECK(b(3) == b(1));  // U holds
    CHECK(r(3) > 0x80);
    CHECK(r(2) > 0x80);
    CHECK(r(2) < r(3));  // V halfway
}

TEST_CASE("DYUV: each line restarts from the start value, clips, and ignores Pixel Repeat") {
    Planes p;
    for (int i = 0; i < kLineWidth; ++i) p.a[kLine + i] = uint8_t(i);
    Mcd212 mcd = plane_a(kIcmDyuv);
    load_register(mcd, 0, kRegDyuvAbsStartA, 0x808080);
    const Line first = decode(mcd, p);
    write_mmio16(mcd, kVsr1, uint16_t(kLine));
    const Line again = decode(mcd, p);
    for (int i = 0; i < kLineWidth; ++i) REQUIRE(first.row[i] == again.row[i]);

    Mcd212 repeated = plane_a(kIcmDyuv, kLine, ddr_file_type(kDdrFtMosaic, 1));
    load_register(repeated, 0, kRegDyuvAbsStartA, 0x808080);
    const Line mosaic = decode(repeated, p);
    for (int i = 0; i < kLineWidth; ++i) REQUIRE(mosaic.row[i] == first.row[i]);

    std::fill(p.a.begin() + kLine, p.a.begin() + kLine + kLineWidth, uint8_t(0));
    p.a[kLine + 1] = 0x70;  // dV = 79 on Y 250: red past 255
    Mcd212 bright = plane_a(kIcmDyuv);
    load_register(bright, 0, kRegDyuvAbsStartA, 0xfa8080);
    CHECK((decode(bright, p).row[1] >> 16) == 0xff);
}

TEST_CASE("RL7: a byte with bit 7 clear is one pixel, set is a run of the next byte's length, 0 to the end") {
    Planes p;
    const uint8_t data[] = {0x05, 0x83, 0x04, 0x81, 0x00, 0x09};
    std::copy(std::begin(data), std::end(data), p.a.begin() + kLine);
    Mcd212 mcd = plane_a(kIcmClut7, kLine, ddr_file_type(kDdrFtRle));
    const Line line = decode(mcd, p);
    CHECK(line.row[0] == clut_color(5));
    for (int x = 1; x < 5; ++x) CHECK(line.row[x] == clut_color(3));
    for (int x = 5; x < kLineWidth; ++x) REQUIRE(line.row[x] == clut_color(1));
    CHECK(vsr(mcd, 0) == kLine + 5);
}

TEST_CASE("RL7: a run past the line's end stops there") {
    Planes p;
    const uint8_t data[] = {0x82, 0xff, 0x84, 0xff, 0x09};
    std::copy(std::begin(data), std::end(data), p.a.begin() + kLine);
    Mcd212 mcd = plane_a(kIcmClut7, kLine, ddr_file_type(kDdrFtRle));
    const Line line = decode(mcd, p);
    CHECK(line.row[254] == clut_color(2));
    CHECK(line.row[255] == clut_color(4));
    CHECK(line.row[kLineWidth - 1] == clut_color(4));
    CHECK(vsr(mcd, 0) == kLine + 4);
}

TEST_CASE("RL7 and RL3 leave the pixels past a 360-pixel line black and absent") {
    Planes p;
    p.a[kLine] = 0x81;  // run of 1 to the end
    p.a[kLine + 1] = 0x00;
    Mcd212 rl7 = plane_a(kIcmClut7, kLine, ddr_file_type(kDdrFtRle));
    rl7.begin_field(312);  // no CF: 360 pixels
    const Line line = decode(rl7, p);
    CHECK(line.row[359] == clut_color(1));
    CHECK(line.row[360] == kStudioBlack);
    CHECK_FALSE(line.present[360]);

    Mcd212 rl3 = plane_a(kIcmClut4, kLine, ddr_file_type(kDdrFtRle));
    rl3.begin_field(312);
    uint32_t row[kFbWidth] = {};
    bool present[kFbWidth] = {};
    call_private::decode_double_res_line(rl3, 0, p.a.data(), p.a.size(), row, present);
    CHECK(row[2 * 359 + 1] == clut_color(1));
    CHECK(row[2 * 360] == kStudioBlack);
    CHECK_FALSE(present[2 * 360]);
}

TEST_CASE("a plane preview decodes lines from any address without moving VSR") {
    Planes p;
    for (int i = 0; i < 2 * kLineWidth; ++i) p.a[0x2000 + i] = uint8_t(i);
    Mcd212 mcd = plane_a(kIcmClut8);
    std::vector<uint32_t> out(2 * kFbWidth);
    mcd.preview_plane(0, 0x2000, 2, p.a.data(), p.a.size(), out.data());
    CHECK(out[0] == clut_color(0));
    CHECK(out[1] == clut_color(0));
    CHECK(out[2] == clut_color(1));
    CHECK(out[kFbWidth] == clut_color(uint8_t(kLineWidth)));
    CHECK(vsr(mcd, 0) == kLine);

    Mcd212 clut4 = plane_a(kIcmClut4);
    p.a[0x2000] = 0x12;
    clut4.preview_plane(0, 0x2000, 1, p.a.data(), p.a.size(), out.data());
    CHECK(out[0] == clut_color(1));  // Double Resolution: a nibble per column
    CHECK(out[1] == clut_color(2));
}

// Green Book V.4.4.4, V.4.6.1: plane A holds T, R and G's top bits; plane B the rest.
TEST_CASE("RGB555 joins the two planes' bytes into one plane B pixel") {
    Planes p;
    std::fill(p.a.begin(), p.a.end(), uint8_t(0x55));  // T 0, R 10101, G 01...
    std::fill(p.b.begin(), p.b.end(), uint8_t(0x6c));  // ...G 011, B 01100
    Mcd212 mcd;
    load_register(mcd, 0, kRegImageCodingMethod, uint32_t(kIcmRgb555) << 8);
    load_register(mcd, 1, kRegWeightFactorB, 63);
    load_register(mcd, 0, kRegTransparencyControl, kNever << kTcrPlaneBShift);
    write_mmio16(mcd, kDcr1, kDcrDe);
    run_field(mcd, p.a.data(), p.b.data(), p.a.size());
    CHECK(pixel(mcd, 100, 100) == 0x00a85860u);

    // T set, and TCR B "RGB": transparent, the backdrop shows.
    std::fill(p.a.begin(), p.a.end(), uint8_t(0xd5));
    load_register(mcd, 0, kRegTransparencyControl, uint32_t(kTcrRgb) << kTcrPlaneBShift);
    load_register(mcd, 0, kRegBackdropColor, 0xc);
    run_field(mcd, p.a.data(), p.b.data(), p.a.size());
    CHECK(pixel(mcd, 100, 100) == k4bppColor[0xc]);
}

TEST_CASE("RGB555 transparency: each TCR B condition, and its inverse") {
    Mcd212 mcd;
    load_regions(mcd, {rc(kRcOpSetRf, 100, 0, 0), rc(kRcOpSetRf, 200, 0, 1)});
    call_private::update_region_arrays(mcd);
    struct Case {
        uint32_t condition;
        bool t0_x0, t1_x0, t0_x150, t0_x250;  // (T bit, column)
    };
    const Case cases[] = {
        {kTcrAlways, true, true, true, true},     {kTcrKey, false, false, false, false},
        {kTcrRgb, false, true, false, false},     {kTcrRf0, false, false, true, true},
        {kTcrRf1, false, false, false, true},     {kTcrRf0Key, false, false, true, true},
        {kTcrRf1Key, false, false, false, true},  {0x7, false, false, false, false},
    };
    for (const Case& c : cases) {
        for (bool invert : {false, true}) {
            CAPTURE(c.condition);
            CAPTURE(invert);
            load_register(mcd, 0, kRegTransparencyControl,
                          (c.condition | (invert ? kTcrInvert : 0u)) << kTcrPlaneBShift);
            CHECK(call_private::rgb555_transparent(mcd, 0, false) == (c.t0_x0 != invert));
            CHECK(call_private::rgb555_transparent(mcd, 0, true) == (c.t1_x0 != invert));
            CHECK(call_private::rgb555_transparent(mcd, 150, false) == (c.t0_x150 != invert));
            CHECK(call_private::rgb555_transparent(mcd, 250, false) == (c.t0_x250 != invert));
        }
    }
}

TEST_CASE("a CLUT plane's region-flag conditions follow the flags, with or without the colour key") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegImageCodingMethod, kIcmClut8);
    load_register(mcd, 0, kRegTransparentColorA, 0x00fc0000u);
    load_regions(mcd, {rc(kRcOpSetRf, 100, 0, 0), rc(kRcOpSetRf, 200, 0, 1)});
    call_private::update_region_arrays(mcd);
    const uint32_t red = 0x00fc0000u, blue = 0x000000fcu;
    load_register(mcd, 0, kRegTransparencyControl, kTcrRf0);
    CHECK_FALSE(call_private::plane_transparent(mcd, 0, red, 50));
    CHECK(call_private::plane_transparent(mcd, 0, blue, 150));
    load_register(mcd, 0, kRegTransparencyControl, kTcrRf1);
    CHECK_FALSE(call_private::plane_transparent(mcd, 0, blue, 150));
    CHECK(call_private::plane_transparent(mcd, 0, blue, 250));
    load_register(mcd, 0, kRegTransparencyControl, kTcrRf0Key);
    CHECK(call_private::plane_transparent(mcd, 0, red, 50));
    CHECK_FALSE(call_private::plane_transparent(mcd, 0, blue, 50));
    CHECK(call_private::plane_transparent(mcd, 0, blue, 150));
    load_register(mcd, 0, kRegTransparencyControl, kTcrRf1Key | kTcrInvert);
    CHECK(call_private::plane_transparent(mcd, 0, blue, 150));
    CHECK_FALSE(call_private::plane_transparent(mcd, 0, red, 150));
    CHECK_FALSE(call_private::plane_transparent(mcd, 0, blue, 250));
}

// Green Book V.5.10.2. Without NR, one list of up to 8 entries in column order.
TEST_CASE("Region Control sets weights and flags from each entry's column on") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegWeightFactorA, 63);
    load_register(mcd, 1, kRegWeightFactorB, 62);
    load_regions(mcd, {
        rc(kRcOpWfA, 100, 10),
        rc(kRcOpWfB, 150, 20),
        rc(kRcOpSetRf, 200, 0, 0),
        rc(kRcOpSetRf, 250, 0, 1),
        rc(kRcOpResetRf, 300, 0, 0),
        rc(kRcOpWfASetRf, 350, 30, 0),
        rc(kRcOpWfBResetRf, 400, 40, 1),
        rc(kRcOpWfAResetRf, 450, 50, 0),
    });
    call_private::update_region_arrays(mcd);
    const zcdi_test::RegionWeights& w = access_private::weight_factor_line_(mcd);
    const zcdi_test::RegionFlags& f = access_private::region_flag_line_(mcd);
    const struct {
        int x;
        int wa, wb;
        bool rf0, rf1;
    } expected[] = {
        {0, 63, 62, false, false},   {99, 63, 62, false, false},  {100, 10, 62, false, false},
        {150, 10, 20, false, false}, {200, 10, 20, true, false},  {250, 10, 20, true, true},
        {300, 10, 20, false, true},  {350, 30, 20, true, true},   {400, 30, 40, true, false},
        {450, 50, 40, false, false}, {767, 50, 40, false, false},
    };
    for (const auto& e : expected) {
        CAPTURE(e.x);
        CHECK(w[0][e.x] == e.wa);
        CHECK(w[1][e.x] == e.wb);
        CHECK(f[0][e.x] == e.rf0);
        CHECK(f[1][e.x] == e.rf1);
    }
    CHECK(access_private::region_pairs_uniform_(mcd));
}

TEST_CASE("without NR, an entry left of the one before, or op 0, ends the list") {
    Mcd212 mcd;
    load_regions(mcd, {rc(kRcOpSetRf, 300, 0, 0), rc(kRcOpSetRf, 200, 0, 1), rc(kRcOpSetRf, 400, 0, 1)});
    call_private::update_region_arrays(mcd);
    const zcdi_test::RegionFlags& f = access_private::region_flag_line_(mcd);
    CHECK(f[0][300]);
    CHECK_FALSE(f[1][500]);

    load_regions(mcd, {rc(kRcOpSetRf, 100, 0, 0), rc(kRcOpEnd, 200), rc(kRcOpSetRf, 300, 0, 1)});
    call_private::update_region_arrays(mcd);
    CHECK(f[0][100]);
    CHECK_FALSE(f[1][500]);
}

// With NR, entries 0-3 drive flag 0 and 4-7 flag 1, in any column order.
TEST_CASE("with NR, each flag has its own list of four, in any order") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegImageCodingMethod, uint32_t(kIcmNr) | kIcmClut8);
    load_register(mcd, 0, kRegRegionControl + 0, rc(kRcOpResetRf, 300));
    load_register(mcd, 0, kRegRegionControl + 1, rc(kRcOpSetRf, 200));
    load_register(mcd, 0, kRegRegionControl + 4, rc(kRcOpWfBSetRf, 101, 33));
    call_private::update_region_arrays(mcd);
    const zcdi_test::RegionWeights& w = access_private::weight_factor_line_(mcd);
    const zcdi_test::RegionFlags& f = access_private::region_flag_line_(mcd);
    CHECK_FALSE(f[0][199]);
    CHECK(f[0][200]);
    CHECK_FALSE(f[0][300]);
    CHECK_FALSE(f[1][100]);
    CHECK(f[1][101]);
    CHECK(w[1][101] == 33);
    CHECK_FALSE(access_private::region_pairs_uniform_(mcd));  // column 101 splits a pixel
}

TEST_CASE("a weight change at an odd column splits a Normal Resolution pixel in the picture") {
    Planes p;
    std::fill(p.a.begin() + kLine, p.a.end(), uint8_t(37));
    Mcd212 mcd = plane_a(kIcmClut8);
    load_register(mcd, 0, kRegWeightFactorA, 63);
    load_register(mcd, 0, kRegTransparencyControl, kNever);
    load_regions(mcd, {rc(kRcOpWfA, 101, 0)});
    write_mmio16(mcd, kDcr1, uint16_t(kDcrDe | kDcrIc));
    run_field(mcd, p.a.data(), p.b.data(), p.a.size());
    CHECK(pixel(mcd, 100, 0) == clut_color(37));
    CHECK(pixel(mcd, 101, 0) == kStudioBlack);  // weight 0: black
}

// Green Book V.5.9: ICF'/63 times (C - 16), rounded, plus 16; no clamp before the sum.
TEST_CASE("with mixing off, a lone plane is weighted on its own") {
    Mcd212 mcd;
    load_register(mcd, 0, kRegTransparencyControl, kTcrDisableMx);
    CHECK(call_private::composite_pixel(mcd, 0x00969696u, true, 0, false, uint8_t(32), uint8_t(63)) == 0x00545454u);
    CHECK(call_private::composite_pixel(mcd, 0x00080808u, true, 0, false, uint8_t(32), uint8_t(63)) == 0x000c0c0cu);
    CHECK(call_private::composite_pixel(mcd, 0x00969696u, true, 0, false, uint8_t(63), uint8_t(63)) == 0x00969696u);
}

TEST_CASE("a CLUT4 plane renders at Double Resolution") {
    Planes p;
    std::fill(p.a.begin() + kLine, p.a.end(), uint8_t(0x12));
    Mcd212 mcd = plane_a(kIcmClut4);
    load_register(mcd, 0, kRegWeightFactorA, 63);
    load_register(mcd, 0, kRegTransparencyControl, kNever);
    write_mmio16(mcd, kDcr1, uint16_t(kDcrDe | kDcrIc));
    run_field(mcd, p.a.data(), p.b.data(), p.a.size());
    CHECK(pixel(mcd, 200, 10) == clut_color(1));
    CHECK(pixel(mcd, 201, 10) == clut_color(2));
}

// Periods are in units of 12 frames.
TEST_CASE("a blinking cursor stays on for CON periods and off for COF") {
    Planes p;
    Mcd212 mcd;
    load_register(mcd, 0, kRegCursorControl, kCurcntEn | 2u << kCurcntConShift | 1u << kCurcntCofShift);
    auto off_after = [&](int fields) {
        while (fields-- > 0) run_field(mcd, p.a.data(), p.b.data(), p.a.size());
        return mcd.state().cursor_blink_state_;
    };
    CHECK_FALSE(off_after(23));
    CHECK(off_after(1));   // field 24
    CHECK(off_after(11));  // field 35
    CHECK_FALSE(off_after(1));
}

}  // TEST_SUITE
