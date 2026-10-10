#include "doctest.h"

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

ZCDI_PRIVATE_MEMBER(StateClut, Mcd212SaveState, clut_, uint32_t[256]);
ZCDI_PRIVATE_MEMBER(StateCursorControl, Mcd212SaveState, cursor_control_, uint32_t);
ZCDI_PRIVATE_MEMBER(StateCursorPattern, Mcd212SaveState, cursor_pattern_, uint16_t[16]);

namespace {

bool same_frame(const Mcd212& a, const Mcd212& b) {
    const size_t n = size_t(a.fb_width()) * size_t(a.fb_height());
    for (size_t i = 0; i < n; ++i) {
        if (a.framebuffer()[i] != b.framebuffer()[i]) return false;
    }
    return true;
}

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("a restored MCD212 renders the same next frames") {
    std::vector<uint8_t> plane_a(0x80000), plane_b(0x80000);
    for (size_t i = 0; i < plane_a.size(); ++i) {
        plane_a[i] = uint8_t(i * 7);
        plane_b[i] = uint8_t(i * 13);
    }
    Mcd212 a;
    for (int i = 0; i < 256; ++i) priv<StateClut>(a)[i] = uint32_t(i * 0x010203);
    // Enabled (bit23), blinking on/off with 12-frame on and off periods,
    // colour 15.
    priv<StateCursorControl>(a) = 0x800000u | 0x080000u | 0x010000u | 0x000fu;
    for (uint16_t& row : priv<StateCursorPattern>(a)) row = 0xffff;
    for (int f = 0; f < 5; ++f) zcdi_test::run_field(a, plane_a.data(), plane_b.data(), plane_a.size());

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    Mcd212 b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::save_state(b) == state);

    // The frame buffer isn't saved; the next frame redraws all of it.
    for (int f = 0; f < 30; ++f) {
        CAPTURE(f);
        zcdi_test::run_field(a, plane_a.data(), plane_b.data(), plane_a.size());
        zcdi_test::run_field(b, plane_a.data(), plane_b.data(), plane_a.size());
        CHECK(same_frame(a, b));
    }
}

TEST_CASE("a restored MCD212 re-binds the DVC frame only when it still fits") {
    std::vector<uint8_t> rgba(16 * 8 * 4, 0x80);
    Mcd212 a;
    a.set_dvc_frame(rgba.data(), 16, 8);
    const std::vector<uint8_t> state = zcdi_test::save_state(a);

    Mcd212 b;
    REQUIRE(zcdi_test::load_state(b, state));
    b.rebind_dvc_frame(rgba.data(), rgba.size() - 1);  // too small: disabled
    CHECK(zcdi_test::save_state(b) != state);

    Mcd212 c;
    REQUIRE(zcdi_test::load_state(c, state));
    c.rebind_dvc_frame(rgba.data(), rgba.size());
    CHECK(zcdi_test::save_state(c) == state);
}

}  // TEST_SUITE
