// The Board's wiring between chips: DMA, the shared level-4 interrupt, and the back socket.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "chips/cdic/cdic.h"
#include "chips/scc68070/scc68070.h"
#include "core/board.h"
#include "support/dvc_cart.h"
#include "support/dvc_private.h"
#include "support/fake_disc.h"
#include "support/machine_board.h"
#include "support/slave_rig.h"
#include "support/vmpeg_feed.h"
#include "support/vmpeg_regs.h"

using zcdi::Board;
using namespace zcdi::address_map;
using namespace zcdi::chips::scc68070;

namespace {

constexpr uint32_t kDma0 = kDmaBase, kDma1 = kDmaBase + kDmaChannelStride;
constexpr uint32_t kCdic = kCdicRegs.base;
constexpr uint32_t kVmpeg = kDvcRegs.base;

// A DMA channel's memory address, word count and direction (OCR bit 7: device to memory).
void program_dma(Board& board, uint32_t channel, uint32_t address, uint16_t words, bool to_memory) {
    board.write32(channel + kDmaMac, address);
    board.write16(channel + kDmaMtc, words);
    board.write8(channel + kDmaOcr + 1, to_memory ? 0x80 : 0x00);
}

// Reads file 1 / channel 0 from `lba` as the BIOS CD driver programs it (see CdicRig).
void start_cdic_read(Board& board, int lba) {
    board.write16(kCdic + 0x006, 0x0100);
    board.write16(kCdic + 0x008, 0x0000);
    board.write16(kCdic + 0x00a, 0x0001);
    board.write16(kCdic + 0x00c, 0x0001);
    const zcdi_test::MsfBcd t = zcdi_test::msf_bcd(lba);
    board.write16(kCdic + 0x002, uint16_t(t.m << 8 | t.s));
    board.write16(kCdic + 0x004, uint16_t(t.f << 8));
    board.write16(kCdic + 0x000, zcdi::chips::cdic::kCmdReadMode2);
    board.write16(kCdic + 0x3fe, 0xc000);
}

// Runs the peripherals until `ipl` shows, at most `cycles`.
bool tick_until_ipl(Board& board, int ipl, int cycles) {
    for (int done = 0; done < cycles; done += 1000) {
        board.periph_tick(1000);
        if (board.update_interrupts() == ipl) return true;
    }
    return false;
}

}  // namespace

