// IMGSZ (0xe04002/4) is the next picture's size, once playing.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

namespace {

void declare_size(VmpegDevice& v, uint16_t w, uint16_t h) {
    write16(v, kFmvImageWidthOffset, w);
    write16(v, kFmvImageHeightOffset, h);
}

bool image_size_is(VmpegDevice& v, uint16_t w, uint16_t h) {
    return read16(v, kFmvImageWidthOffset) == w && read16(v, kFmvImageHeightOffset) == h;
}

}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("IMGSZ keeps fmvdrv's size until Play") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);  // Decoder On
    declare_size(v, 352, 224);
    v.set_next_picture(368, 272, 3);
    CHECK(image_size_is(v, 352, 224));
    write16(v, kFmvSyscmdOffset, 0x0008);  // Play
    CHECK(image_size_is(v, 352, 224));     // until the next field reports it
    v.set_next_picture(368, 272, 3);
    CHECK(image_size_is(v, 368, 272));
}

TEST_CASE("once playing IMGSZ follows the next picture") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    write16(v, kFmvSyscmdOffset, 0x0008);
    v.set_next_picture(368, 272, 3);
    CHECK(image_size_is(v, 368, 272));
    v.set_next_picture(352, 240, 3);
    CHECK(image_size_is(v, 352, 240));
}

TEST_CASE("Stop and Decoder Off hold IMGSZ again") {
    for (uint16_t cmd : {uint16_t(0x0080), uint16_t(0x2000)}) {
        CAPTURE(cmd);
        VmpegDevice v;
        write16(v, kFmvSyscmdOffset, 0x1000);
        write16(v, kFmvSyscmdOffset, 0x0008);
        write16(v, kFmvSyscmdOffset, cmd);
        declare_size(v, 352, 224);
        v.set_next_picture(368, 272, 3);
        CHECK(image_size_is(v, 352, 224));
    }
}

}  // TEST_SUITE
