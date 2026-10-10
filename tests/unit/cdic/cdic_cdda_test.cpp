#include "doctest.h"

#include <cstdint>
#include <vector>

#include "cdic/cdic.h"
#include "support/cdic_rig.h"
#include "support/fake_audio_cd.h"

using zcdi::mamecdi::Cdic;
using zcdi_test::FakeAudioCd;
using zcdi_test::set_time;
using zcdi_test::write_reg16;

namespace {

// Reading XBUF acknowledges it.
bool xbuf_pending(Cdic& cdic) { return cdic.read_reg_byte(0x3f6) & 0x80; }

// The sector of `track` whose 588 stereo samples `block` holds, or -1.
int matching_sector(const int16_t* block, int track, int sectors) {
    for (int k = 0; k < sectors; ++k) {
        bool same = true;
        for (int i = 0; i < FakeAudioCd::kSamplesPerSector && same; ++i) {
            const int n = k * FakeAudioCd::kSamplesPerSector + i;
            same = block[2 * i] == FakeAudioCd::sample(track, n, false) &&
                   block[2 * i + 1] == FakeAudioCd::sample(track, n, true);
        }
        if (same) return k;
    }
    return -1;
}

// The Q subcode stored with the next sector delivered: control, track, index,
// relative MSF, absolute MSF.
std::vector<uint8_t> next_subq(Cdic& cdic) {
    for (int step = 0; step < 4000; ++step) {
        cdic.tick_cycles(10000);
        if (!xbuf_pending(cdic)) continue;
        const uint32_t q = ((cdic.read_reg_byte(0x3ff) & 1) ? 0xa00u : 0u) + 2340;
        std::vector<uint8_t> out;
        for (int i : {0, 1, 2, 3, 4, 5, 7, 8, 9}) out.push_back(cdic.read_ram_byte(q + 2 * i + 1));
        return out;
    }
    return {};
}

}  // namespace