TEST_SUITE("core") {

TEST_CASE("a CDIC DMA copies CDIC RAM to memory and back, from DMA channel 0's address") {
    Board board;
    for (uint32_t i = 0; i < 16; ++i) board.write8(kCdicRam.base + 0x100 + i, uint8_t(0xa0 + i));

    program_dma(board, kDma0, 0x2000, 8, /*to_memory=*/true);
    board.write16(kCdic + zcdi::chips::cdic::kRegDmaControl, 0x0100);  // CDIC RAM byte 0x100
    for (uint32_t i = 0; i < 16; ++i) CHECK(board.read8(0x2000 + i) == uint8_t(0xa0 + i));
    CHECK(board.read32(kDma0 + kDmaMac) == 0x2010);  // advanced by the bytes moved

    for (uint32_t i = 0; i < 4; ++i) board.write8(0x3000 + i, uint8_t(0x50 + i));
    program_dma(board, kDma0, 0x3000, 2, /*to_memory=*/false);
    board.write16(kCdic + zcdi::chips::cdic::kRegDmaControl, 0x0200);
    for (uint32_t i = 0; i < 4; ++i) CHECK(board.read8(kCdicRam.base + 0x200 + i) == uint8_t(0x50 + i));
}

TEST_CASE("a VMPEG DMA feeds memory to the FMV word by word and completes channel 1") {
    const std::vector<uint8_t> es = {0x00, 0x00, 0x01, 0xb3, 0x16, 0x00, 0xf0, 0x13, 0xff, 0xff};
    Board board;
    zcdi_test::insert_dvc_cart(board);
    for (std::size_t i = 0; i < es.size(); ++i) board.write8(0x4000 + uint32_t(i), es[i]);
    program_dma(board, kDma1, 0x4000, uint16_t(es.size() / 2), /*to_memory=*/false);
    board.write16(kVmpeg + zcdi_test::kFmvSyscmdOffset, 0x1000);  // Decoder On
    board.write16(kVmpeg + zcdi_test::kFmvSyscmdOffset, 0x8000);  // start DMA, to video

    zcdi::dvc::Dvc& dvc = access_private::dvc_(board);
    CHECK(zcdi_test::drain_fmv_es_bytes(dvc) == zcdi_test::demux_video(es.data(), es.size()));
    CHECK(board.read32(kDma1 + kDmaMac) == 0x4000 + es.size());
    CHECK(board.read16(kDma1 + kDmaMtc) == 0);
    CHECK((board.read8(kDma1 + kDmaCsr) & kDmaCsrCoc) != 0);
    CHECK_FALSE(dvc.dma_active());
}

TEST_CASE("a VMPEG DMA towards memory moves nothing but still advances channel 1") {
    Board board;
    zcdi_test::insert_dvc_cart(board);
    board.write8(0x4000, 0x5a);
    program_dma(board, kDma1, 0x4000, 4, /*to_memory=*/true);
    board.write16(kVmpeg + zcdi_test::kFmvSyscmdOffset, 0x1000);
    board.write16(kVmpeg + zcdi_test::kFmvSyscmdOffset, 0x8000);
    CHECK(zcdi_test::drain_fmv_es_bytes(access_private::dvc_(board)).empty());
    CHECK(board.read8(0x4000) == 0x5a);
    CHECK(board.read32(kDma1 + kDmaMac) == 0x4008);
}

// The CDIC and VMPEG share the SCC68070's IN4 pin through a latch.
TEST_CASE("level 4 is acknowledged by whichever of CDIC and VMPEG raised it first") {
    zcdi_test::FakeDisc disc(std::vector<uint8_t>(40, zcdi_test::kSubmodeData));
    Board board;
    board.set_disc(&disc);
    board.write16(kCdic + zcdi::chips::cdic::kRegInterruptVector, 0x0070);
    board.write16(kVmpeg + 2 * zcdi::dvc::vmpeg::kFmvIvec, 0x0328);  // vector 0x65
    CHECK(board.interrupt_ack(3) == 0x38 + 3);  // other levels: the SCC's own vectors

    start_cdic_read(board, 0);
    REQUIRE(tick_until_ipl(board, 4, 40'000'000));  // spin-up, then the first sector
    CHECK(board.interrupt_ack(4) == 0x70);

    // VMPEG asks while the CDIC holds the latch: the CDIC keeps it.
    board.write16(kVmpeg + zcdi_test::kFmvTcntOffset, 0);
    board.write16(kVmpeg + 2 * zcdi::dvc::vmpeg::kFmvIer, zcdi::dvc::vmpeg::kIntTim);
    board.periph_tick(3000);
    REQUIRE(access_private::dvc_(board).irq_asserted());
    board.update_interrupts();
    CHECK(board.interrupt_ack(4) == 0x70);

    // The CDIC's request drops (stop the read, read its status): the latch frees, then VMPEG takes it.
    board.write16(kCdic + 0x3fe, 0x0000);
    board.read16(kCdic + zcdi::chips::cdic::kRegXbuf);
    board.read16(kCdic + zcdi::chips::cdic::kRegAbuf);
    for (int i = 0; i < 2; ++i) board.update_interrupts();
    CHECK(board.interrupt_ack(4) == 0x65);
}

TEST_CASE("the back socket's device reaches the SCC68070 UART as 7-bit bytes once the SLAVE lowers PC6") {
    Board board;
    board.plug(zcdi::Socket::kBack, zcdi::DeviceId::kRelative);
    board.write8(kUartUcr, 0x01);  // receiver on
    board.periph_tick(200'000);
    CHECK((board.read8(kUartUsr) & kUsrRxRdy) == 0);  // RTS negated: the device holds

    board.write8(zcdi_test::channel_addr(3), zcdi::chips::slave::kSysPc6High);
    bool received = false;
    for (int i = 0; i < 400 && !received; ++i) {
        board.periph_tick(10'000);
        received = (board.read8(kUartUsr) & kUsrRxRdy) != 0;
    }
    REQUIRE(received);
    CHECK(board.read8(kUartUrh) == 'M');  // the relative device's ID, stop bit dropped
}

TEST_CASE("when the CDIC and VMPEG both wait for the latch, VMPEG gets it") {
    zcdi_test::FakeDisc disc(std::vector<uint8_t>(40, zcdi_test::kSubmodeData));
    Board board;
    board.set_disc(&disc);
    board.write16(kCdic + zcdi::chips::cdic::kRegInterruptVector, 0x0070);
    board.write16(kVmpeg + 2 * zcdi::dvc::vmpeg::kFmvIvec, 0x0328);
    board.write16(kVmpeg + zcdi_test::kFmvTcntOffset, 0);
    board.write16(kVmpeg + 2 * zcdi::dvc::vmpeg::kFmvIer, zcdi::dvc::vmpeg::kIntTim);
    start_cdic_read(board, 0);
    for (int done = 0; done < 40'000'000; done += 10'000) board.periph_tick(10'000);  // no latch update meanwhile
    REQUIRE(board.update_interrupts() == 4);
    CHECK(board.interrupt_ack(4) == 0x65);

    board.read16(kVmpeg + zcdi_test::kFmvIsrOffset);  // VMPEG's request drops: the CDIC was waiting
    for (int i = 0; i < 2; ++i) board.update_interrupts();
    CHECK(board.interrupt_ack(4) == 0x70);
}

}  // TEST_SUITE
