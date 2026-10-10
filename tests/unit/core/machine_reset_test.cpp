#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/machine.h"
#include "core/machine_inspector.h"
#include "support/fake_disc.h"
#include "support/fake_machine.h"
#include "support/state_roundtrip.h"

using zcdi_test::build_fake_machine;
using zcdi_test::save_machine;

namespace {

constexpr uint32_t kRamMarker = 0x00008000;  // main RAM, untouched by the fake BIOS
constexpr uint32_t kCounter = 0x00001000;    // where the fake BIOS stores d0
constexpr uint32_t kCdicRam = 0x00300010;    // CDIC buffer RAM

// Leaves traces a reset must clear or keep: frames run, main RAM and a
// chip's buffer RAM written.
void dirty(zcdi::Machine& m) {
    for (int i = 0; i < 4; ++i) m.run_frame();
    m.bus().write8(kRamMarker, 0x5a);
    m.bus().write8(kCdicRam, 0x77);
}

constexpr uint32_t kCdicReg = 0x00303c00;  // CDIC registers

void write_cdic16(zcdi::Machine& m, uint32_t off, uint16_t v) {
    m.bus().write8(kCdicReg + off, uint8_t(v >> 8));
    m.bus().write8(kCdicReg + off + 1, uint8_t(v));
}

// Read Mode 2 of file 1 / channel 0 from LBA 0, through the bus. Returns the
// marker that landed in the bank DBUF points at, or -1.
int read_first_sector_marker(zcdi::Machine& m) {
    write_cdic16(m, 0x006, 0x0100);  // file 1
    write_cdic16(m, 0x008, 0x0000);
    write_cdic16(m, 0x00a, 0x0001);  // channel 0
    write_cdic16(m, 0x00c, 0x0001);
    write_cdic16(m, 0x002, 0x0002);  // 00:02:00 = LBA 0
    write_cdic16(m, 0x004, 0x0000);
    write_cdic16(m, 0x000, 0x002a);  // Read Mode 2
    write_cdic16(m, 0x3fe, 0xc000);
    for (int frame = 0; frame < 50; ++frame) {
        m.run_frame();
        if (!(m.bus().read8(kCdicReg + 0x3f6) & 0x80)) continue;
        const uint16_t dbuf = uint16_t(m.bus().read8(kCdicReg + 0x3fe) << 8 | m.bus().read8(kCdicReg + 0x3ff));
        const uint32_t bank = (dbuf & 1) ? 0xa00 : 0;
        return m.bus().read8(0x00300000 + 8 + bank + 4);
    }
    return -1;
}

constexpr uint32_t kBootCount = 0x00002000;  // bumped by every boot of exit_bios()

// First boot asks the SLAVE for a reset (0x8a on channel 2) and spins, as the
// real BIOS does at 0x400636; main RAM keeps the boot count.
std::vector<std::byte> exit_bios() {
    std::vector<std::byte> rom(0x80000, std::byte{0});
    const uint8_t code[] = {
        0x00, 0x07, 0x00, 0x00,                          // reset SSP $70000
        0x00, 0x40, 0x00, 0x08,                          // reset PC $400008
        0x52, 0x38, 0x20, 0x00,                          // addq.b #1,$2000.w
        0x0c, 0x38, 0x00, 0x01, 0x20, 0x00,              // cmpi.b #1,$2000.w
        0x66, 0x08,                                      // bne.s spin
        0x13, 0xfc, 0x00, 0x8a, 0x00, 0x31, 0x00, 0x05,  // move.b #$8a,$310005
        0x60, 0xfe,                                      // spin: bra.s spin
    };
    for (size_t i = 0; i < sizeof(code); ++i) rom[i] = std::byte{code[i]};
    return rom;
}

}  // namespace

