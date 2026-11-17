// CD-i ADPCM sound groups (Green Book IV.3-IV.5), decoded on their own.
#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "chips/cdic/cdic_adpcm.h"

using zcdi::chips::AdpcmResampleState;
using zcdi::chips::History;
using zcdi::chips::decode_xa_adpcm;

namespace {

constexpr int kGroups = 18, kGroupSize = 128, kSamplesPerUnit = 28;

// Coding byte (Green Book IV.3.2.4): bit 0 stereo, bits 2-3 rate, bits 4-5 width, bit 6 emphasis.
constexpr uint8_t kLevelAMono = 0x10, kLevelAStereo = 0x11, kLevelCMono = 0x04, kLevelBStereo = 0x01;
constexpr uint8_t kEmphasis = 0x40;

// One sector's sound groups, every unit's parameter byte `param`.
struct Sector {
    uint8_t data[kGroups * kGroupSize] = {};
    explicit Sector(uint8_t param = 0x00) {
        for (int g = 0; g < kGroups; ++g) {
            for (int i = 0; i < 16; ++i) data[g * kGroupSize + i] = param;
        }
    }
    // Level A: sample `s` of sound unit `unit` (0-3) in group `g`.
    uint8_t& level_a(int g, int unit, int s) { return data[g * kGroupSize + 16 + unit + s * 4]; }
};

std::vector<int16_t> decode(const Sector& sector, uint8_t coding, History history[2]) {
    std::vector<int16_t> out;
    REQUIRE(decode_xa_adpcm(sector.data, coding, out, history));
    return out;
}

std::vector<int16_t> decode(const Sector& sector, uint8_t coding) {
    History history[2];
    return decode(sector, coding, history);
}

}  // namespace

