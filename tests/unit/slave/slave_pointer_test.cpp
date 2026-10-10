// Paced like the 1200-baud device (CDi_MiSTer pointing_device.sv).
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "slave/slave.h"
#include "support/slave_rig.h"

using zcdi_test::channel_addr;

namespace {

constexpr int kSlot = 15000000 / 120;       // cycles per serial byte slot
constexpr int kFrame = 15000000 / 50;       // PAL frame
constexpr int kStep = 1000;

struct Report {
    long cycle;
    bool button1;
    int x;
};

struct Harness {
    zcdi::mamecdi::SlaveHle slave;
    long cycle = 0;
    std::vector<Report> reports;

    explicit Harness(bool enable_polling = true) {
        if (enable_polling) slave.write_byte(channel_addr(0), 0x83);
    }
    // Advances in small steps, draining each report the moment it's raised
    // (the CPU's IN2 handler reads it well within one slot on hardware).
    void run(long cycles) {
        for (long done = 0; done < cycles; done += kStep) {
            slave.tick_cycles(kStep);
            cycle += kStep;
            if (!slave.irq2_asserted()) continue;
            uint8_t b[4];
            for (int i = 0; i < 4; ++i) b[i] = slave.read_byte(channel_addr(0));
            reports.push_back({cycle, (b[0] & (0x02 << 3)) != 0, ((b[0] & 0x07) << 7) | b[1]});
        }
    }
};

}  // namespace

TEST_SUITE("slave") {

// 5 PAL frames = 12 slots, so phases 0-4 cover the grid.
TEST_CASE("one-frame taps at every slot phase") {
    int reported = 0;
    for (int phase = 0; phase < 5; ++phase) {
        CAPTURE(phase);
        Harness h;
        h.run(long(kFrame) * (4 + phase));
        h.slave.update_pointer(0, 0, true);
        h.run(kFrame);
        h.slave.update_pointer(0, 0, false);
        h.run(long(kFrame) * 5);
        const Report* down = nullptr;
        const Report* up = nullptr;
        for (const Report& r : h.reports) {
            if (r.button1 && !down) down = &r;
            if (!r.button1 && down && !up) up = &r;
        }
        if (!down) continue;
        ++reported;
        CHECK(up != nullptr);
        if (up) CHECK(up->cycle - down->cycle >= 3 * kSlot - kStep);
    }
    // 20 ms taps against a 25 ms sample period: most, not all, are seen.
    CHECK(reported >= 3);
}

TEST_CASE("two-frame taps are always reported") {
    for (int phase = 0; phase < 5; ++phase) {
        CAPTURE(phase);
        Harness h;
        h.run(long(kFrame) * (4 + phase));
        h.slave.update_pointer(0, 0, true);
        h.run(2L * kFrame);
        h.slave.update_pointer(0, 0, false);
        h.run(long(kFrame) * 5);
        bool down = false;
        for (const Report& r : h.reports) down |= r.button1;
        CHECK(down);
    }
}

TEST_CASE("changes during transmission go to the next report") {
    Harness h;
    h.run(10L * kSlot);           // idle, nothing to report yet (no change)
    size_t before = h.reports.size();
    h.slave.update_pointer(5, 0, true);
    h.run(kSlot);                 // sampled at the next slot boundary
    h.slave.update_pointer(7, 0, false);
    h.run(10L * kSlot);
    REQUIRE(h.reports.size() == before + 2);
    const Report& first = h.reports[before];
    const Report& second = h.reports[before + 1];
    CHECK(first.button1);
    CHECK(first.x == 5);
    CHECK(!second.button1);
    CHECK(second.x == 12);
    CHECK(second.cycle - first.cycle >= 3 * kSlot - kStep);
}

TEST_CASE("one tick spanning many slots runs every boundary") {
    zcdi::mamecdi::SlaveHle slave;
    slave.write_byte(channel_addr(0), 0x83);
    slave.update_pointer(3, 0, true);
    slave.tick_cycles(2 * kSlot);
    CHECK(!slave.irq2_asserted());  // sampled, still transmitting
    slave.tick_cycles(8 * kSlot);
    CHECK(slave.irq2_asserted());
    uint8_t b0 = slave.read_byte(channel_addr(0));
    CHECK((b0 & (0x02 << 3)) != 0);
}

TEST_CASE("no reports before pointer input is enabled, or after it's disabled") {
    Harness h(false);
    h.slave.update_pointer(10, 10, true);
    h.slave.write_byte(channel_addr(3), 0xf7);  // developer mode, not an enable
    h.run(20L * kSlot);
    CHECK(h.reports.empty());

    h.slave.write_byte(channel_addr(0), 0x83);
    h.run(20L * kSlot);
    REQUIRE(h.reports.size() == 1);  // the current state, unchanged since
    CHECK(h.reports[0].x == 10);

    h.slave.write_byte(channel_addr(0), 0x84);
    h.slave.update_pointer(5, 0, false);
    h.run(20L * kSlot);
    CHECK(h.reports.size() == 1);
}

TEST_CASE("absolute position (lightgun)") {
    Harness h;
    h.slave.set_pointer(700, 300, false);
    h.run(10L * kSlot);
    CHECK(h.reports.size() == 1);
    if (!h.reports.empty()) CHECK(h.reports.back().x == 700);
    h.slave.set_pointer(700, 300, false);
    h.run(10L * kSlot);
    CHECK(h.reports.size() == 1);
    h.slave.set_pointer(5000, -20, true);
    h.run(10L * kSlot);
    REQUIRE(h.reports.size() == 2);
    CHECK(h.reports.back().x == 767);
    CHECK(h.reports.back().button1);
}

}  // TEST_SUITE
