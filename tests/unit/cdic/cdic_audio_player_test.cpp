#include "doctest.h"

#include <cstdint>
#include <vector>

#include "cdic/cdic.h"
#include "support/cdic_rig.h"
#include "support/fake_disc.h"

using zcdi::mamecdi::Cdic;
using zcdi_test::read_reg16;
using zcdi_test::set_time;
using zcdi_test::write_reg16;

namespace {

constexpr uint32_t kBuffer0 = 0x2800;
constexpr uint32_t kBuffer1 = 0x3200;
constexpr uint8_t kStereoLevelB = 0x01;  // 4 ticks a buffer
constexpr int kTickCycles = 200000;      // one 75 Hz sector tick

void fill_buffer(Cdic& cdic, uint32_t base, uint8_t coding) {
    cdic.write_ram_byte(base + 11, coding);
    for (uint32_t i = 0; i < 2324; ++i) cdic.write_ram_byte(base + 12 + i, uint8_t(0x31 + i * 7));
}

constexpr uint32_t kAchan = 0x00c;
constexpr uint32_t kAbuf = 0x3f4;
constexpr uint32_t kAudctl = 0x3fa;
constexpr uint32_t kDbuf = 0x3fe;

void ticks(Cdic& cdic, int n) {
    for (int i = 0; i < n; ++i) cdic.tick_cycles(kTickCycles);
}

}  // namespace

