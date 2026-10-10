// CDi_MiSTer dsp_registers.sv.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"

using zcdi::mamecdi::VmpegDevice;

namespace {
// kFmaDspa word index 0x1811 -> byte offset 0x3022/0x3023 (high/low).
// kFmaDspd word index 0x1812 -> byte offset 0x3024/0x3025 (high/low).
// Only the low ("completing") byte carries real effect for either register.
void write_dspa(VmpegDevice& v, uint8_t val) { v.write_byte(0x3023, val); }
void write_dspd(VmpegDevice& v, uint8_t val) { v.write_byte(0x3025, val); }
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("DSPA/DSPD program the stereo gain matrix") {
    VmpegDevice v;
    // No reset in dsp_registers.sv: power-on 0, muted.
    CHECK(v.mpeg_volume_r2r() == 0);
    CHECK(v.mpeg_volume_l2l() == 0);

    // dsp_registers.sv order: mode, target, then four dspa=7 writes fill
    // {R2R,L2R,R2L,L2L}; distinct dB values tell the slots apart.
    write_dspa(v, 0); write_dspd(v, 0x80);  // mode
    write_dspa(v, 1); write_dspd(v, 0x93);  // target
    write_dspa(v, 7); write_dspd(v, 0);     // R2R: 0dB -> 144
    write_dspa(v, 7); write_dspd(v, 10);    // L2R: 10dB -> 46
    write_dspa(v, 7); write_dspd(v, 20);    // R2L: 20dB -> 14
    write_dspa(v, 7); write_dspd(v, 0x80);  // L2L: mute (bit7 set) -> 0

    CHECK(v.mpeg_volume_r2r() == 144);
    CHECK(v.mpeg_volume_l2r() == 46);
    CHECK(v.mpeg_volume_r2l() == 14);
    CHECK(v.mpeg_volume_l2l() == 0);

    SUBCASE("a write outside the mode/target gate leaves the matrix alone") {
        // Matches real hardware's own dspa==0/1-only update.
        write_dspa(v, 0); write_dspd(v, 0x00);  // mode reset to something else
        write_dspa(v, 7); write_dspd(v, 30);    // NOT gated -- should be ignored
        CHECK(v.mpeg_volume_r2r() == 144);
    }
}

}  // TEST_SUITE
