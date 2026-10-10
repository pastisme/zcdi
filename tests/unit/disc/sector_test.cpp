#include "doctest.h"

#include <cstring>
#include <initializer_list>

#include "disc/sector.h"

using namespace zcdi;

TEST_SUITE("disc") {

TEST_CASE("mode 2 sector geometry") {
    CHECK(kRawSectorSize == 2352);
    CHECK(kForm1DataSize == 2048);
    CHECK(kForm2DataSize == 2324);
    CHECK(kSyncSize + kHeaderSize + kSubheaderSize + kForm1DataSize + 280 == kRawSectorSize);
}

TEST_CASE("lba to msf counts the 150-sector pregap") {
    struct Case { int lba, m, s, f; };
    for (const Case c : {Case{0, 0, 2, 0},  // first data sector
                         Case{74, 0, 2, 74}, Case{75, 0, 3, 0},
                         Case{4499, 1, 1, 74}, Case{100000, 22, 15, 25}}) {
        CAPTURE(c.lba);
        int m = 0, s = 0, f = 0;
        lba_to_msf(c.lba, m, s, f);
        CHECK(m == c.m);
        CHECK(s == c.s);
        CHECK(f == c.f);
    }
}

TEST_CASE("a byteswapped sector is told by its whole sync field") {
    uint8_t sector[kRawSectorSize] = {};
    const uint8_t sync[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::memcpy(sector, sync, sizeof sync);
    CHECK_FALSE(sector_looks_byteswapped(sector));
    byteswap_sector(sector, kRawSectorSize);
    CHECK(sector_looks_byteswapped(sector));

    // CD-DA starting with the sample 255 (bytes ff 00) is not a sector.
    uint8_t audio[kRawSectorSize] = {};
    audio[0] = 0xff;
    CHECK_FALSE(sector_looks_byteswapped(audio));
}

TEST_CASE("the generated scramble sequence starts as ECMA-130 Annex B") {
    for (int i = 0; i < kSyncSize; ++i) CHECK(kSectorScramble[i] == 0);
    const uint8_t start[8] = {0x01, 0x80, 0x00, 0x60, 0x00, 0x28, 0x00, 0x1e};
    for (int i = 0; i < 8; ++i) CHECK(kSectorScramble[kSyncSize + i] == start[i]);
    CHECK(kSectorScramble[kRawSectorSize - 1] == 0x99);
}

TEST_CASE("a scrambled sector is valid only once descrambled") {
    const int lba = 4499;
    uint8_t sector[kRawSectorSize] = {};
    const uint8_t header[4] = {0x01, 0x01, 0x74, 0x02};  // 01:01:74 BCD with the pregap, mode 2
    const uint8_t sub[4] = {0x01, 0x02, 0x64, 0x7f};
    std::memcpy(sector + kSyncSize, header, sizeof header);
    std::memcpy(sector + kSyncSize + kHeaderSize, sub, sizeof sub);
    std::memcpy(sector + kSyncSize + kHeaderSize + 4, sub, sizeof sub);
    REQUIRE(sector_looks_valid(sector, lba));
    CHECK_FALSE(sector_looks_valid(sector, lba + 1));

    descramble_sector(sector);  // the same XOR scrambles
    CHECK_FALSE(sector_looks_valid(sector, lba));
    descramble_sector(sector);
    CHECK(sector_looks_valid(sector, lba));
}

}  // TEST_SUITE
