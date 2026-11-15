#include "doctest.h"

#include <cstdint>

#include "chips/cdic/cdic.h"

using zcdi::chips::Cdic;
using namespace zcdi::chips::cdic;

TEST_SUITE("cdic") {

TEST_CASE("Time and Channel are 32 bits wide, high byte first") {
    Cdic cdic;
    for (uint32_t reg : {kRegTime, kRegChannel}) {
        CAPTURE(reg);
        for (uint32_t i = 0; i < 4; ++i) cdic.write_reg_byte(reg + i, uint8_t(0x11 * (i + 1)));
        for (uint32_t i = 0; i < 4; ++i) CHECK(cdic.read_reg_byte(reg + i) == 0x11 * (i + 1));
    }
}

TEST_CASE("DMA control is write-only") {
    Cdic cdic;
    cdic.write_reg_byte(kRegDmaControl, 0x12);
    cdic.write_reg_byte(kRegDmaControl + 1, 0x34);
    CHECK(cdic.dma_control() == 0x1234);
    CHECK(cdic.read_reg_byte(kRegDmaControl) == 0);
    CHECK(cdic.read_reg_byte(kRegDmaControl + 1) == 0);
}

TEST_CASE("reading XBUF's high byte clears its ready bit") {
    Cdic cdic;
    cdic.write_reg_byte(kRegXbuf, 0x80);
    cdic.write_reg_byte(kRegXbuf + 1, 0x05);
    CHECK(cdic.read_reg_byte(kRegXbuf + 1) == 0x05);
    CHECK(cdic.read_reg_byte(kRegXbuf) == 0x80);
    CHECK(cdic.read_reg_byte(kRegXbuf) == 0x00);
    CHECK(cdic.read_reg_byte(kRegXbuf + 1) == 0x05);
}

}  // TEST_SUITE
