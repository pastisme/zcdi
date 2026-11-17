// SCC68070 on-chip registers (SCC68070 User Manual), byte by byte.
#include "doctest.h"

#include <cstdint>
#include <string>

#include "chips/scc68070/scc68070.h"

using zcdi::chips::Scc68070Periph;
using namespace zcdi::chips::scc68070;

namespace {

constexpr uint32_t kDma0 = kDmaBase, kDma1 = kDmaBase + kDmaChannelStride;
constexpr int kCyclesPerTimerTick = 96;  // CPU cycles per timer count
constexpr int kFastestCharCycles = 7812;  // UCSR clock select 7, 19200 baud: 10 bits

void write16(Scc68070Periph& scc, uint32_t addr, uint16_t v) {
    scc.write_byte(addr, uint8_t(v >> 8));
    scc.write_byte(addr + 1, uint8_t(v));
}
uint16_t read16(Scc68070Periph& scc, uint32_t addr) {
    return uint16_t(scc.read_byte(addr) << 8 | scc.read_byte(addr + 1));
}

}  // namespace

TEST_SUITE("scc68070") {

TEST_CASE("the interrupt level registers keep their two 3-bit fields") {
    Scc68070Periph scc;
    for (uint32_t reg : {kLir, kPicr1, kPicr2}) {
        CAPTURE(reg);
        scc.write_byte(reg, 0xff);
        CHECK(scc.read_byte(reg) == 0x77);
        scc.write_byte(reg, 0x35);
        CHECK(scc.read_byte(reg) == 0x35);
    }
}

TEST_CASE("the I2C registers read back, and the bus reads idle") {
    Scc68070Periph scc;
    const uint32_t regs[] = {kI2cIdr, kI2cIar, kI2cIcr, kI2cIccr};
    for (uint32_t reg : regs) scc.write_byte(reg, uint8_t(reg));
    for (uint32_t reg : regs) CHECK(scc.read_byte(reg) == uint8_t(reg));
    scc.write_byte(kI2cIsr, 0x00);
    CHECK(scc.read_byte(kI2cIsr) == kI2cIsrIdle);
}

TEST_CASE("the UART control registers read back, and the transmitter is always ready") {
    Scc68070Periph scc;
    for (uint32_t reg : {kUartUmr, kUartUcsr, kUartUcr}) {
        CAPTURE(reg);
        scc.write_byte(reg, 0x5a);
        CHECK(scc.read_byte(reg) == 0x5a);
    }
    CHECK(scc.read_byte(kUartUsr) == (kUsrTxRdy | kUsrTxEmt));
}

TEST_CASE("bytes sent to UTHR are the console output, drained once") {
    Scc68070Periph scc;
    for (char c : std::string("OS-9\n")) scc.write_byte(kUartUth, uint8_t(c));
    CHECK(scc.drain_uart_output() == "OS-9\n");
    CHECK(scc.drain_uart_output().empty());
}

TEST_CASE("the transmitter interrupts once per character time while enabled") {
    Scc68070Periph scc;
    scc.write_byte(kPicr2, 0x03);  // transmit at level 3
    scc.write_byte(kUartUcr, 0x04);  // transmitter enabled
    scc.write_byte(kUartUcsr, 0x07);  // restarts the character timer
    scc.tick(kFastestCharCycles - 1);
    CHECK(scc.current_ipl() == 0);
    scc.tick(1);
    CHECK(scc.current_ipl() == 3);

    // Acknowledged by the IACK cycle, or by writing PICR2 bit 3.
    CHECK(scc.iack(3) == 0x38 + 3);
    CHECK(scc.current_ipl() == 0);
    scc.tick(kFastestCharCycles);
    CHECK(scc.current_ipl() == 3);
    scc.write_byte(kPicr2, 0x08 | 0x03);
    CHECK(scc.current_ipl() == 0);

    scc.write_byte(kUartUcr, 0x00);  // disabled
    scc.tick(10 * kFastestCharCycles);
    CHECK(scc.current_ipl() == 0);
}

TEST_CASE("the timer registers are big-endian words") {
    Scc68070Periph scc;
    const uint32_t regs[] = {kTimerReload, kTimer0, kTimer1, kTimer2};
    for (uint32_t reg : regs) write16(scc, reg, uint16_t(0x1200 | (reg & 0xff)));
    for (uint32_t reg : regs) CHECK(read16(scc, reg) == uint16_t(0x1200 | (reg & 0xff)));
    scc.write_byte(kTimerTsrTcr + 1, 0x11);
    CHECK(scc.read_byte(kTimerTsrTcr + 1) == 0x11);
}

TEST_CASE("TSR flags are cleared one by one by writing 1") {
    Scc68070Periph scc;
    scc.write_byte(kTimerTsrTcr + 1, kTcrMatch << 4);  // timer 1 in match mode
    write16(scc, kTimerReload, 0x1234);
    write16(scc, kTimer1, 0x1234);
    write16(scc, kTimer0, 0xffff);
    scc.tick(kCyclesPerTimerTick);  // overflows to the reload value, which matches timer 1
    CHECK(scc.read_byte(kTimerTsrTcr) == (kTsrOv0 | kTsrMa1));
    scc.write_byte(kTimerTsrTcr, kTsrOv0);
    CHECK(scc.read_byte(kTimerTsrTcr) == kTsrMa1);
    scc.write_byte(kTimerTsrTcr, kTsrMa1);
    CHECK(scc.read_byte(kTimerTsrTcr) == 0);
}

TEST_CASE("a DMA channel's registers read back byte by byte") {
    Scc68070Periph scc;
    for (uint32_t ch : {kDma0, kDma1}) {
        CAPTURE(ch);
        scc.write_byte(ch + kDmaCsr + 1, 0x11);  // CER
        scc.write_byte(ch + kDmaOcr, 0x22);       // DCR
        scc.write_byte(ch + kDmaOcr + 1, 0x33);   // OCR
        scc.write_byte(ch + kDmaCcr, 0x44);       // SCR
        scc.write_byte(ch + kDmaCcr + 1, 0x05);   // CCR, no start
        write16(scc, ch + kDmaMtc, 0x6677);
        write16(scc, ch + kDmaMac, 0x8899);
        write16(scc, ch + kDmaMac + 2, 0xaabb);
        CHECK(scc.read_byte(ch + kDmaCsr + 1) == 0x11);
        CHECK(read16(scc, ch + kDmaOcr) == 0x2233);
        CHECK(read16(scc, ch + kDmaCcr) == 0x4405);
        CHECK(read16(scc, ch + kDmaMtc) == 0x6677);
        CHECK(read16(scc, ch + kDmaMac) == 0x8899);
        CHECK(read16(scc, ch + kDmaMac + 2) == 0xaabb);
        CHECK(scc.read_byte(ch + kDmaCsr) == 0);
    }
    CHECK(scc.dma_operation_control() == 0x33);
    CHECK(scc.dma_memory_address_counter() == 0x8899aabbu);
}

TEST_CASE("CSR's completion flag is cleared by writing 1, and CCR's start bit doesn't stick") {
    Scc68070Periph scc;
    scc.write_byte(kDma0 + kDmaCcr + 1, kDmaCcrStart | 0x05);
    CHECK(scc.read_byte(kDma0 + kDmaCcr + 1) == 0x05);
    CHECK(scc.read_byte(kDma0 + kDmaCsr) == kDmaCsrCoc);
    scc.write_byte(kDma0 + kDmaCsr, uint8_t(~kDmaCsrCoc));
    CHECK(scc.read_byte(kDma0 + kDmaCsr) == kDmaCsrCoc);
    scc.write_byte(kDma0 + kDmaCsr, kDmaCsrCoc);
    CHECK(scc.read_byte(kDma0 + kDmaCsr) == 0);
}

TEST_CASE("unmodelled registers read 0 and ignore writes") {
    Scc68070Periph scc;
    for (uint32_t addr : {0x80008000u, kDma0 + 0x02, kDma0 + 0x10, kI2cIdr - 1}) {
        CAPTURE(addr);
        scc.write_byte(addr, 0xff);
        CHECK(scc.read_byte(addr) == 0);
    }
}

}  // TEST_SUITE
