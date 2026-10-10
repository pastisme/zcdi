#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/machine.h"
#include "support/fake_audio_cd.h"
#include "support/fake_machine.h"

using zcdi_test::FakeAudioCd;

namespace {

constexpr uint32_t kSlaveChannel3 = 0x310007;

std::unique_ptr<zcdi::Machine> auto_play_machine() {
    std::vector<std::byte> bios = zcdi_test::fake_bios();
    std::unique_ptr<zcdi::Machine> m = std::make_unique<zcdi::Machine>(bios, zcdi::VideoStandard::kPal,
                                        zcdi::Machine::InitParams{.auto_play = true});
    m->cold_boot();
    return m;
}

// Asks the servo for the disc status (channel 3, as servo_hle.sv) and reads its 4 bytes.
uint32_t servo_disc_status(zcdi::Machine& m) {
    for (uint8_t v : {0xfa, 0xb0, 0x00, 0x00, 0x00}) m.bus().write8(kSlaveChannel3, v);
    for (int i = 0; i < 50; ++i) m.run_frame();
    uint32_t status = 0;
    for (int i = 0; i < 4; ++i) status = status << 8 | m.bus().read8(kSlaveChannel3);
    return status;
}

}  // namespace

TEST_SUITE("core") {

TEST_CASE("an audio CD is reported as audio and keeps Auto Play off") {
    std::unique_ptr<zcdi::Machine> m = auto_play_machine();
    REQUIRE(m->mount_disc(std::make_unique<FakeAudioCd>(std::vector<int>{75, 75})));
    CHECK_FALSE(m->bus().playcdi_overlay_active());
    CHECK(servo_disc_status(*m) == 0xb0000125);
}

// The Apprentice, Lucky Luke: the CD-i program sits before track 1's INDEX 01.
TEST_CASE("a CD-i Ready disc is reported as audio but Auto Play starts it") {
    std::unique_ptr<zcdi::Machine> m = auto_play_machine();
    FakeAudioCd::Options options;
    options.cdi_ready = true;
    REQUIRE(m->mount_disc(std::make_unique<FakeAudioCd>(std::vector<int>{75, 75}, options)));
    CHECK(m->bus().playcdi_overlay_active());
    CHECK(servo_disc_status(*m) == 0xb0000125);
}

}  // TEST_SUITE