TEST_SUITE("cdic") {

TEST_CASE("Fetch TOC lists an audio CD's tracks as audio, disc type CD-DA") {
    FakeAudioCd disc({10, 20, 30});
    Cdic cdic;
    cdic.set_disc(&disc);
    write_reg16(cdic, 0x000, 0x0027);  // Fetch TOC
    write_reg16(cdic, 0x3fe, 0xc000);
    // POINT, PMIN, PSEC, PFRAME: tracks at LBA 0/10/30, lead-out at 60.
    const uint8_t expected[][4] = {{0x01, 0x00, 0x02, 0x00}, {0x02, 0x00, 0x02, 0x10}, {0x03, 0x00, 0x02, 0x30},
                                   {0xa0, 0x01, 0x00, 0x00}, {0xa1, 0x03, 0x00, 0x00}, {0xa2, 0x00, 0x02, 0x60}};
    int delivered = 0;
    for (int step = 0; step < 8000 && delivered < 18; ++step) {
        cdic.tick_cycles(10000);
        if (!xbuf_pending(cdic)) continue;
        const uint32_t q = ((cdic.read_reg_byte(0x3ff) & 1) ? 0xa00u : 0u) + 2340;
        auto qbyte = [&](int i) { return cdic.read_ram_byte(q + 2 * i + 1); };
        const auto& e = expected[delivered / 3];
        CAPTURE(delivered);
        CHECK(qbyte(0) == 0x01);  // audio, ADR 1
        CHECK(qbyte(2) == e[0]);
        CHECK(qbyte(7) == e[1]);
        CHECK(qbyte(8) == e[2]);
        CHECK(qbyte(9) == e[3]);
        ++delivered;
    }
    CHECK(delivered == 18);
}

// .bin CD-DA is little-endian, CHD big-endian (Disc::cdda_big_endian).
TEST_CASE("Play CDDA plays an audio CD's samples unchanged") {
    FakeAudioCd::Options options;
    SUBCASE("little-endian") { options.big_endian = false; }
    SUBCASE("big-endian") { options.big_endian = true; }
    FakeAudioCd disc({10, 40}, options);
    Cdic cdic;
    cdic.set_disc(&disc);
    set_time(cdic, 10);  // track 2
    write_reg16(cdic, 0x000, 0x0028);  // Play CDDA
    write_reg16(cdic, 0x3fe, 0xc000);

    std::vector<int16_t> pcm;
    bool playing = false;
    for (int step = 0; step < 4000 && pcm.size() < 8 * 1176; ++step) {
        cdic.tick_cycles(10000);
        if (!playing && xbuf_pending(cdic)) {
            write_reg16(cdic, 0x3fa, 0x0800);  // start the player on the first sector
            playing = true;
        }
        const std::vector<int16_t> out = cdic.drain_audio();
        pcm.insert(pcm.end(), out.begin(), out.end());
    }
    REQUIRE(pcm.size() >= 8 * 1176);
    // The player reads a bank before that tick's sector lands in it: after the
    // first sector and one stale buffer it plays track 2 in order, sample for sample.
    for (int block = 2; block < 8; ++block) {
        CAPTURE(block);
        CHECK(matching_sector(pcm.data() + block * 1176, 2, 40) == block - 2);
    }
}

// Red Book Q mode 1; the BIOS CD player shows its track and elapsed time.
TEST_CASE("Play CDDA's Q subcode names the track and the time in it") {
    FakeAudioCd disc({10, 40});
    Cdic cdic;
    cdic.set_disc(&disc);
    set_time(cdic, 15);  // 5 sectors into track 2
    write_reg16(cdic, 0x000, 0x0028);
    write_reg16(cdic, 0x3fe, 0xc000);
    CHECK(next_subq(cdic) == std::vector<uint8_t>{0x01, 0x02, 0x01, 0x00, 0x00, 0x05, 0x00, 0x02, 0x15});
}

// Index 00 counts down to INDEX 01: a CD-i Ready program before track 1.
TEST_CASE("before track 1's INDEX 01 the Q subcode is index 00, counting down") {
    FakeAudioCd::Options options;
    options.cdi_ready = true;
    FakeAudioCd disc({10}, options);
    Cdic cdic;
    cdic.set_disc(&disc);
    set_time(cdic, FakeAudioCd::kCdiReadyPregap - 10);
    write_reg16(cdic, 0x000, 0x0028);
    write_reg16(cdic, 0x3fe, 0xc000);
    CHECK(next_subq(cdic) == std::vector<uint8_t>{0x01, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x03, 0x65});
}

TEST_CASE("a data sector's Q subcode counts the track time from 00:00:00") {
    zcdi_test::CdicRig rig({0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48});
    rig.start_read(5);
    CHECK(next_subq(rig.cdic) == std::vector<uint8_t>{0x41, 0x01, 0x01, 0x00, 0x00, 0x05, 0x00, 0x02, 0x05});
}

// The BIOS CD player stops on track AA; without it the last two buffers looped.
TEST_CASE("Play CDDA runs on into a silent lead-out, Q track AA") {
    FakeAudioCd disc({10});
    Cdic cdic;
    cdic.set_disc(&disc);
    set_time(cdic, 9);  // the last sector
    write_reg16(cdic, 0x000, 0x0028);
    write_reg16(cdic, 0x3fe, 0xc000);
    CHECK(next_subq(cdic) == std::vector<uint8_t>{0x01, 0x01, 0x01, 0x00, 0x00, 0x09, 0x00, 0x02, 0x09});
    CHECK(next_subq(cdic) == std::vector<uint8_t>{0x01, 0xaa, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x10});
    const uint32_t bank = (cdic.read_reg_byte(0x3ff) & 1) ? 0x1e00 : 0x1400;
    bool silent = true;
    for (uint32_t i = 0; i < 2352; ++i) silent = silent && cdic.read_ram_byte(bank + i) == 0;
    CHECK(silent);
    CHECK(next_subq(cdic) == std::vector<uint8_t>{0x01, 0xaa, 0x01, 0x00, 0x00, 0x01, 0x00, 0x02, 0x11});
}

}  // TEST_SUITE
