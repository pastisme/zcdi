#include "doctest.h"

#include <cstdint>

#include "chips/mcd212/mcd212.h"

using zcdi::chips::Mcd212;
using namespace zcdi::chips::mcd212;

TEST_SUITE("mcd212") {

TEST_CASE("each path's DCR, VSR, DDR and DCA read back byte by byte") {
    Mcd212 mcd;
    const uint32_t regs[] = {kDcr1, kVsr1, kDdr1, kDca1, kDcr2, kVsr2, kDdr2, kDca2};
    for (uint32_t i = 0; i < 8; ++i) {
        mcd.write_byte(regs[i], uint8_t(0x10 + i));
        mcd.write_byte(regs[i] + 1, uint8_t(0x80 + i));
    }
    for (uint32_t i = 0; i < 8; ++i) {
        CAPTURE(i);
        CHECK(mcd.read_byte(regs[i]) == 0x10 + i);
        CHECK(mcd.read_byte(regs[i] + 1) == 0x80 + i);
    }
}

TEST_CASE("CSRW is write-only and the gaps read 0") {
    Mcd212 mcd;
    mcd.write_byte(kCsr1W, 0x80);
    mcd.write_byte(kCsr2W, 0x80);
    CHECK(mcd.read_byte(kCsr1W) == 0);
    CHECK(mcd.read_byte(kCsr2W) == 0);
    mcd.write_byte(0x06, 0xff);
    mcd.write_byte(0x1e, 0xff);
    CHECK(mcd.read_byte(0x06) == 0);
    CHECK(mcd.read_byte(0x1e) == 0);
}

}  // TEST_SUITE
