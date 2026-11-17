#include "doctest.h"

#include <memory>

#include "disc/chd_disc.h"
#include "fixtures/chd_postgap_fixture.h"
#include "fixtures/chd_subcode_rw_fixture.h"
#include "fixtures/chd_two_track_fixture.h"
#include "support/mem_files.h"

using namespace zcdi;

namespace {

template <size_t N>
std::unique_ptr<DiscContext> fixture_context(const uint8_t (&bytes)[N]) {
    return zcdi_test::memory_context("fixture.chd", {{"fixture.chd", zcdi_test::bytes_of(bytes)}});
}

}  // namespace

TEST_SUITE("disc") {

TEST_CASE("CHD tracks after the first skip chdman's padding frames") {
    std::unique_ptr<Disc> disc = open_chd_disc(fixture_context(zcdi_test::kChdTwoTrackFixtureBytes));
    REQUIRE(disc);

    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 2);
    CHECK(toc[0].start_lba == 0);
    CHECK(toc[1].start_lba == 6);  // padding has no LBA
    CHECK(disc->lead_out_lba() == 9);

    // Track 1's 6 frames are followed by 2 padding frames in the CHD, so
    // track 2's first frame (LBA 6) is CHD frame 8.
    for (int lba = 0; lba < 9; ++lba) {
        CAPTURE(lba);
        RawSector s{};
        REQUIRE(disc->read_sector(lba, s));
        CHECK(s.data[0] == 0x10 + lba);
        CHECK(s.data[kRawSectorSize - 1] == 0x10 + lba);
    }
}

TEST_CASE("CHD postgap sectors read as blank, not as padding or the next track") {
    std::unique_ptr<Disc> disc = open_chd_disc(fixture_context(zcdi_test::kChdPostgapFixtureBytes));
    REQUIRE(disc);

    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 2);
    CHECK(toc[1].start_lba == 10); // 6 frames + 4 postgap
    CHECK(disc->lead_out_lba() == 13);

    // The postgap is longer than track 1's 2 padding frames, so a CHD frame
    // offset would reach track 2's first frames.
    for (int lba = 0; lba < 13; ++lba) {
        CAPTURE(lba);
        RawSector s{};
        REQUIRE(disc->read_sector(lba, s));
        const int frame = lba < 6 ? lba : lba >= 10 ? lba - 4 : -1; // bin frames, gap removed
        const uint8_t want = frame < 0 ? 0 : uint8_t(0x10 + frame);
        CHECK(s.data[0] == want);
        CHECK(s.data[kRawSectorSize - 1] == want);
    }
}

TEST_CASE("a CHD track with RW_RAW subcode gives its R..W symbols") {
    std::unique_ptr<Disc> disc = open_chd_disc(fixture_context(zcdi_test::kChdSubcodeRwFixtureBytes));
    REQUIRE(disc);
    REQUIRE(disc->lead_out_lba() == 3);
    for (int lba = 0; lba < 3; ++lba) {
        CAPTURE(lba);
        RawSector s{};
        REQUIRE(disc->read_sector(lba, s));
        CHECK(s.data[kRawSectorSize - 1] == 0x10 + lba);
        uint8_t rw[kSubcodeRwSize] = {};
        REQUIRE(disc->read_subcode_rw(lba, rw));
        for (uint32_t i = 0; i < kSubcodeRwSize; ++i) {
            CAPTURE(i);
            CHECK(rw[i] == ((lba + int(i)) & 0x3f));  // P and Q bits dropped
        }
    }
    uint8_t rw[kSubcodeRwSize] = {};
    CHECK_FALSE(disc->read_subcode_rw(3, rw));
}

TEST_CASE("a CHD without subcode gives none") {
    std::unique_ptr<Disc> disc = open_chd_disc(fixture_context(zcdi_test::kChdTwoTrackFixtureBytes));
    REQUIRE(disc);
    uint8_t rw[kSubcodeRwSize] = {};
    CHECK_FALSE(disc->read_subcode_rw(0, rw));
}

}  // TEST_SUITE
