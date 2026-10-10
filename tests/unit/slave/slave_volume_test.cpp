// As SLAVE firmware 2.0 programs the two AD7528 DACs: code/256 of the input,
// scaled 9/16 (dual_ad7528.sv). 0 dB is code 255 -> 143, 6 dB code 128 -> 72.
#include "doctest.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "scc68070/scc68070.h"
#include "slave/slave.h"

using zcdi::mamecdi::SlaveHle;

namespace {

constexpr uint32_t kChannel2 = 0x310005;
constexpr int kTickCycles = 7680;  // the SLAVE timer's 512 us tick at 15 MHz

void send(SlaveHle& slave, std::initializer_list<uint8_t> bytes) {
    for (uint8_t v : bytes) slave.write_byte(kChannel2, v);
}

void run_ticks(SlaveHle& slave, int ticks) {
    for (int i = 0; i < ticks; ++i) slave.tick_cycles(kTickCycles);
    std::vector<int16_t> discard;
    slave.attenuate(discard);  // close the window, as every frame does
}

// One stereo sample through the attenuators, left and right.
std::vector<int16_t> pass(SlaveHle& slave, int16_t l, int16_t r) {
    std::vector<int16_t> pcm{l, r};
    slave.attenuate(pcm);
    return pcm;
}

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("power-on attenuation is plain stereo at 0 dB") {
    SlaveHle slave;
    CHECK(pass(slave, 1024, -2048) == std::vector<int16_t>{572, -1144});
}

TEST_CASE("0xc0-0xcf set the four paths, the low nibble mutes some") {
    SlaveHle slave;
    SUBCASE("6 dB on the straight paths, cross paths muted") {
        send(slave, {0xc5, 6, 0, 6, 0});
        CHECK(pass(slave, 1024, -2048) == std::vector<int16_t>{288, -576});
    }
    SUBCASE("all four paths at 0 dB mix to mono") {
        send(slave, {0xc0, 0, 0, 0, 0});
        CHECK(pass(slave, 1024, 512) == std::vector<int16_t>{858, 858});
    }
    SUBCASE("swapped channels") {
        send(slave, {0xca, 0, 0, 0, 0});
        CHECK(pass(slave, 1024, 512) == std::vector<int16_t>{286, 572});
    }
    SUBCASE("45 dB and more is silence") {
        send(slave, {0xc5, 45, 0, 44, 0});
        CHECK(pass(slave, 1024, 1024) == std::vector<int16_t>{0, 4});
    }
    SUBCASE("nothing changes before the fourth byte") {
        send(slave, {0xc5, 6, 0, 6});
        CHECK(pass(slave, 1024, 1024) == std::vector<int16_t>{572, 572});
    }
}

TEST_CASE("0x82 fades out and 0x83 fades back in over 52 ticks") {
    SlaveHle slave;
    send(slave, {0x82});
    run_ticks(slave, 20);
    const std::vector<int16_t> mid = pass(slave, 1024, 1024);
    CHECK(mid[0] > 0);
    CHECK(mid[0] < 572);
    run_ticks(slave, 40);
    CHECK(pass(slave, 1024, 1024) == std::vector<int16_t>{0, 0});

    send(slave, {0x83});
    run_ticks(slave, 60);
    CHECK(pass(slave, 1024, 1024) == std::vector<int16_t>{572, 572});
}

TEST_CASE("a mute fade lands where it happened in the frame") {
    SlaveHle slave;
    std::vector<int16_t> pcm(200, 1024);  // 100 stereo samples
    send(slave, {0x82});
    for (int i = 0; i < 60; ++i) slave.tick_cycles(kTickCycles);
    slave.attenuate(pcm);
    CHECK(pcm[0] > 560);   // the fade had barely started
    CHECK(pcm[198] == 0);  // and was over by the end
}

TEST_CASE("master volume ramps only once 0x88 enables it") {
    SlaveHle slave;
    send(slave, {0x86});
    CHECK(pass(slave, 1024, 0)[0] == 572);

    send(slave, {0x88, 0x86});  // 1 dB softer now, then a step per 11 interrupts
    CHECK(pass(slave, 1024, 0)[0] == 512);
    run_ticks(slave, 16 * 11);
    CHECK(pass(slave, 1024, 0)[0] == 456);
    send(slave, {0x87});
    run_ticks(slave, 16 * 30);
    CHECK(pass(slave, 1024, 0)[0] == 456);
}

TEST_CASE("0x8f toggles a 20 dB dim") {
    SlaveHle slave;
    send(slave, {0x8f});
    CHECK(pass(slave, 1024, 0)[0] == 56);
    send(slave, {0x8f});
    CHECK(pass(slave, 1024, 0)[0] == 572);
}

}  // TEST_SUITE
