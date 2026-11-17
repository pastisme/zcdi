#include "doctest.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chips/cdic/cdic.h"
#include "chips/mcd212/mcd212.h"
#include "chips/scc68070/scc68070.h"
#include "chips/slave/slave.h"
#include "core/machine.h"
#include "core/machine_inspector.h"
#include "dvc/dvc.h"
#include "support/fake_machine.h"
#include "support/state_roundtrip.h"
#include "support/machine_board.h"

using zcdi_test::board;

using zcdi_test::build_fake_machine;
using zcdi_test::save_machine;

namespace {

constexpr uint32_t kCounter = 0x00001000;  // where the fake BIOS stores d0
constexpr uint32_t kNvramBase = zcdi::address_map::kNvram.base;  // MK48T08, even bytes

}  // namespace

TEST_SUITE("core") {

TEST_CASE("peek reads memory without changing the machine") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    m->run_frame();
    zcdi::MachineInspector view(*m);
    CHECK(view.peek32(kCounter) == board(*m).read32(kCounter));
    CHECK(view.peek8(0x4fffe0) == 0xff);  // MCD212 register

    const std::vector<uint8_t> before = save_machine(*m);
    for (uint32_t a = 0; a < 0x1000000; a += 0x100) view.peek8(a);
    for (uint32_t a = 0x80000000; a < 0x80010000; ++a) view.peek8(a);
    CHECK(zcdi_test::first_difference(before, save_machine(*m)) == -1);
}

TEST_CASE("the RTC can be pinned and survives a cold boot") {
    std::vector<std::byte> bios = zcdi_test::fake_bios();
    std::unique_ptr<zcdi::Machine> m = std::make_unique<zcdi::Machine>(bios, zcdi::VideoStandard::kPal);
    using namespace std::chrono;
    m->set_rtc_boot_time(sys_days{1995y / July / 14} + 12h);
    m->cold_boot();
    CHECK(board(*m).read8(kNvramBase + 0x1fff * 2) == 0x95);  // year, BCD
    CHECK(board(*m).read8(kNvramBase + 0x1ffe * 2) == 0x07);  // month
    CHECK(board(*m).read8(kNvramBase + 0x1ffd * 2) == 0x14);  // date
    CHECK(board(*m).read8(kNvramBase + 0x1ff1 * 2) == 0x19);  // century, a stored byte
}

// Mid-frame lines are for a debugger stopped inside run_frame().
TEST_CASE("line() is the lines per field between frames") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    const zcdi::MachineInspector view(*m);
    CHECK(view.line() == zcdi::field_lines(zcdi::VideoStandard::kPal));
    m->run_frame();
    CHECK(view.line() == zcdi::field_lines(zcdi::VideoStandard::kPal));
}

// fake_bios(): SSP $70000, then at $400008 addq.l #1,d0 / move.l d0,$1000.w / bra.s.
TEST_CASE("the CPU view shows the fake BIOS loop in supervisor mode") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    m->run_frame();
    const zcdi::MachineInspector view(*m);
    const zcdi::MachineInspector::CpuRegisters regs = view.registers();
    CHECK(regs.pc == view.pc());
    CHECK(view.cpu().pc() == view.pc());
    CHECK(view.pc() >= 0x400008);
    CHECK(view.pc() <= 0x40000e);
    CHECK((regs.sr & 0x2000) != 0);  // S
    CHECK(view.sp() == 0x70000);
    CHECK(regs.a[7] == view.sp());
    CHECK(regs.isp == view.sp());
    // d0 is stored after each increment.
    CHECK(regs.d[0] - view.peek32(kCounter) <= 1);
    CHECK(regs.d[0] > 0);
}

TEST_CASE("disassembly gives each instruction's size, bytes and mnemonic") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    const zcdi::MachineInspector view(*m);
    CHECK(view.peek16(0x400008) == 0x5280);

    zcdi::MachineInspector::DisassemblyLine addq = view.disassemble(0x400008);
    CHECK(addq.valid);
    CHECK(addq.pc == 0x400008);
    CHECK(addq.size == 2);
    CHECK(std::string(addq.bytes) == "52 80");
    CHECK(std::string(addq.text).find("addq.l") == 0);

    zcdi::MachineInspector::DisassemblyLine move = view.disassemble(0x40000a);
    CHECK(move.size == 4);
    CHECK(std::string(move.bytes) == "21 C0 10 00");
    CHECK(std::string(move.text).find("move.l") == 0);

    // A 32-bit immediate, from RAM.
    board(*m).write16(0x2000, 0x203c);  // MOVE.L #$12345678,D0
    board(*m).write32(0x2002, 0x12345678);
    zcdi::MachineInspector::DisassemblyLine imm = view.disassemble(0x2000);
    CHECK(imm.size == 6);
    CHECK(std::string(imm.text).find("$12345678") != std::string::npos);
}

TEST_CASE("the frame counter restarts on a cold boot") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    const zcdi::MachineInspector view(*m);
    CHECK(view.frame() == 0);
    for (int i = 0; i < 3; ++i) m->run_frame();
    CHECK(view.frame() == 3);
    m->cold_boot();
    CHECK(view.frame() == 0);
}

TEST_CASE("the memory views are the machine's own memory") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    m->run_frame();
    const zcdi::MachineInspector view(*m);
    CHECK(view.nvram_data().data() == m->nvram_data().data());
    REQUIRE(view.plane0_data().size() == 0x80000);
    REQUIRE(view.plane1_data().size() == 0x80000);
    for (uint32_t i = 0; i < 4; ++i) CHECK(view.plane0_data()[kCounter + i] == std::byte{view.peek8(kCounter + i)});
    board(*m).write8(0x200010, 0x5a);
    CHECK(view.plane1_data()[0x10] == std::byte{0x5a});
    CHECK(view.dvc_cart_ram_data().empty());  // no cartridge

    const std::array<std::byte, 16> dvc_rom{};
    m.reset();  // one Machine at a time
    m = zcdi_test::build_fake_machine_with({.dvc = dvc_rom});
    CHECK(zcdi::MachineInspector(*m).dvc_cart_ram_data().size() == 0x100000);
}

TEST_CASE("the chip views follow the machine") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    const zcdi::MachineInspector view(*m);
    m->set_tray_open(true);
    CHECK(view.slave().tray_open());
    m->set_tray_open(false);
    CHECK_FALSE(view.slave().tray_open());
    CHECK(view.mcd212().fb_width() == 768);
    CHECK_FALSE(view.cdic().disc_active());
    CHECK(view.scc68070().dma_transfer_counter() == 0);
    CHECK(view.dvc().fmv_state().fmv_dclk_ == 0);
}

}  // TEST_SUITE
