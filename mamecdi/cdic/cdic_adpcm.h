#pragma once
#include <cstdint>
#include <vector>

namespace zcdi {
namespace mamecdi {

// One pair for the CDIC's life (same_cdi m_xa_last); pass the same one to every call.
struct History {
    int32_t s0 = 0, s1 = 0;
};

// Green Book XA ADPCM (Level A/B/C). Appends
// interleaved stereo S16 at 37800 Hz; false for 16-bit/MPEG codings.
bool decode_xa_adpcm(const uint8_t* sector_audio_data, uint8_t coding, std::vector<int16_t>& out, History history[2]);

struct AdpcmResampleState {
    std::vector<int16_t> carry; // trailing not-yet-fully-consumed input frames (interleaved stereo)
    double phase = 0.0; // fractional position (in input-frame units, always in [0,1)) into carry[0]/carry[1]'s pair
};

// Linear interpolation at the exact 6:7 ratio, so no long-term drift.
void resample_adpcm_to_44100(const std::vector<int16_t>& in_37800hz, std::vector<int16_t>& out_44100hz,
                             AdpcmResampleState& state);

} // namespace mamecdi
} // namespace zcdi