TEST_SUITE("core") {

// As cditop.sv last_reset_by_slave: Auto Play stays off until an external reset.
TEST_CASE("a SLAVE reset request resets the machine and turns Auto Play off") {
    std::vector<std::byte> bios = exit_bios();
    std::unique_ptr<zcdi::Machine> m = std::make_unique<zcdi::Machine>(bios, zcdi::VideoStandard::kPal,
                                        zcdi::Machine::InitParams{.auto_play = true});
    m->cold_boot();
    REQUIRE(m->mount_disc(std::make_unique<zcdi_test::FakeDisc>(
        std::vector<uint8_t>(40, zcdi_test::kSubmodeData))));
    REQUIRE(m->bus().playcdi_overlay_active());

    for (int i = 0; i < 3; ++i) m->run_frame();
    CHECK(m->bus().read8(kBootCount) == 2);
    CHECK_FALSE(m->bus().playcdi_overlay_active());

    // Saved with the machine; an external reset or cold boot clears it.
    const std::vector<uint8_t> after_exit = save_machine(*m);
    SUBCASE("reset") { m->reset(); }
    SUBCASE("cold boot") { m->cold_boot(); }
    CHECK(m->bus().playcdi_overlay_active());
    REQUIRE(zcdi_test::load_machine(*m, after_exit));
    CHECK_FALSE(m->bus().playcdi_overlay_active());
}

// The RTC keeps counting across a cold boot, so its section is blanked.
TEST_CASE("a cold boot after running matches a freshly built machine") {
    std::vector<uint8_t> after_cold_boot;
    {
        std::unique_ptr<zcdi::Machine> a = build_fake_machine();
        dirty(*a);
        a->cold_boot();
        CHECK(a->bus().read8(kRamMarker) == 0);
        CHECK(a->bus().read8(kCdicRam) == 0);
        for (int i = 0; i < 3; ++i) a->run_frame();
        after_cold_boot = save_machine(*a);
    }
    std::unique_ptr<zcdi::Machine> b = build_fake_machine();
    for (int i = 0; i < 3; ++i) b->run_frame();
    std::vector<uint8_t> fresh = save_machine(*b);
    zcdi_test::blank_section(after_cold_boot, "RTC ");
    zcdi_test::blank_section(fresh, "RTC ");
    CHECK(zcdi_test::first_difference(after_cold_boot, fresh) == -1);
}

TEST_CASE("a reset keeps main RAM and resets everything else") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    dirty(*m);
    const uint8_t counter_byte = m->bus().read8(kCounter + 3);
    REQUIRE(counter_byte != 0);
    m->reset();
    CHECK(m->bus().read8(kRamMarker) == 0x5a);
    CHECK(m->bus().read8(kCounter + 3) == counter_byte);
    CHECK(m->bus().read8(kCdicRam) == 0);

    // Apart from main RAM (and the RTC), a reset machine is a cold-booted
    // one: cold boot the same machine, put the same RAM back, compare.
    std::vector<uint8_t> after_reset = save_machine(*m);
    std::vector<uint8_t> ram;
    zcdi::StateWriter keep(ram);
    m->bus().ram_state_io(keep);
    m->cold_boot();
    zcdi::StateReader restore(ram);
    m->bus().ram_state_io(restore);
    REQUIRE(!restore.failed());
    std::vector<uint8_t> cold_plus_ram = save_machine(*m);
    zcdi_test::blank_section(after_reset, "RTC ");
    zcdi_test::blank_section(cold_plus_ram, "RTC ");
    CHECK(zcdi_test::first_difference(after_reset, cold_plus_ram) == -1);
}

TEST_CASE("NVRAM keeps its address and contents across a cold boot and a reset") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    // The frontend writes its .srm through this pointer (retro_get_memory_data).
    std::byte* nvram = const_cast<std::byte*>(m->bus().nvram_data().data());
    nvram[0x100] = std::byte{0xa5};
    m->run_frame();
    m->cold_boot();
    CHECK(m->bus().nvram_data().data() == nvram);
    CHECK(nvram[0x100] == std::byte{0xa5});
    m->reset();
    CHECK(m->bus().nvram_data().data() == nvram);
    CHECK(nvram[0x100] == std::byte{0xa5});
}

TEST_CASE("a reset and a cold boot keep the mounted disc readable") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    REQUIRE(m->mount_disc(std::make_unique<zcdi_test::FakeDisc>(
        std::vector<uint8_t>(40, zcdi_test::kSubmodeData))));
    const int before = read_first_sector_marker(*m);
    REQUIRE(before >= zcdi_test::FakeDisc::marker(0));
    REQUIRE(before < zcdi_test::FakeDisc::marker(40));
    const std::vector<uint8_t> state_with_disc = save_machine(*m);

    SUBCASE("reset") { m->reset(); }
    SUBCASE("cold boot") { m->cold_boot(); }

    CHECK(zcdi_test::load_machine(*m, state_with_disc));
    const int after = read_first_sector_marker(*m);
    CHECK(after >= zcdi_test::FakeDisc::marker(0));
    CHECK(after < zcdi_test::FakeDisc::marker(40));
}

TEST_CASE("an open tray keeps the disc from the CDIC until it closes or the machine resets") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    REQUIRE(m->mount_disc(std::make_unique<zcdi_test::FakeDisc>(
        std::vector<uint8_t>(40, zcdi_test::kSubmodeData))));
    m->set_tray_open(true);
    CHECK(m->tray_open());
    CHECK(read_first_sector_marker(*m) == -1);

    SUBCASE("closed") { m->set_tray_open(false); }
    SUBCASE("reset") { m->reset(); }
    CHECK_FALSE(m->tray_open());
    CHECK(read_first_sector_marker(*m) >= zcdi_test::FakeDisc::marker(0));
}

TEST_CASE("a state saved with the tray open loads with it open") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    REQUIRE(m->mount_disc(std::make_unique<zcdi_test::FakeDisc>(
        std::vector<uint8_t>(40, zcdi_test::kSubmodeData))));
    m->set_tray_open(true);
    const std::vector<uint8_t> open_state = save_machine(*m);
    m->set_tray_open(false);
    REQUIRE(zcdi_test::load_machine(*m, open_state));
    CHECK(m->tray_open());
    CHECK(read_first_sector_marker(*m) == -1);
}

// A frontend switching player model builds the new machine, then drops the old.
TEST_CASE("destroying the previous machine leaves the new one its bus") {
    std::unique_ptr<zcdi::Machine> old_machine = build_fake_machine();
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    old_machine.reset();
    for (int i = 0; i < 2; ++i) m->run_frame();
    CHECK(zcdi::MachineInspector(*m).peek32(kCounter) != 0);
}

}  // TEST_SUITE
