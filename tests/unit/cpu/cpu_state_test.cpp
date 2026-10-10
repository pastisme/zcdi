#include "doctest.h"

#include <array>
#include <cstdint>
#include <vector>

#include "support/cpu_fixture.h"
#include "support/state_roundtrip.h"

using zcdi_test::CpuFixture;

namespace {

struct Snapshot {
    std::array<uint32_t, 8> d{}, a{};
    uint32_t pc = 0;
    uint16_t sr = 0;
    uint32_t usp = 0, isp = 0;
    std::vector<uint8_t> ram;

    bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot(CpuFixture& f) {
    Snapshot s;
    for (unsigned i = 0; i < 8; ++i) {
        s.d[i] = f.cpu.d(i);
        s.a[i] = f.cpu.a(i);
    }
    s.pc = f.cpu.pc();
    s.sr = f.cpu.sr();
    s.usp = f.cpu.usp();
    s.isp = f.cpu.isp();
    for (uint32_t addr = 0x4000; addr < 0x5000; ++addr) s.ram.push_back(f.bus.read8(addr));
    return s;
}

void restore_ram(CpuFixture& f, const Snapshot& s) {
    for (uint32_t i = 0; i < s.ram.size(); ++i) f.bus.write8(0x4000 + i, s.ram[i]);
}

}  // namespace

TEST_SUITE("cpu") {

TEST_CASE("a restored CPU replays the same instructions to the same result") {
    CpuFixture f;
    f.code({
        0x7000,          // moveq #0,d0
        0x41f8, 0x4000,  // lea $4000.w,a0
        0x5280,          // loop: addq.l #1,d0
        0x20c0,          //   move.l d0,(a0)+
        0xb0fc, 0x5000,  //   cmpa.w #$5000,a0
        0x66f6,          //   bne.s loop
        0x60fe,          // bra.s *
    });
    f.reset_and_run(1000);

    const std::vector<uint8_t> state = zcdi_test::save_state(f.cpu);
    const Snapshot at_save = snapshot(f);
    f.cpu.run(3000);
    const Snapshot first = snapshot(f);
    REQUIRE(first.d[0] != at_save.d[0]);

    REQUIRE(zcdi_test::load_state(f.cpu, state));
    restore_ram(f, at_save);
    CHECK(snapshot(f) == at_save);
    f.cpu.run(3000);
    CHECK(snapshot(f) == first);
}

TEST_CASE("a truncated CPU state is rejected and changes nothing") {
    CpuFixture f;
    f.code({0x60fe});
    f.reset_and_run(100);
    std::vector<uint8_t> state = zcdi_test::save_state(f.cpu);
    const std::vector<uint8_t> before = state;
    state.pop_back();
    CHECK(!zcdi_test::load_state(f.cpu, state));
    CHECK(zcdi_test::save_state(f.cpu) == before);
}

}  // TEST_SUITE
