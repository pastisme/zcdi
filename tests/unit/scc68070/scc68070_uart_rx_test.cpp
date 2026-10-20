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
    CHECK_FALSE(scc.uart_receiver_enabled());
    scc.write_byte(kUartUcr, 0x09);  // receiver and transmitter enabled
    CHECK(scc.uart_receiver_enabled());
    scc.uart_receive(0x40);
    CHECK(scc.current_ipl() == 5);
    scc.write_byte(kUartUcr, 0x0a);  // receiver disabled: the byte stays, its interrupt doesn't
    CHECK(scc.current_ipl() == 0);
    scc.write_byte(kUartUcr, 0x09);
    CHECK(scc.current_ipl() == 5);
}

// The back socket device keeps sending while /t2 is closed or reconfigured.
TEST_CASE("a disabled receiver takes no byte") {
    Scc68070Periph scc;
    scc.write_byte(kPicr2, 0x50);
    scc.uart_receive(0x4a);  // power-on: receiver off
    scc.write_byte(kUartUcr, 0x09);
    CHECK((scc.read_byte(kUartUsr) & 0x01) == 0);
    CHECK(scc.current_ipl() == 0);
    scc.uart_receive(0x4a);
    CHECK(scc.current_ipl() == 5);
    CHECK(scc.read_byte(kUartUrh) == 0x4a);
}

// Datasheet RCR 7:4 = 0010: "The receiver is disabled and the FIFO flushed."
TEST_CASE("reset receiver flushes the holding register") {
    Scc68070Periph scc;
    scc.write_byte(kUartUcr, 0x01);
    scc.uart_receive(0x4d);
    CHECK((scc.read_byte(kUartUsr) & 0x01) != 0);
    scc.write_byte(kUartUcr, 0x20);
    scc.write_byte(kUartUcr, 0x01);
    CHECK((scc.read_byte(kUartUsr) & 0x01) == 0);
}

}  // TEST_SUITE