TEST_SUITE("cdic") {

TEST_CASE("AUDCTL bit 11 starts the player, bit 13 alone doesn't") {
    Cdic cdic;
    fill_buffer(cdic, kBuffer0, kStereoLevelB);
    fill_buffer(cdic, kBuffer1, kStereoLevelB);
    write_reg16(cdic, kAudctl, 0x2000);
    ticks(cdic, 10);
    CHECK(cdic.drain_audio().empty());

    write_reg16(cdic, kAudctl, 0x2800);
    ticks(cdic, 1);
    CHECK_FALSE(cdic.drain_audio().empty());
    CHECK((read_reg16(cdic, kAbuf) & 0x8000) == 0);
    ticks(cdic, 4);  // the first buffer has played
    CHECK(cdic.irq4_asserted());
    CHECK((read_reg16(cdic, kAbuf) & 0x8000) != 0);
    CHECK_FALSE(cdic.irq4_asserted());  // reading ABUF acknowledges it
}

TEST_CASE("without AUDCTL bit 13 a played buffer raises no interrupt") {
    Cdic cdic;
    fill_buffer(cdic, kBuffer0, kStereoLevelB);
    fill_buffer(cdic, kBuffer1, kStereoLevelB);
    write_reg16(cdic, kAudctl, 0x0800);
    ticks(cdic, 5);
    CHECK_FALSE(cdic.irq4_asserted());
    CHECK((read_reg16(cdic, kAbuf) & 0x8000) != 0);
}

TEST_CASE("a 0xff coding stops the player and sets AUDCTL bit 0") {
    Cdic cdic;
    fill_buffer(cdic, kBuffer0, kStereoLevelB);
    fill_buffer(cdic, kBuffer1, 0xff);
    write_reg16(cdic, kAudctl, 0x0800);
    ticks(cdic, 1);
    CHECK(read_reg16(cdic, kAudctl) == 0x0800);
    ticks(cdic, 4);  // buffer 0 done, buffer 1 is 0xff
    CHECK(read_reg16(cdic, kAudctl) == 0x0001);
    CHECK(read_reg16(cdic, kAudctl) == 0x0000);  // the read cleared bit 0

    SUBCASE("it stays stopped") {
        cdic.drain_audio();
        fill_buffer(cdic, kBuffer1, kStereoLevelB);
        ticks(cdic, 10);
        CHECK(cdic.drain_audio().empty());
    }
    SUBCASE("a DBUF write stopping the read clears bit 0 too") {
        fill_buffer(cdic, kBuffer0, 0xff);
        write_reg16(cdic, kAudctl, 0x0800);
        ticks(cdic, 1);
        write_reg16(cdic, kDbuf, 0x0000);
        CHECK(read_reg16(cdic, kAudctl) == 0x0000);
    }
}

TEST_CASE("clearing bit 11 finishes the buffer, or aborts it with ACHAN set") {
    Cdic cdic;
    fill_buffer(cdic, kBuffer0, kStereoLevelB);
    fill_buffer(cdic, kBuffer1, kStereoLevelB);
    SUBCASE("ACHAN 0: the buffer plays out and is reported") {
        write_reg16(cdic, kAchan, 0x0000);
        write_reg16(cdic, kAudctl, 0x0800);
        ticks(cdic, 1);
        write_reg16(cdic, kAudctl, 0x0000);
        CHECK_FALSE(cdic.drain_audio().empty());
        ticks(cdic, 4);
        CHECK((read_reg16(cdic, kAbuf) & 0x8000) != 0);
        ticks(cdic, 10);
        CHECK(cdic.drain_audio().empty());
    }
    SUBCASE("ACHAN set: the buffer is cut off unheard") {
        write_reg16(cdic, kAchan, 0x0001);
        write_reg16(cdic, kAudctl, 0x0800);
        ticks(cdic, 1);
        write_reg16(cdic, kAudctl, 0x0000);
        CHECK(cdic.drain_audio().empty());
        ticks(cdic, 10);
        CHECK((read_reg16(cdic, kAbuf) & 0x8000) == 0);
    }
}

TEST_CASE("Play CDDA stores every sector, alternating CD-DA buffers") {
    zcdi_test::CdicRig rig(std::vector<uint8_t>(40, 0));
    set_time(rig.cdic, 0);
    write_reg16(rig.cdic, 0x000, 0x0028);  // Play CDDA
    write_reg16(rig.cdic, 0x3fe, 0xc000);
    int delivered = 0;
    for (int step = 0; step < 4000 && delivered < 4; ++step) {
        rig.cdic.tick_cycles(10000);
        if (!(rig.cdic.read_reg_byte(0x3f6) & 0x80)) continue;
        const bool bank1 = rig.cdic.read_reg_byte(0x3ff) & 1;
        CHECK(bank1 == (delivered % 2 == 0));  // the first sector goes to 0x1e00
        // FakeDisc's marker (offset 24): .bin samples come unswapped.
        const uint8_t marker = rig.cdic.read_ram_byte((bank1 ? 0x1e00 : 0x1400) + 24);
        CHECK(marker >= zcdi_test::FakeDisc::marker(0));
        CHECK(marker < zcdi_test::FakeDisc::marker(40));
        ++delivered;
    }
    CHECK(delivered == 4);
}

TEST_CASE("Fetch TOC streams the lead-in's Q subcode, each entry three times") {
    zcdi_test::CdicRig rig(std::vector<uint8_t>(40, 0));
    write_reg16(rig.cdic, 0x000, 0x0027);  // Fetch TOC
    write_reg16(rig.cdic, 0x3fe, 0xc000);
    // FakeDisc: one data track at LBA 0, lead-out at LBA 1000 (00:15:25).
    const uint8_t expected[][4] = {{0x01, 0x00, 0x02, 0x00}, {0xa0, 0x01, 0x10, 0x00},
                                   {0xa1, 0x01, 0x00, 0x00}, {0xa2, 0x00, 0x15, 0x25}};
    int delivered = 0;
    for (int step = 0; step < 8000 && delivered < 15; ++step) {
        rig.cdic.tick_cycles(10000);
        if (!(rig.cdic.read_reg_byte(0x3f6) & 0x80)) continue;
        // Q subcode, one byte a word, after the sector in the bank DBUF names.
        const uint32_t q = ((rig.cdic.read_reg_byte(0x3ff) & 1) ? 0xa00u : 0u) + 2340;
        auto qbyte = [&](int i) { return rig.cdic.read_ram_byte(q + 2 * i + 1); };
        const auto& e = expected[delivered / 3 % 4];
        CAPTURE(delivered);
        CHECK(qbyte(0) == 0x41);  // data track, ADR 1
        CHECK(qbyte(1) == 0x00);  // track 0: the lead-in
        CHECK(qbyte(2) == e[0]);
        CHECK(qbyte(7) == e[1]);
        CHECK(qbyte(8) == e[2]);
        CHECK(qbyte(9) == e[3]);
        ++delivered;
    }
    CHECK(delivered == 15);  // the lead-in repeats until the read is stopped
}

TEST_CASE("in CD-DA mode the player plays the raw buffer DBUF names") {
    Cdic cdic;
    write_reg16(cdic, 0x000, 0x0028);  // Play CDDA (no disc: nothing arrives)
    write_reg16(cdic, kDbuf, 0xc000);  // DBUF bit 0 clear: 0x1400 is the latest
    for (uint32_t i = 0; i < 2352; ++i) cdic.write_ram_byte(0x1400 + i, uint8_t(i));
    write_reg16(cdic, kAudctl, 0x0800);
    cdic.tick_cycles(1);  // an idle player starts at once
    const std::vector<int16_t> pcm = cdic.drain_audio();
    REQUIRE(pcm.size() == 1176);
    CHECK(pcm[0] == int16_t(0x0100));  // left: bytes 0, 1
    CHECK(pcm[1] == int16_t(0x0302));  // right: bytes 2, 3
    ticks(cdic, 1);
    CHECK(cdic.drain_audio().size() == 1176);  // then 0x1e00
}

}  // TEST_SUITE
