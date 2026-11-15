// RegsUpd: window register updates taken at vsync.
#include "doctest.h"

#include <cstdint>

#include "dvc/dvc.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi_test;

namespace {
constexpr uint16_t kPlay = zcdi::dvc::vmpeg::kSysCmdPlay;
constexpr uint16_t kStep = zcdi::dvc::vmpeg::kSysCmdStep;
constexpr uint16_t kRegsUpd = zcdi::dvc::vmpeg::kVidCmdRegsUpd;
constexpr uint16_t kScroll = zcdi::dvc::vmpeg::kVidCmdScroll;
constexpr uint16_t kShow = zcdi::dvc::vmpeg::kVidCmdShow;
constexpr uint16_t kIntVcup = zcdi::dvc::vmpeg::kIntVcup;
constexpr uint16_t kIntDcl = 1u << 7;
constexpr uint32_t kPalField = 600'000;
constexpr int kFirstActiveLine = 26;

bool vcup_dcl(const Dvc& v) { return (fmv_isr(v) & (kIntVcup | kIntDcl)) == (kIntVcup | kIntDcl); }

void regs_upd(Dvc& v, uint16_t scrpos_x, uint16_t mode = 0) {
    write16(v, kFmvScrposXOffset, scrpos_x);
    write16(v, kFmvVidcmdOffset, uint16_t(kRegsUpd | mode));
}

uint16_t shown_x(const Dvc& v) { return v.display_window().scrpos_x; }

void lines(Dvc& v, int to) {
    for (int line = 0; line < to; ++line) v.end_of_line(line, kFirstActiveLine, 280);
}

// A field the way Dvc runs it: vsync, then the frame player. True if a picture was selected.
bool field(Dvc& v, size_t ready = 8) {
    notify_vsync(v);
    return frame_player_field(v, kPalField, ready);
}

// 25 Hz playback, past the start delay: a picture every other field.
void play(Dvc& v) {
    write16(v, kFmvSyscmdOffset, 0x1000);
    set_video_frame_decoded(v, 3);
    write16(v, kFmvSyscmdOffset, kPlay);
    v.tick_cycles(400'000);
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("a non-scroll RegsUpd raises VCUP at vsync and applies with the next picture") {
    Dvc v;
    write16(v, kFmvSyscmdOffset, kStep);
    regs_upd(v, 100);
    CHECK_FALSE(vcup_dcl(v));
    REQUIRE_FALSE(field(v));  // Step is armed here
    CHECK(vcup_dcl(v));
    CHECK(shown_x(v) == 0);
    REQUIRE(field(v));
    CHECK(shown_x(v) == 100);
}

TEST_CASE("a scroll RegsUpd applies at the next vsync, picture or not") {
    Dvc v;
    regs_upd(v, 100, kScroll);
    CHECK(shown_x(v) == 0);
    field(v, 0);
    CHECK(vcup_dcl(v));
    CHECK(shown_x(v) == 100);
}

TEST_CASE("a RegsUpd written after the window started scanning out waits one more picture") {
    Dvc v;
    play(v);
    write16(v, kFmvVidcmdOffset, kShow);
    while (!field(v)) {
    }
    lines(v, 100);  // the window starts on line 26
    regs_upd(v, 100);
    int pictures = 0;
    while (pictures < 1) pictures += field(v) ? 1 : 0;
    CHECK(shown_x(v) == 0);
    while (pictures < 2) pictures += field(v) ? 1 : 0;
    CHECK(shown_x(v) == 100);
}

TEST_CASE("a RegsUpd written before the window applies with the next picture") {
    Dvc v;
    play(v);
    write16(v, kFmvVidcmdOffset, kShow);
    while (!field(v)) {
    }
    lines(v, 10);  // still in vertical blanking
    regs_upd(v, 100);
    while (!field(v)) {
    }
    CHECK(shown_x(v) == 100);
}

TEST_CASE("after the last picture a pending RegsUpd applies at vsync") {
    Dvc v;
    play(v);
    while (!field(v)) {
    }
    set_video_last_picture(v);
    regs_upd(v, 100);
    field(v, 0);
    CHECK(shown_x(v) == 100);
}

TEST_CASE("a scroll RegsUpd in vertical blanking first applies the pending picture update") {
    Dvc v;
    regs_upd(v, 100);
    field(v, 0);  // pending, no picture
    REQUIRE(shown_x(v) == 0);
    regs_upd(v, 200, kScroll);  // line 0: vertical blanking
    CHECK(shown_x(v) == 100);
    field(v, 0);
    CHECK(shown_x(v) == 200);
}

TEST_CASE("without a pending RegsUpd vsync raises no VCUP") {
    Dvc v;
    field(v, 0);
    CHECK((fmv_isr(v) & kIntVcup) == 0);
}

}  // TEST_SUITE
