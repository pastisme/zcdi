#include "doctest.h"

#include <cstdint>
#include <vector>

#include "cdic/cdic.h"
#include "support/cdic_rig.h"
#include "support/fake_disc.h"

using zcdi_test::CdicRig;
using zcdi_test::Delivery;
using zcdi_test::FakeDisc;
using zcdi_test::kSubmodeAudio;
using zcdi_test::kSubmodeAudioTrigger;
using zcdi_test::kSubmodeData;
using zcdi_test::write_reg16;

TEST_SUITE("cdic") {

TEST_CASE("the audio bank follows the audio toggle") {
    const std::vector<uint8_t> submodes = {kSubmodeAudio, kSubmodeData, kSubmodeData, kSubmodeData,
                                           kSubmodeAudioTrigger, kSubmodeData, kSubmodeData, kSubmodeData,
                                           kSubmodeAudio, kSubmodeData};
    CdicRig rig(submodes);
    rig.start_read(0);
    const std::vector<Delivery> d = rig.collect(submodes.size());
    REQUIRE(d.size() == submodes.size());
    int audio_seen = 0;
    for (size_t i = 0; i < d.size(); ++i) {
        CAPTURE(i);
        const bool audio = submodes[i] != kSubmodeData;
        CHECK(((d[i].dbuf & 4) != 0) == audio);
        CHECK(d[i].submode == submodes[i]);
        CHECK(d[i].marker == FakeDisc::marker(int(i)));
        if (audio) {
            // First audio sector after a reset goes to 0x2800, then alternate.
            CHECK((d[i].dbuf & 1) == (audio_seen & 1));
            ++audio_seen;
        }
    }
}

TEST_CASE("the data bank alternates on every sector") {
    const std::vector<uint8_t> submodes = {kSubmodeData, kSubmodeAudio, kSubmodeData,
                                           kSubmodeData, kSubmodeAudio, kSubmodeData};
    CdicRig rig(submodes);
    rig.start_read(0);
    const std::vector<Delivery> d = rig.collect(submodes.size());
    REQUIRE(d.size() == submodes.size());
    int expected_bank = 1;
    for (size_t i = 0; i < d.size(); ++i) {
        CAPTURE(i);
        if (submodes[i] == kSubmodeData) CHECK((d[i].dbuf & 1) == expected_bank);
        expected_bank ^= 1;
    }
}

// After one audio sector both toggles point at bank 1.
TEST_CASE("a DBUF write with bit14 clear resets both toggles") {
    CdicRig rig({kSubmodeAudio, kSubmodeData, kSubmodeAudio});
    rig.start_read(0);
    REQUIRE(rig.collect(1).size() == 1);
    rig.stop();
    rig.start_read(1);
    const std::vector<Delivery> d = rig.collect(2);
    REQUIRE(d.size() == 2);
    CHECK(d[0].marker == FakeDisc::marker(1));
    CHECK((d[0].dbuf & 5) == 1);  // data, bank 1
    CHECK(d[1].marker == FakeDisc::marker(2));
    CHECK((d[1].dbuf & 5) == 4);  // audio, bank 0
}

// cdic.sv:453 compares only FILE's high byte.
TEST_CASE("FILE's low byte doesn't affect the file match") {
    CdicRig rig({kSubmodeData, kSubmodeData});
    write_reg16(rig.cdic, 0x006, 0x01ff);
    rig.start_read(0);
    CHECK(rig.collect(2).size() == 2);
}

// cdic.sv:613: executing any command acknowledges a pending sector interrupt.
TEST_CASE("a new command clears XBUF bit 15") {
    CdicRig rig({kSubmodeData, kSubmodeData, kSubmodeData});
    rig.start_read(0);
    for (int step = 0; step < 4000 && !rig.cdic.irq4_asserted(); ++step) rig.cdic.tick_cycles(10000);
    REQUIRE(rig.cdic.irq4_asserted());
    write_reg16(rig.cdic, 0x000, 0x002a);
    write_reg16(rig.cdic, 0x3fe, 0xc000);
    CHECK_FALSE(rig.cdic.irq4_asserted());
    CHECK((rig.cdic.read_reg_byte(0x3f6) & 0x80) == 0);
}

// cdapdriv takes any XBUF after Update as a sector (Dragon's Lair game over).
TEST_CASE("Update during a seek raises no XBUF before the first sector") {
    CdicRig rig({kSubmodeData, kSubmodeData});
    rig.start_read(1);
    write_reg16(rig.cdic, 0x000, 0x002e);
    write_reg16(rig.cdic, 0x3fe, 0xc000);
    const std::vector<Delivery> d = rig.collect(1);
    REQUIRE(d.size() == 1);
    CHECK(d[0].marker == FakeDisc::marker(1));
}

// cdic.sv spin_down_cnt: with no read, Update answers 2 sector ticks later.
TEST_CASE("Update with no read raises XBUF") {
    CdicRig rig({kSubmodeData});
    write_reg16(rig.cdic, 0x000, 0x002e);
    write_reg16(rig.cdic, 0x3fe, 0xc000);
    for (int step = 0; step < 100 && !rig.cdic.irq4_asserted(); ++step) rig.cdic.tick_cycles(10000);
    CHECK(rig.cdic.irq4_asserted());
}

// The tray opens mid-read and closes on the same disc: the read carries on.
TEST_CASE("a read waits while the CDIC has no disc") {
    CdicRig rig({kSubmodeData, kSubmodeData, kSubmodeData});
    rig.start_read(0);
    REQUIRE(rig.collect(1).size() == 1);
    rig.cdic.set_disc(nullptr);
    CHECK(rig.collect(1).empty());
    rig.cdic.set_disc(&rig.disc);
    const std::vector<Delivery> d = rig.collect(1);
    REQUIRE(d.size() == 1);
    CHECK(d[0].marker == FakeDisc::marker(1));
}

}  // TEST_SUITE
