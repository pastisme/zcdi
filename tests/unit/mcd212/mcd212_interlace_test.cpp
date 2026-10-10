// DCR1.SM interlace: CDi_MiSTer video_timing.sv (parity, line counts),
// ica_dca_ctrl.sv (ICA at 0x404 in even fields), mcd212.sv ica_parity.
#include "doctest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mcd212/mcd212.h"
#include "support/mcd212_field.h"
#include "support/private_access.h"
#include "support/state_roundtrip.h"

using zcdi::mamecdi::Mcd212;
using zcdi::mamecdi::Mcd212SaveState;
using zcdi_test::priv;
using zcdi_test::run_field;

ZCDI_PRIVATE_MEMBER(InterlaceDcr, Mcd212SaveState, dcr_, uint16_t[2]);
ZCDI_PRIVATE_MEMBER(InterlaceIcmEv, Mcd212SaveState, icm_ev_, bool);

namespace {

constexpr uint32_t kCsr2R = 0x01, kCsr1R = 0x11;
constexpr uint16_t kIc = 0x0200, kSm = 0x1000;
constexpr int kPalLines = 312, kNtscLines = 262;
constexpr int kLineWidth = 384, kFbWidth = 768, kFbHeight = 280;

struct Planes {
    std::vector<uint8_t> a = std::vector<uint8_t>(0x80000), b = std::vector<uint8_t>(0x80000);
    // ICA: STOP at 0x400, INTERRUPT then STOP at 0x404.
    Planes() {
        a[0x404] = 0x60;
        a[0x408] = 0x00;
    }
};

int field(Mcd212& mcd, Planes& p, int lines = kPalLines) {
    return run_field(mcd, p.a.data(), p.b.data(), p.a.size(), lines);
}

// IT1, cleared by the read: the 0x404 list ran.
bool even_list_ran(Mcd212& mcd) { return (mcd.read_byte(kCsr2R) & 0x04) != 0; }
bool pa(Mcd212& mcd) { return (mcd.read_byte(kCsr1R) & 0x20) != 0; }

uint32_t pixel(const Mcd212& mcd, int x, int row) {
    return mcd.framebuffer()[static_cast<std::size_t>(row) * kFbWidth + x];
}

struct Pictures {
    std::vector<uint8_t> red = std::vector<uint8_t>(kLineWidth * kFbHeight * 4), blue = red;
    Pictures() {
        for (std::size_t i = 0; i < red.size(); i += 4) {
            red[i] = 0xc8;
            blue[i + 2] = 0xc8;
            red[i + 3] = blue[i + 3] = 0xff;
        }
    }
};
constexpr uint32_t kRed = 0x00c80000u, kBlue = 0x000000c8u;

// The whole picture is DVC, so each field's colour is its frame's.
void show_dvc(Mcd212& mcd) {
    mcd.set_dvc_attached(true);
    mcd.set_dvc_window(0, 0, kLineWidth, kFbHeight, 0, 0, /*show=*/true, /*vcd_pixel_clock=*/false);
    priv<InterlaceIcmEv>(mcd) = true;
}

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("without SM every field is odd: same line count, ICA at 0x400, PA toggling") {
    Planes p;
    Mcd212 mcd;
    priv<InterlaceDcr>(mcd)[0] = kIc;
    bool last_pa = pa(mcd);
    for (int f = 0; f < 4; ++f) {
        CAPTURE(f);
        CHECK(field(mcd, p) == kPalLines);
        CHECK_FALSE(even_list_ran(mcd));
        CHECK(pa(mcd) != last_pa);
        last_pa = pa(mcd);
        CHECK(mcd.output_height() == kFbHeight);
    }
}

TEST_CASE("with SM, fields alternate odd and even") {
    Planes p;
    Mcd212 mcd;
    priv<InterlaceDcr>(mcd)[0] = kIc | kSm;
    CHECK(field(mcd, p) == kPalLines);  // odd
    CHECK_FALSE(even_list_ran(mcd));
    for (int pair = 0; pair < 3; ++pair) {
        CAPTURE(pair);
        CHECK(field(mcd, p) == kPalLines + 1);  // even: the half line
        CHECK(even_list_ran(mcd));
        CHECK(pa(mcd));  // set after an even field, read during the odd one
        CHECK(field(mcd, p) == kPalLines);
        CHECK_FALSE(even_list_ran(mcd));
        CHECK_FALSE(pa(mcd));
    }
    Mcd212 ntsc;
    priv<InterlaceDcr>(ntsc)[0] = kSm;
    CHECK(field(ntsc, p, kNtscLines) == kNtscLines);
    CHECK(field(ntsc, p, kNtscLines) == kNtscLines + 1);
}

TEST_CASE("clearing SM in an even field ends it a line late, then odd fields only") {
    Planes p;
    Mcd212 mcd;
    priv<InterlaceDcr>(mcd)[0] = kIc | kSm;
    field(mcd, p);
    priv<InterlaceDcr>(mcd)[0] = kIc;
    CHECK(field(mcd, p) == kPalLines + 1);
    CHECK_FALSE(even_list_ran(mcd));  // ica_parity = !sm | parity
    for (int f = 0; f < 3; ++f) CHECK(field(mcd, p) == kPalLines);
}

TEST_CASE("interlaced fields are woven, the first one line-doubled") {
    Planes p;
    Pictures pic;
    const auto& red = pic.red;
    const auto& blue = pic.blue;
    Mcd212 mcd;
    show_dvc(mcd);

    mcd.set_dvc_frame(red.data(), kLineWidth, kFbHeight);
    field(mcd, p);
    CHECK(mcd.output_height() == kFbHeight);
    CHECK(pixel(mcd, 100, 100) == kRed);

    priv<InterlaceDcr>(mcd)[0] = kSm;
    mcd.set_dvc_frame(blue.data(), kLineWidth, kFbHeight);
    field(mcd, p);  // odd, first woven: both rows
    CHECK(mcd.output_height() == 2 * kFbHeight);
    CHECK(pixel(mcd, 100, 200) == kBlue);
    CHECK(pixel(mcd, 100, 201) == kBlue);

    mcd.set_dvc_frame(red.data(), kLineWidth, kFbHeight);
    field(mcd, p);  // even: odd rows only
    CHECK(pixel(mcd, 100, 200) == kBlue);
    CHECK(pixel(mcd, 100, 201) == kRed);
    CHECK(pixel(mcd, 100, 2 * kFbHeight - 1) == kRed);

    priv<InterlaceDcr>(mcd)[0] = 0;
    field(mcd, p);
    CHECK(mcd.output_height() == kFbHeight);
    CHECK(pixel(mcd, 100, 201) == kRed);
}

TEST_CASE("a restored MCD212 continues with the same field parity") {
    Planes p;
    Mcd212 a;
    priv<InterlaceDcr>(a)[0] = kIc | kSm;
    field(a, p);  // the next one is even
    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    Mcd212 b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(field(b, p) == kPalLines + 1);
    CHECK(even_list_ran(b));
}

TEST_CASE("a restored MCD212 keeps the woven rows the next field doesn't draw") {
    Planes p;
    Pictures pic;
    Mcd212 a;
    show_dvc(a);
    priv<InterlaceDcr>(a)[0] = kSm;
    a.set_dvc_frame(pic.blue.data(), kLineWidth, kFbHeight);
    field(a, p);  // odd, first woven
    a.set_dvc_frame(pic.red.data(), kLineWidth, kFbHeight);
    field(a, p);  // even: odd rows red
    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    Mcd212 b;
    show_dvc(b);
    REQUIRE(zcdi_test::load_state(b, state));
    for (Mcd212* m : {&a, &b}) {
        m->set_dvc_frame(pic.blue.data(), kLineWidth, kFbHeight);
        field(*m, p);  // odd: even rows blue
    }
    CHECK(pixel(b, 100, 200) == kBlue);
    CHECK(pixel(b, 100, 201) == kRed);
    const std::size_t n = static_cast<std::size_t>(kFbWidth) * 2 * kFbHeight;
    CHECK(std::equal(a.framebuffer(), a.framebuffer() + n, b.framebuffer()));
}

}  // TEST_SUITE
