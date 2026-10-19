// CDi_MiSTer rtl/scc68070.sv.
#include "doctest.h"

#include <cstdint>

#include "scc68070/scc68070.h"

using zcdi::mamecdi::Scc68070Periph;

namespace {

constexpr uint32_t kUartUsr = 0x80002013;
constexpr uint32_t kUartUcr = 0x80002017;
constexpr uint32_t kUartUrh = 0x8000201b;
constexpr uint32_t kPicr2 = 0x80002047;

}  // namespace

TEST_SUITE("scc68070") {

TEST_CASE("a received byte is held until URHR is read") {
    Scc68070Periph scc;
    scc.write_byte(kPicr2, 0x50);  // receive at level 5
    scc.write_byte(kUartUcr, 0x01);
    CHECK((scc.read_byte(kUartUsr) & 0x01) == 0);
    CHECK(scc.current_ipl() == 0);

    scc.uart_receive(0x4a);
    CHECK((scc.read_byte(kUartUsr) & 0x01) != 0);
    CHECK(scc.current_ipl() == 5);
    CHECK(scc.iack(5) == 0x38 + 5);
    CHECK(scc.current_ipl() == 5);  // level: acknowledging doesn't clear it

    CHECK(scc.read_byte(kUartUrh) == 0x4a);
    CHECK((scc.read_byte(kUartUsr) & 0x01) == 0);
    CHECK(scc.current_ipl() == 0);
}

TEST_CASE("the receive interrupt needs the receiver enabled") {
    Scc68070Periph scc;
    scc.write_byte(kPicr2, 0x50);
    scc.uart_receive(0x40);
    CHECK(scc.current_ipl() == 0);
    CHECK_FALSE(scc.uart_receiver_enabled());
    scc.write_byte(kUartUcr, 0x09);  // receiver and transmitter enabled
    CHECK(scc.uart_receiver_enabled());
    CHECK(scc.current_ipl() == 5);
    scc.write_byte(kUartUcr, 0x0a);  // receiver disabled
    CHECK(scc.current_ipl() == 0);
}

TEST_CASE("RTS follows the receiver enable") {
    Scc68070Periph scc;
    CHECK(scc.consume_uart_rts());  // reset: receiver off
    scc.write_byte(kUartUcr, 0x09);
    CHECK_FALSE(scc.consume_uart_rts());
    scc.write_byte(kUartUcr, 0x0a);
    CHECK(scc.consume_uart_rts());
    CHECK(scc.consume_uart_rts());  // still off
}

// u68070's SetStat writes UCR 0a, 40, then 09 within a few instructions.
TEST_CASE("an RTS pulse between two calls is still reported once") {
    Scc68070Periph scc;
    scc.write_byte(kUartUcr, 0x09);
    CHECK_FALSE(scc.consume_uart_rts());
    scc.write_byte(kUartUcr, 0x0a);
    scc.write_byte(kUartUcr, 0x40);
    scc.write_byte(kUartUcr, 0x09);
    CHECK(scc.uart_receiver_enabled());
    CHECK(scc.consume_uart_rts());
    CHECK_FALSE(scc.consume_uart_rts());
}

}  // TEST_SUITE
