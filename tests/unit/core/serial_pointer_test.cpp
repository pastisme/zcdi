// CDi_MiSTer rtl/pointing_device.sv.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "core/serial_pointer.h"
#include "scc68070/scc68070.h"
#include "support/state_roundtrip.h"

using zcdi::SerialPointer;

namespace {

constexpr int kByteCycles = zcdi::mamecdi::kCpuClockHz / 120;
constexpr int kChunk = 1000;

// The bytes the device sends in `byte_times` byte times with RTS released.
std::vector<int> run(SerialPointer& p, int byte_times) {
    std::vector<int> out;
    for (int c = 0; c < byte_times * kByteCycles; c += kChunk) {
        const int b = p.tick(kChunk, false);
        if (b >= 0) out.push_back(b);
    }
    return out;
}

}  // namespace

TEST_SUITE("core") {

TEST_CASE("an unplugged device sends nothing") {
    SerialPointer p;
    CHECK(run(p, 10).empty());
}

TEST_CASE("the device ID comes once RTS is released, then reports while moving") {
    SerialPointer p;
    p.set_connected(true);
    for (int i = 0; i < 1000; ++i) CHECK(p.tick(kChunk, true) == -1);
    CHECK(run(p, 4) == std::vector<int>{'J' | 0x80});

    p.set_input({.right = true, .button1 = true});
    // Speed N ramps 2, 2, 4 per report; one report every 3 byte times.
    CHECK(run(p, 9) == std::vector<int>{0xe0, 0x82, 0x80, 0xe0, 0x82, 0x80, 0xe0, 0x84, 0x80});

    p.set_input({.up = true});
    // Released B1, a new direction: the ramp kept counting while held (-4).
    CHECK(run(p, 3) == std::vector<int>{0xcc, 0x80, 0xbc});

    p.set_input({});
    CHECK(run(p, 3) == std::vector<int>{0xc0, 0x80, 0x80});  // stopping is reported
    CHECK(run(p, 6).empty());
}

TEST_CASE("mouse motion switches to a relative device, the pad switches back") {
    SerialPointer p;
    p.set_connected(true);
    run(p, 1);
    const SerialPointer::Input in{.mouse_dx = 5};
    p.set_input(in);
    CHECK(run(p, 1) == std::vector<int>{'M' | 0x80});
    p.set_input(in);  // accumulated while relative
    p.set_input(in);
    CHECK(run(p, 3) == std::vector<int>{0xc0, 0x80 | 10, 0x80});

    p.set_input({.left = true});
    CHECK(run(p, 1) == std::vector<int>{'J' | 0x80});
}

TEST_CASE("a restored device sends the same bytes") {
    SerialPointer a;
    a.set_connected(true);
    run(a, 2);
    const SerialPointer::Input in{.down = true};
    a.set_input(in);
    run(a, 4);
    a.tick(kByteCycles / 3, false);

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    SerialPointer b;
    b.set_connected(true);
    REQUIRE(zcdi_test::load_state(b, state));
    b.set_input(in);
    CHECK(run(b, 12) == run(a, 12));
}

}  // TEST_SUITE