TEST_SUITE("cdic") {

TEST_CASE("each coding gives its sector's sample count, as stereo frames at 37.8 kHz") {
    const Sector sector;
    // Level A: 4 sound units of 28 per group; Levels B/C: 8. Mono is doubled, 18.9 kHz repeated.
    CHECK(decode(sector, kLevelAMono).size() == 2u * kGroups * 4 * kSamplesPerUnit);
    CHECK(decode(sector, kLevelAStereo).size() == 2u * kGroups * 2 * kSamplesPerUnit);
    CHECK(decode(sector, kLevelCMono).size() == 2u * 2 * kGroups * 8 * kSamplesPerUnit);
    CHECK(decode(sector, kLevelBStereo).size() == 2u * kGroups * 4 * kSamplesPerUnit);
}

TEST_CASE("reserved codings are refused and add nothing") {
    const Sector sector;
    History history[2];
    std::vector<int16_t> out = {1, 2};
    for (uint8_t coding : {uint8_t(0x20), uint8_t(0x30), uint8_t(0x02), uint8_t(0x08), uint8_t(0x0c)}) {
        CAPTURE(int(coding));
        CHECK_FALSE(decode_xa_adpcm(sector.data, coding, out, history));
    }
    CHECK(out.size() == 2);
}

// Green Book IV.5.3: with filter 0 a sample is the data shifted by 8 - range (8 bits).
TEST_CASE("Level A mono: filter 0 is the 8-bit sample shifted left by 8 minus the range") {
    Sector sector;
    sector.level_a(0, 0, 0) = 0x12;
    sector.level_a(0, 0, 1) = 0x80;  // -128
    sector.level_a(0, 1, 0) = 0x7f;  // unit 1 follows unit 0's 28 samples
    std::vector<int16_t> out = decode(sector, kLevelAMono);
    CHECK(out[0] == 0x1200);
    CHECK(out[1] == out[0]);  // mono on both channels
    CHECK(out[2] == -0x8000);
    CHECK(out[2 * kSamplesPerUnit] == 0x7f00);

    Sector ranged(0x02);  // range 2: shifted by 6
    ranged.level_a(0, 0, 0) = 0x12;
    CHECK(decode(ranged, kLevelAMono)[0] == 0x12 << 6);
}

// Green Book IV.3.5, Figure IV.8: left in sound units 0 and 2, right in 1 and 3.
TEST_CASE("Level A stereo: units 0 and 2 are left, 1 and 3 right") {
    Sector sector;
    sector.level_a(0, 0, 0) = 0x01;
    sector.level_a(0, 1, 0) = 0x02;
    sector.level_a(0, 2, 0) = 0x03;
    sector.level_a(0, 3, 0) = 0x04;
    std::vector<int16_t> out = decode(sector, kLevelAStereo);
    CHECK(out[0] == 0x0100);
    CHECK(out[1] == 0x0200);
    CHECK(out[2 * kSamplesPerUnit] == 0x0300);
    CHECK(out[2 * kSamplesPerUnit + 1] == 0x0400);
}

// Green Book Figure IV.15: filter 1 adds 0.9375 (60/64) of the previous sample.
TEST_CASE("filter 1 adds 60/64 of the previous sample, and the sum saturates") {
    Sector sector(0x10);
    History history[2];
    history[0].s0 = 1000;
    std::vector<int16_t> out = decode(sector, kLevelAMono, history);
    CHECK(out[0] == 938);
    CHECK(out[2] == 879);  // 938 * 60/64, rounded

    Sector loud(0x10);
    loud.level_a(0, 0, 0) = 0x7f;
    History hot[2];
    hot[0].s0 = 30000;
    CHECK(decode(loud, kLevelAMono, hot)[0] == 32767);
}

TEST_CASE("the history carries from one sector to the next") {
    Sector first;
    first.level_a(kGroups - 1, 3, kSamplesPerUnit - 1) = 0x40;  // the sector's last sample
    History history[2];
    decode(first, kLevelAMono, history);
    CHECK(history[0].s0 == 0x4000);
    const std::vector<int16_t> out = decode(Sector(0x10), kLevelAMono, history);
    CHECK(out[0] == (60 * 0x4000 + 32) / 64);
}

TEST_CASE("18.9 kHz sectors play each frame twice") {
    Sector sector;
    sector.level_a(0, 0, 0) = 0x10;
    sector.level_a(0, 0, 1) = 0x20;
    const std::vector<int16_t> out = decode(sector, kLevelAMono | 0x04);
    CHECK(out.size() == 4u * kGroups * 4 * kSamplesPerUnit);
    CHECK(out[0] == 0x1000);
    CHECK(out[2] == 0x1000);
    CHECK(out[4] == 0x2000);
}

// IEC 60908 50/15 us: gain 1 at DC, 15/50 at the top of the band.
TEST_CASE("de-emphasis keeps a constant level and cuts the highest frequency to 30%") {
    Sector dc, nyquist;
    for (int g = 0; g < kGroups; ++g) {
        for (int unit = 0; unit < 4; ++unit) {
            for (int s = 0; s < kSamplesPerUnit; ++s) {
                dc.level_a(g, unit, s) = 0x20;
                nyquist.level_a(g, unit, s) = s % 2 ? 0xe0 : 0x20;  // +-0x2000
            }
        }
    }
    const std::vector<int16_t> flat = decode(dc, kLevelAMono | kEmphasis);
    CHECK(std::abs(flat[flat.size() - 2] - 0x2000) <= 0x2000 / 100);

    const std::vector<int16_t> alternating = decode(nyquist, kLevelAMono | kEmphasis);
    const double amplitude = std::abs(alternating[alternating.size() - 2]);
    CHECK(amplitude == doctest::Approx(0.3 * 0x2000).epsilon(0.05));
    CHECK(decode(nyquist, kLevelAMono)[0] == 0x2000);  // without emphasis, untouched
}

TEST_CASE("the 44.1 kHz resampler waits for two input frames") {
    AdpcmResampleState state;
    std::vector<int16_t> out;
    zcdi::chips::resample_adpcm_to_44100({100, -100}, out, state);
    CHECK(out.empty());
    zcdi::chips::resample_adpcm_to_44100({100, -100, 100, -100}, out, state);
    REQUIRE(out.size() >= 2);
    CHECK(out[0] == 100);
    CHECK(out[1] == -100);
}

}  // TEST_SUITE
