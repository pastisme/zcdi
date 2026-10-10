#include "doctest.h"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <utility>

#include "libretro/input_mapping.h"

using zcdi::analog_stick_delta;

TEST_SUITE("libretro") {

TEST_CASE("a stick inside the dead zone doesn't move the pointer") {
    int dx = 5, dy = 5;
    analog_stick_delta(3000, -2000, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
}

TEST_CASE("a stick near an axis moves along that axis only") {
    int dx = 0, dy = 0;
    analog_stick_delta(32000, 2600, dx, dy);  // ~5 degrees of drift
    CHECK(dx == 16);
    CHECK(dy == 0);
    analog_stick_delta(-5000, -30000, dx, dy);  // ~9.5 degrees
    CHECK(dx == 0);
    CHECK(dy < 0);
}

TEST_CASE("a stick away from the axes keeps its angle") {
    int dx = 0, dy = 0;
    analog_stick_delta(20000, -20000, dx, dy);
    CHECK(dx > 0);
    CHECK(dx == -dy);
    analog_stick_delta(30000, 8000, dx, dy);  // ~15 degrees
    CHECK(dy > 0);
    CHECK(dx > dy);
}

TEST_CASE("full tilt is the same speed in every direction") {
    for (auto [x, y] : {std::pair{32767, 0}, std::pair{0, -32768}, std::pair{23170, 23170}}) {
        CAPTURE(x);
        CAPTURE(y);
        int dx = 0, dy = 0;
        analog_stick_delta(int16_t(x), int16_t(y), dx, dy);
        CHECK(std::abs(dx) + std::abs(dy) <= 23);
        CHECK(std::max(std::abs(dx), std::abs(dy)) >= 11);
    }
}

namespace {

// B and Left held on port 1, nothing on port 0.
int16_t fake_input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 1 || device != RETRO_DEVICE_JOYPAD) return 0;
    const int16_t held = (1 << RETRO_DEVICE_ID_JOYPAD_B) | (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return held;
    return (held >> id) & 1;
}

bool env_with_bitmasks(unsigned cmd, void*) { return cmd == RETRO_ENVIRONMENT_GET_INPUT_BITMASKS; }
bool env_without_bitmasks(unsigned, void*) { return false; }

}  // namespace

TEST_CASE("joypad buttons read the same with and without frontend bitmasks") {
    const uint16_t expected = (1 << RETRO_DEVICE_ID_JOYPAD_B) | (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    for (retro_environment_t env : {env_with_bitmasks, env_without_bitmasks}) {
        zcdi::query_input_bitmasks(env);
        CHECK(zcdi::joypad_buttons(fake_input_state, 1) == expected);
        CHECK(zcdi::joypad_buttons(fake_input_state, 0) == 0);
    }
    zcdi::query_input_bitmasks(nullptr);
}

}  // TEST_SUITE
