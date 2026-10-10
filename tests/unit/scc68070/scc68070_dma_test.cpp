#include "doctest.h"

#include <cstdint>

#include "scc68070/scc68070.h"

using zcdi::mamecdi::Scc68070Periph;

namespace {
// Copies of scc68070.cpp's private kDmaCh0*/kDmaCh1* constants.
constexpr uint32_t kDmaCh0Csr = 0x80004000;  // hi=status, lo=error
constexpr uint32_t kDmaCh0Ccr = 0x80004007;  // lo byte of 0x80004006 (hi=SCR)
constexpr uint32_t kDmaCh1Csr = 0x80004040;
constexpr uint32_t kDmaCh1CcrByte = 0x80004047;  // lo byte of 0x80004046 (hi=SCR)
constexpr uint32_t kDmaCh1MtcHi = 0x8000404a;
constexpr uint32_t kDmaCh1MtcLo = 0x8000404b;

constexpr uint8_t kCsrCoc = 0x80;  // Channel Operation Complete
}  // namespace

TEST_SUITE("scc68070") {

// cdapdriv busy-polls on it for every disc read.
TEST_CASE("channel 0 CCR start strobes COC") {
    Scc68070Periph scc;
    CHECK((scc.read_byte(kDmaCh0Csr) & kCsrCoc) == 0);
    scc.write_byte(kDmaCh0Ccr, 0x80);
    CHECK((scc.read_byte(kDmaCh0Csr) & kCsrCoc) != 0);
}

TEST_CASE("channel 1 CCR start strobes COC") {
    Scc68070Periph scc;
    CHECK((scc.read_byte(kDmaCh1Csr) & kCsrCoc) == 0);
    scc.write_byte(kDmaCh1CcrByte, 0x80);
    CHECK((scc.read_byte(kDmaCh1Csr) & kCsrCoc) != 0);
}

// Autonomous completion at MTC == 0, separate from the CCR strobe.
TEST_CASE("channel 1 completes when MTC reaches zero") {
    Scc68070Periph scc;
    scc.write_byte(kDmaCh1MtcHi, 0);
    scc.write_byte(kDmaCh1MtcLo, 3);  // MTC = 3 words
    CHECK(scc.dma_ch1_transfer_counter() == 3);

    scc.complete_dma_ch1_burst(2);  // partial -- 2 of 3 words
    CHECK(scc.dma_ch1_transfer_counter() == 1);
    CHECK((scc.read_byte(kDmaCh1Csr) & kCsrCoc) == 0);  // not complete yet

    scc.complete_dma_ch1_burst(1);  // remainder -- MTC reaches 0
    CHECK(scc.dma_ch1_transfer_counter() == 0);
    CHECK((scc.read_byte(kDmaCh1Csr) & kCsrCoc) != 0);  // real completion
}

TEST_CASE("channel 1 OCR and MAC round-trip through register writes") {
    Scc68070Periph scc;
    // kDmaCh1Ocr (0x80004044) is the HIGH byte (device_control/DCR);
    // OCR itself is the LOW byte, kDmaCh1Ocr+1 = 0x80004045.
    scc.write_byte(0x80004045, 0x12);  // OCR (direction bit etc.)
    CHECK(scc.dma_ch1_operation_control() == 0x12);
    scc.write_byte(0x8000404c, 0x00);
    scc.write_byte(0x8000404d, 0x00);
    scc.write_byte(0x8000404e, 0x01);
    scc.write_byte(0x8000404f, 0x00);
    CHECK(scc.dma_ch1_memory_address_counter() == 0x00000100u);
    scc.advance_dma_ch1_memory_address_counter(4);
    CHECK(scc.dma_ch1_memory_address_counter() == 0x00000104u);
}

}  // TEST_SUITE
