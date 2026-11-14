// RegsUpd: window register updates taken at vsync.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

namespace {
constexpr uint16_t kPlay = zcdi::mamecdi::vmpeg::kSysCmdPlay;
constexpr uint16_t kStep = zcdi::mamecdi::vmpeg::kSysCmdStep;
constexpr uint16_t kRegsUpd = zcdi::mamecdi::vmpeg::kVidCmdRegsUpd;
constexpr uint16_t kScroll = zcdi::mamecdi::vmpeg::kVidCmdScroll;
constexpr uint16_t kShow = zcdi::mamecdi::vmpeg::kVidCmdShow;
constexpr uint16_t kIntVcup = zcdi::mamecdi::vmpeg::kIntVcup;
constexpr uint16_t kIntDcl = 1u << 7;
constexpr uint32_t kPalField = 600'000;
constexpr int kFirstActiveLine = 26;

bool vcup_dcl(const VmpegDevice& v) { return (v.fmv_isr() & (kIntVcup | kIntDcl)) == (kIntVcup | kIntDcl); }

void regs_upd(VmpegDevice& v, uint16_t scrpos_x, uint16_t mode = 0) {
    write16(v, kFmvScrposXOffset, scrpos_x);
    write16(v, kFmvVidcmdOffset, uint16_t(kRegsUpd | mode));
}

uint16_t shown_x(const VmpegDevice& v) { return v.display_window().scrpos_x; }

void lines(VmpegDevice& v, int to) {
    for (int line = 0; line < to; ++line) v.notify_line(line, kFirstActiveLine, 280);
}

// A field the way Bus runs it: vsync, then the frame player. True if a picture was selected.
bool field(VmpegDevice& v, size_t ready = 8) {
    v.notify_vsync();
    return v.frame_player_field(kPalField, ready).show;
}

// 25 Hz playback, past the start delay: a picture every other field.
void play(VmpegDevice& v) {
    write16(v, kFmvSyscmdOffset, 0x1000);
    v.set_video_frame_decoded(3);
    write16(v, kFmvSyscmdOffset, kPlay);
    v.tick_cycles(400'000);
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("a non-scroll RegsUpd raises VCUP at vsync and applies with the next picture") {
    VmpegDevice v;
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
    VmpegDevice v;
    regs_upd(v, 100, kScroll);
    CHECK(shown_x(v) == 0);
    field(v, 0);
    CHECK(vcup_dcl(v));
    CHECK(shown_x(v) == 100);
}

TEST_CASE("a RegsUpd written after the window started scanning out waits one more picture") {
    VmpegDevice v;
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
    VmpegDevice v;
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
    VmpegDevice v;
    play(v);
    while (!field(v)) {
    }
    v.set_video_last_picture();
    regs_upd(v, 100);
    field(v, 0);
    CHECK(shown_x(v) == 100);
}

TEST_CASE("a scroll RegsUpd in vertical blanking first applies the pending picture update") {
    VmpegDevice v;
    regs_upd(v, 100);
    field(v, 0);  // pending, no picture
    REQUIRE(shown_x(v) == 0);
    regs_upd(v, 200, kScroll);  // line 0: vertical blanking
    CHECK(shown_x(v) == 100);
    field(v, 0);
    CHECK(shown_x(v) == 200);
}

TEST_CASE("without a pending RegsUpd vsync raises no VCUP") {
    VmpegDevice v;
    field(v, 0);
    CHECK((v.fmv_isr() & kIntVcup) == 0);
}

}  // TEST_SUITE
