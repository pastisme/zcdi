#include "doctest.h"

#include <cstdint>
#include <vector>

#include "slave/slave.h"
#include "support/slave_rig.h"
#include "support/state_roundtrip.h"

using zcdi_test::channel_addr;

namespace {

constexpr int kSlot = 15000000 / 120;

// Every IN2 report as (cycle, 4 bytes), draining it like the CPU would.
std::vector<std::vector<long>> run(zcdi::mamecdi::SlaveHle& s, long cycles) {
    std::vector<std::vector<long>> out;
    for (long done = 0; done < cycles; done += 1000) {
        s.tick_cycles(1000);
        if (!s.irq2_asserted()) continue;
        std::vector<long> r = {done};
        for (int i = 0; i < 4; ++i) r.push_back(s.read_byte(channel_addr(0)));
        out.push_back(r);
    }
    return out;
}

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("a SLAVE restored mid-report sends the same reports") {
    zcdi::mamecdi::SlaveHle a;
    a.write_byte(channel_addr(0), 0x83);  // enable polling
    a.update_pointer(5, -3, true);
    run(a, kSlot + 500);  // sampled, report still in flight

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    zcdi::mamecdi::SlaveHle b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::save_state(b) == state);

    a.update_pointer(2, 0, false);
    b.update_pointer(2, 0, false);
    const auto ra = run(a, 10L * kSlot);
    const auto rb = run(b, 10L * kSlot);
    CHECK(ra.size() == 2);
    CHECK(rb == ra);
}

TEST_CASE("a SLAVE state with an out-of-range command length is rejected") {
    zcdi::mamecdi::SlaveHle a;
    std::vector<uint8_t> state = zcdi_test::save_state(a);
    // Section header (8) then channel 0: in[4], then in_len (4 bytes LE).
    state[8 + 4] = 5;
    zcdi::mamecdi::SlaveHle b;
    CHECK(!zcdi_test::load_state(b, state));
}

}  // TEST_SUITE
