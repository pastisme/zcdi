// What the frontend takes from a Machine each frame: the picture and the audio.
#include "doctest.h"

#include <memory>

#include "chips/mcd212/mcd212.h"
#include "core/machine.h"
#include "core/machine_inspector.h"
#include "support/fake_machine.h"

TEST_SUITE("core") {

// Datasheet Table 5-4: without CF, 360 Normal Resolution pixels a line;
// PAL without FD or ST: 280 lines. The buffer holds two woven fields.
TEST_CASE("the video frame is the MCD212's picture with its shown area") {
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    m->run_frame();
    const zcdi::Machine::VideoFrame frame = m->video_frame();
    const zcdi::MachineInspector view(*m);
    CHECK(frame.pixels == view.mcd212().framebuffer());
    CHECK(frame.width == 720);
    CHECK(frame.height == 280);
    CHECK(frame.pitch == 768);
    CHECK(frame.buffer_height == 560);
    CHECK(frame.buffer_height >= frame.height);
    // A 28 MHz line's 360 pixels against 52.4 us of 4:3 screen and 287.5 of its lines (Green Book V.2.4.1).
    const double expected = 4.0 / 3.0 * (360 * 4 / 1792.0 * 64.0 / 52.4) * (287.5 / 280);
    CHECK(frame.display_aspect == doctest::Approx(expected));
}

TEST_CASE("with no disc and no cartridge, a frame's audio is empty") {
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    m->run_frame();
    const zcdi::AudioStreams audio = m->drain_audio();
    CHECK(audio.cdic.empty());
    CHECK(audio.dvc.empty());
}

}  // TEST_SUITE
