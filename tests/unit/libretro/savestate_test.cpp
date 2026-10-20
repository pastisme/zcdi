#include "doctest.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "audio/mixer.h"
#include "libretro/input_mapping.h"
#include "libretro/savestate.h"
#include "support/fake_machine.h"
#include "support/state_roundtrip.h"

using zcdi::InputHistory;
using zcdi::Mixer;
using zcdi::StateSizeReporter;

namespace {

struct Core {
    std::unique_ptr<zcdi::Machine> machine = zcdi_test::build_fake_machine();
    Mixer audio{44100};
    InputHistory input;

    std::vector<uint8_t> save() {
        std::vector<uint8_t> out;
        zcdi::save_core_state(*machine, audio, input, out);
        return out;
    }
    bool load(const std::vector<uint8_t>& bytes) {
        return zcdi::load_core_state(*machine, audio, input, bytes);
    }
    // Moves all three parts on, so a load has something to undo.
    void advance(int16_t sample) {
        machine->run_frame();
        audio.push_samples(std::vector<int16_t>(2000, sample));
        input.gun_x = sample;
    }
};

}  // namespace

TEST_SUITE("libretro") {

TEST_CASE("a core state restores the machine, audio queue and input history") {
    Core c;
    c.advance(100);
    const std::vector<uint8_t> state = c.save();
    CHECK(zcdi::core_state_size(*c.machine, c.audio, c.input) == state.size());
    c.advance(200);
    REQUIRE(c.load(state));
    CHECK(zcdi_test::first_difference(c.save(), state) == -1);
    CHECK(c.input.gun_x == 100);
}

TEST_CASE("a damaged core state is rejected and changes nothing") {
    Core c;
    c.advance(100);
    const std::vector<uint8_t> good = c.save();
    c.advance(200);
    const std::vector<uint8_t> current = c.save();

    SUBCASE("frontend part truncated") {
        std::vector<uint8_t> bad(good.begin(), good.end() - 1);
        CHECK(!c.load(bad));
    }
    SUBCASE("frontend part missing") {
        std::vector<uint8_t> bad = good;
        bad.resize(zcdi_test::save_machine(*c.machine).size());
        CHECK(!c.load(bad));
    }
    SUBCASE("machine part damaged") {
        std::vector<uint8_t> bad = good;
        bad[8] ^= 0x80;  // the format version, right after the ZCST header
        CHECK(!c.load(bad));
    }
    CHECK(zcdi_test::first_difference(c.save(), current) == -1);
}

TEST_CASE("a core state may be followed by padding") {
    Core c;
    c.advance(100);
    std::vector<uint8_t> state = c.save();
    const std::vector<uint8_t> exact = state;
    state.resize(state.size() + 1000, 0);
    c.advance(200);
    REQUIRE(c.load(state));
    CHECK(zcdi_test::first_difference(c.save(), exact) == -1);
}

TEST_CASE("the reported state size follows the frontend's variable-size support") {
    SUBCASE("variable: the size needed now") {
        StateSizeReporter r(true);
        CHECK(r.report(1000) == 1000);
        CHECK(r.report(5000) == 5000);
        CHECK(r.report(2000) == 2000);
    }
    SUBCASE("fixed: one size for the session, never more") {
        StateSizeReporter r(false);
        const size_t first = r.report(4'000'000);
        CHECK(first == StateSizeReporter::kFixedSize);
        CHECK(r.report(1000) == first);
        // A state that outgrows it isn't reported bigger: retro_serialize()
        // fails for it instead (the buffer is too small).
        CHECK(r.report(StateSizeReporter::kFixedSize + 1) == first);
    }
    SUBCASE("fixed: a first state already bigger sets the session size") {
        StateSizeReporter r(false);
        const size_t big = StateSizeReporter::kFixedSize + 123;
        CHECK(r.report(big) == big);
        CHECK(r.report(1000) == big);
    }
}

}  // TEST_SUITE
