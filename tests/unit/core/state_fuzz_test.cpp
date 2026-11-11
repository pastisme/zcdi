#include "doctest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "core/machine.h"
#include "core/state_archive.h"
#include "support/fake_disc.h"
#include "support/fake_machine.h"

TEST_SUITE("core") {

// Run under ASan/UBSan to catch what a check() should have rejected.
TEST_CASE("a state with a damaged field is rejected, or loads into a machine that runs") {
    const std::array<std::byte, 16> dvc_rom{};
    const std::vector<std::byte> bios = zcdi_test::fake_bios();
    auto m = std::make_unique<zcdi::Machine>(bios, zcdi::VideoStandard::kPal, zcdi::Machine::InitParams{.dvc = dvc_rom});
    m->mount_disc(std::make_unique<zcdi_test::FakeDisc>(std::vector<uint8_t>(40, zcdi_test::kSubmodeData)));
    m->cold_boot();
    m->bus().plug(zcdi::Bus::Socket::kFront, zcdi::DeviceId::kManeuvering);
    for (int i = 0; i < 3; ++i) m->run_frame();

    std::vector<uint8_t> good;
    std::vector<zcdi::StateWriter::Span> spans;
    zcdi::StateWriter w(good);
    w.log_spans(&spans);
    m->save_state(w);
    REQUIRE(spans.size() > 100);

    // Every field is as likely a target as the 1 MiB of RAM.
    std::mt19937 rng(1);
    int loaded = 0;
    for (int trial = 0; trial < 400; ++trial) {
        const zcdi::StateWriter::Span s = spans[rng() % spans.size()];
        std::vector<uint8_t> bad = good;
        const size_t at = s.offset + rng() % s.length;
        bad[at] ^= uint8_t(1 + rng() % 255);
        CAPTURE(trial);
        CAPTURE(at);
        zcdi::StateReader r(bad);
        if (m->load_state(r)) {
            ++loaded;
            m->run_frame();
            m->run_frame();
        }
    }
    zcdi::StateReader r(good);
    CHECK(m->load_state(r));
    CHECK(loaded > 0);
}

}  // TEST_SUITE
