#include "doctest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/machine.h"
#include "core/machine_inspector.h"
#include "support/fake_machine.h"
#include "support/state_roundtrip.h"

using zcdi_test::build_fake_machine;
using zcdi_test::load_machine;
using zcdi_test::save_machine;

TEST_SUITE("core") {

TEST_CASE("frames replayed from a machine state reach the same state") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    for (int i = 0; i < 3; ++i) m->run_frame();
    const std::vector<uint8_t> at_save = save_machine(*m);
    CHECK(m->state_size() == at_save.size());
    for (int i = 0; i < 5; ++i) m->run_frame();
    const std::vector<uint8_t> first = save_machine(*m);
    REQUIRE(zcdi_test::first_difference(first, at_save) != -1);

    REQUIRE(load_machine(*m, at_save));
    CHECK(zcdi_test::first_difference(save_machine(*m), at_save) == -1);
    for (int i = 0; i < 5; ++i) m->run_frame();
    CHECK(zcdi_test::first_difference(save_machine(*m), first) == -1);
}

TEST_CASE("a machine state may be followed by padding") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    m->run_frame();
    std::vector<uint8_t> state = save_machine(*m);
    const std::vector<uint8_t> exact = state;
    state.resize(state.size() + 4096, 0);
    m->run_frame();
    REQUIRE(load_machine(*m, state));
    CHECK(zcdi_test::first_difference(save_machine(*m), exact) == -1);
}

TEST_CASE("a bad machine state is rejected and changes nothing") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    m->run_frame();
    const std::vector<uint8_t> good = save_machine(*m);
    m->run_frame();
    const std::vector<uint8_t> current = save_machine(*m);

    SUBCASE("truncated in the last section") {
        std::vector<uint8_t> bad(good.begin(), good.end() - 1);
        CHECK(!load_machine(*m, bad));
    }
    SUBCASE("another format version") {
        std::vector<uint8_t> bad = good;
        bad[8] ^= 0x80;  // version follows the ZCST tag and length
        CHECK(!load_machine(*m, bad));
    }
    SUBCASE("empty") {
        CHECK(!load_machine(*m, {}));
    }
    CHECK(zcdi_test::first_difference(save_machine(*m), current) == -1);
}

TEST_CASE("a matching state damaged past its identity checks cold-boots the machine") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    for (int i = 0; i < 3; ++i) m->run_frame();
    std::vector<uint8_t> bad = save_machine(*m);
    // Break the MCD212 section tag, deep inside the BUS section.
    const uint8_t tag[4] = {'M', 'C', 'D', ' '};
    size_t at = 0;
    while (at + 4 <= bad.size() && !std::equal(tag, tag + 4, bad.begin() + std::ptrdiff_t(at))) ++at;
    REQUIRE(at + 4 <= bad.size());
    bad[at] ^= 0x20;
    m->run_frame();

    CHECK(!load_machine(*m, bad));
    CHECK(zcdi::MachineInspector(*m).frame() == 0);
    m->run_frame();  // still runs
}

TEST_CASE("a machine state made with another BIOS is rejected") {
    std::vector<uint8_t> state;
    {
        std::unique_ptr<zcdi::Machine> a = build_fake_machine(0);
        a->run_frame();
        state = save_machine(*a);
    }
    std::unique_ptr<zcdi::Machine> b = build_fake_machine(1);
    b->run_frame();
    const std::vector<uint8_t> before = save_machine(*b);
    CHECK(!load_machine(*b, state));
    CHECK(zcdi_test::first_difference(save_machine(*b), before) == -1);
}

}  // TEST_SUITE
