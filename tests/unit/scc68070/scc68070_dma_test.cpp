#include "doctest.h"

#include <cstdint>

#include "chips/scc68070/scc68070.h"

using zcdi::chips::Scc68070Periph;

using namespace zcdi::chips::scc68070;

namespace {
constexpr uint32_t kCh0 = kDmaBase;
constexpr uint32_t kCh1 = kDmaBase + kDmaChannelStride;
}  // namespace

TEST_SUITE("scc68070") {

// cdapdriv busy-polls on it for every disc read.
TEST_CASE("channel 0 CCR start strobes COC") {
    Scc68070Periph scc;
    CHECK((scc.read_byte(kCh0 + kDmaCsr) & kDmaCsrCoc) == 0);
    scc.write_byte(kCh0 + kDmaCcr + 1, kDmaCcrStart);
    CHECK((scc.read_byte(kCh0 + kDmaCsr) & kDmaCsrCoc) != 0);
}

TEST_CASE("channel 1 CCR start strobes COC") {
    Scc68070Periph scc;
    CHECK((scc.read_byte(kCh1 + kDmaCsr) & kDmaCsrCoc) == 0);
    scc.write_byte(kCh1 + kDmaCcr + 1, kDmaCcrStart);
    CHECK((scc.read_byte(kCh1 + kDmaCsr) & kDmaCsrCoc) != 0);
}

// Autonomous completion at MTC == 0, separate from the CCR strobe.
TEST_CASE("channel 1 completes when MTC reaches zero") {
    Scc68070Periph scc;
    scc.write_byte(kCh1 + kDmaMtc, 0);
    scc.write_byte(kCh1 + kDmaMtc + 1, 3);  // MTC = 3 words
    CHECK(scc.dma_ch1_transfer_counter() == 3);

    scc.complete_dma_ch1_burst(2);  // partial -- 2 of 3 words
    CHECK(scc.dma_ch1_transfer_counter() == 1);
    CHECK((scc.read_byte(kCh1 + kDmaCsr) & kDmaCsrCoc) == 0);  // not complete yet

    scc.complete_dma_ch1_burst(1);  // remainder -- MTC reaches 0
    CHECK(scc.dma_ch1_transfer_counter() == 0);
    CHECK((scc.read_byte(kCh1 + kDmaCsr) & kDmaCsrCoc) != 0);  // real completion
}

TEST_CASE("channel 1 OCR and MAC round-trip through register writes") {
    Scc68070Periph scc;
    // The high byte at kDmaOcr is DCR; OCR is the low byte.
    scc.write_byte(kCh1 + kDmaOcr + 1, 0x12);
    CHECK(scc.dma_ch1_operation_control() == 0x12);
    scc.write_byte(kCh1 + kDmaMac, 0x00);
    scc.write_byte(kCh1 + kDmaMac + 1, 0x00);
    scc.write_byte(kCh1 + kDmaMac + 2, 0x01);
    scc.write_byte(kCh1 + kDmaMac + 3, 0x00);
    CHECK(scc.dma_ch1_memory_address_counter() == 0x00000100u);
    scc.advance_dma_ch1_memory_address_counter(4);
    CHECK(scc.dma_ch1_memory_address_counter() == 0x00000104u);
}

}  // TEST_SUITE
