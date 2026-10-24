// Philips "Technical Info Pointing Devices" (1990); CDi_MiSTer rtl/pointing_device.sv.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "core/pointing_device.h"
#include "scc68070/scc68070.h"
#include "support/state_roundtrip.h"

using zcdi::DeviceId;
using zcdi::PointingDevice;

namespace {

constexpr int kByteCycles = zcdi::mamecdi::kCpuClockHz / 120;
constexpr int kChunk = 1000;

// The bytes the device sends in `byte_times` byte times with RTS asserted.
std::vector<int> run(PointingDevice& p, int byte_times) {
    std::vector<int> out;
    for (int c = 0; c < byte_times * kByteCycles; c += kChunk) {
        const int b = p.tick(kChunk, false);
        if (b >= 0) out.push_back(b);
    }
    return out;
}

// Releases RTS and takes the ID and the packet that follows it.
void identify(PointingDevice& p) {
    for (int i = 0; i < 10; ++i) p.tick(kChunk, true);
    run(p, 1 + (p.absolute() ? 0 : 3));
}

}  // namespace

TEST_SUITE("core") {

TEST_CASE("the ID comes once RTS is asserted, then a packet with the buttons") {
    PointingDevice p(DeviceId::kManeuvering);
    for (int i = 0; i < 1000; ++i) CHECK(p.tick(kChunk, true) == -1);
    CHECK(run(p, 1) == std::vector<int>{'J' | 0x80});
    CHECK(run(p, 3) == std::vector<int>{0xc0, 0x80, 0x80});
    CHECK(run(p, 6).empty());
}

TEST_CASE("without RTS held, the ID comes after the power-on delay") {
    PointingDevice p(DeviceId::kRelative);
    CHECK(run(p, 11).empty());  // 100 ms is 12 byte times
    CHECK(run(p, 1) == std::vector<int>{'M' | 0x80});
}

TEST_CASE("a pad sends while held and once when it stops") {
    PointingDevice p(DeviceId::kManeuvering);
    identify(p);
    p.set_input({.right = true, .button1 = true});
    // Speed N ramps 2, 2, 4 per packet; one packet every 3 byte times.
    CHECK(run(p, 9) == std::vector<int>{0xe0, 0x82, 0x80, 0xe0, 0x82, 0x80, 0xe0, 0x84, 0x80});
    p.set_input({.up = true});
    // Released B1, a new direction: the ramp kept counting while held (-4).
    CHECK(run(p, 3) == std::vector<int>{0xcc, 0x80, 0xbc});
    p.set_input({});
    CHECK(run(p, 3) == std::vector<int>{0xc0, 0x80, 0x80});
    CHECK(run(p, 6).empty());
}

TEST_CASE("a mouse sends its motion saturated to 127") {
    PointingDevice p(DeviceId::kRelative);
    identify(p);
    p.set_input({.mouse_dx = 200, .mouse_dy = -2});
    p.set_input({});  // the next frame: nothing more
    CHECK(run(p, 3) == std::vector<int>{0xcd, 0xbf, 0xbe});
    CHECK(run(p, 3) == std::vector<int>{0xc0, 0x80, 0x80});  // stopped
    CHECK(run(p, 6).empty());
}

TEST_CASE("an absolute device sends 4-byte packets, only once on the active area") {
    PointingDevice p(DeviceId::kAbsoluteScreen);
    identify(p);
    CHECK(run(p, 6).empty());  // never aimed: no packet after the ID
    p.set_input({.button1 = true, .abs_x = 0x155, .abs_y = 0x2aa, .in_area = true});
    CHECK(run(p, 4) == std::vector<int>{0xe5, 0xaa, 0x95, 0xaa});
    CHECK(run(p, 8).empty());  // still, same buttons
    p.set_input({.button1 = true, .abs_x = 0x155, .abs_y = 0x2aa});
    CHECK(run(p, 4) == std::vector<int>{0xe5, 0x8a, 0x95, 0xaa});  // left the area: pd clear
}

TEST_CASE("RTS held mid-packet resets the device; releasing it sends the ID again") {
    PointingDevice p(DeviceId::kManeuvering);
    identify(p);
    p.set_input({.right = true});
    CHECK(run(p, 1).size() == 1);
    for (int i = 0; i < 200; ++i) CHECK(p.tick(kChunk, true) == -1);
    CHECK(run(p, 1) == std::vector<int>{'J' | 0x80});
}

TEST_CASE("a restored device sends the same bytes") {
    PointingDevice a(DeviceId::kManeuvering);
    identify(a);
    a.set_input({.down = true, .button2 = true});
    run(a, 1);  // mid-packet
    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    PointingDevice b(DeviceId::kManeuvering);
    REQUIRE(zcdi_test::load_state(b, state));
    b.set_input({.down = true, .button2 = true});
    CHECK(run(b, 8) == run(a, 8));
}

TEST_CASE("a device idle after a packet restores") {
    PointingDevice a(DeviceId::kManeuvering);
    identify(a);
    REQUIRE(run(a, 2).empty());  // nothing new: no packet, the last one's position stays
    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    PointingDevice b(DeviceId::kManeuvering);
    REQUIRE(zcdi_test::load_state(b, state));
    a.set_input({.left = true});
    b.set_input({.left = true});
    CHECK(run(b, 6) == run(a, 6));
}

}  // TEST_SUITE
