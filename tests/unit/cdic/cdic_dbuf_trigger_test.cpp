// CDi_MiSTer cdic.sv DBUF[15] after a command (6f4ce1f).
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "cdic/cdic.h"
#include "support/cdic_rig.h"

using namespace zcdi_test;

namespace {
constexpr int kSectorTick = 200000;  // CPU cycles, 15 MHz / 75
constexpr uint16_t kTrigger = 0x8000;
constexpr uint8_t kData = 0x48;

bool trigger_set(zcdi::mamecdi::Cdic& cdic) { return (read_reg16(cdic, 0x3fe) & kTrigger) != 0; }

void run_command(CdicRig& rig, uint16_t command) {
    set_time(rig.cdic, 20);
    write_reg16(rig.cdic, 0x000, command);
    write_reg16(rig.cdic, 0x3fe, 0xc000);
}
}  // namespace

TEST_SUITE("cdic") {

TEST_CASE("a read keeps DBUF bit 15 for 5 sector ticks") {
    for (uint16_t command : {0x0029, 0x002a, 0x002c}) {  // Read Mode 1, Read Mode 2, Seek
        CAPTURE(command);
        CdicRig rig(std::vector<uint8_t>(1000, kData));
        run_command(rig, command);
        CHECK(trigger_set(rig.cdic));
        rig.cdic.tick_cycles(4 * kSectorTick);
        CHECK(trigger_set(rig.cdic));
        rig.cdic.tick_cycles(kSectorTick);
        CHECK_FALSE(trigger_set(rig.cdic));
    }
}

TEST_CASE("Fetch TOC keeps DBUF bit 15 until its first sector") {
    CdicRig rig(std::vector<uint8_t>(1000, kData));
    run_command(rig, 0x0027);
    rig.cdic.tick_cycles(10 * kSectorTick);  // past 5 ticks, still seeking
    CHECK(trigger_set(rig.cdic));
    bool sector = false;  // XBUF bit 15; reading its high byte acknowledges it
    for (int i = 0; i < 40 && !sector; ++i) {
        rig.cdic.tick_cycles(kSectorTick);
        sector = (rig.cdic.read_reg_byte(0x3f6) & 0x80) != 0;
    }
    REQUIRE(sector);
    CHECK_FALSE(trigger_set(rig.cdic));
}

TEST_CASE("Stop and Update clear DBUF bit 15 at once") {
    for (uint16_t command : {0x0023, 0x0024, 0x002e}) {
        CAPTURE(command);
        CdicRig rig(std::vector<uint8_t>(1000, kData));
        run_command(rig, command);
        CHECK_FALSE(trigger_set(rig.cdic));
    }
}

}  // TEST_SUITE
