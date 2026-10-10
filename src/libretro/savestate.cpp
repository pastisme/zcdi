#include "libretro/savestate.h"

#include <utility>

#include "audio/mixer.h"
#include "core/machine.h"
#include "core/state_archive.h"
#include "libretro/input_mapping.h"

namespace zcdi {

namespace {
// The frontend part, after the machine's ZCST section.
template <class A>
void io_frontend(A& archive, Mixer& audio, InputHistory& input) {
    audio.state_io(archive);
    input.state_io(archive);
}

size_t save_to(StateWriter& w, Machine& machine, Mixer& audio, InputHistory& input) {
    machine.save_state(w);
    io_frontend(w, audio, input);
    return w.failed() ? 0 : w.size();
}
} // namespace

void save_core_state(Machine& machine, Mixer& audio, InputHistory& input, std::vector<uint8_t>& out) {
    out.clear();
    StateWriter w(out);
    save_to(w, machine, audio, input);
}

size_t save_core_state(Machine& machine, Mixer& audio, InputHistory& input, std::span<uint8_t> out) {
    StateWriter w(out);
    return save_to(w, machine, audio, input);
}

size_t core_state_size(Machine& machine, Mixer& audio, InputHistory& input) {
    StateSizer sizer;
    io_frontend(sizer, audio, input);
    return machine.state_size() + sizer.size();
}

bool load_core_state(Machine& machine, Mixer& audio, InputHistory& input, std::span<const uint8_t> data) {
    // Read the frontend part (after ZCST) into copies before touching anything.
    StateReader frontend(data);
    frontend.skip_section(state_tag("ZCST"));
    Mixer audio_copy = audio;
    InputHistory input_copy = input;
    io_frontend(frontend, audio_copy, input_copy);
    if (frontend.failed()) return false;

    StateReader r(data);
    if (!machine.load_state(r)) return false;
    audio = std::move(audio_copy);
    input = input_copy;
    return true;
}

} // namespace zcdi
