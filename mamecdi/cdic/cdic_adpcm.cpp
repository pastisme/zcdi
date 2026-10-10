#include "cdic/cdic_adpcm.h"

#include "core/util.h"

namespace zcdi {
namespace mamecdi {

namespace {
// Filter gains K0/K1 in 64ths (Green Book Figure IV.15: 0.9375 = 60/64, ...).
constexpr int32_t kFilterK0[4] = {0, 60, 115, 98};
constexpr int32_t kFilterK1[4] = {0, 0, -52, -55};

// Range domain and shift are both 16 - width (Green Book IV.5.3, Figure IV.16).
int16_t decode_one(uint8_t param, int32_t raw, int width, History& hist) {
    int filter = (param >> 4) & 3;
    int range = param & 0xf;
    int shift_base = 16 - width;
    if (range > shift_base) range = shift_base; // real gear's valid domain; real content never encodes higher
    int32_t signed_val = raw;
    int32_t sign_bit = 1 << (width - 1);
    if (signed_val & sign_bit) signed_val -= (int32_t(1) << width);
    // Division, not >>6: truncates toward zero.
    int32_t predicted = (kFilterK0[filter] * hist.s0 + kFilterK1[filter] * hist.s1 + 32) / 64;
    int32_t v = clamp16((signed_val << (shift_base - range)) + predicted);
    hist.s1 = hist.s0;
    hist.s0 = v;
    return static_cast<int16_t>(v);
}

constexpr int kGroupSize = 128;
constexpr int kGroupsPerSector = 18;
constexpr int kSamplesPerUnit = 28;

// Sound parameter bytes per sound unit, Green Book Figure IV.9.
constexpr int kHeaderOffsetCh0[4] = {0, 2, 8, 10};
constexpr int kHeaderOffsetCh1[4] = {1, 3, 9, 11};

// Green Book Figure IV.10. Each channel finishes a block before the next (history order).
void decode_group_4bit_stereo(const uint8_t* group, History hist[2], std::vector<int16_t>& out) {
    int16_t ch0[4][kSamplesPerUnit];
    int16_t ch1[4][kSamplesPerUnit];
    for (int p = 0; p < 4; ++p) {
        uint8_t param0 = group[kHeaderOffsetCh0[p]];
        uint8_t param1 = group[kHeaderOffsetCh1[p]];
        for (int s = 0; s < kSamplesPerUnit; ++s) {
            uint8_t data_byte = group[16 + p + s * 4];
            ch0[p][s] = decode_one(param0, data_byte & 0xf, 4, hist[0]);
            ch1[p][s] = decode_one(param1, (data_byte >> 4) & 0xf, 4, hist[1]);
        }
    }
    for (int p = 0; p < 4; ++p) {
        for (int s = 0; s < kSamplesPerUnit; ++s) {
            out.push_back(ch0[p][s]);
            out.push_back(ch1[p][s]);
        }
    }
}

// Green Book IV.3.6: one history across SU0..SU7 in order, low nibble unit first.
void decode_group_4bit_mono(const uint8_t* group, History& hist, std::vector<int16_t>& out) {
    for (int p = 0; p < 4; ++p) {
        uint8_t param_lo = group[kHeaderOffsetCh0[p]]; // unit 2p
        uint8_t param_hi = group[kHeaderOffsetCh1[p]]; // unit 2p+1
        for (int s = 0; s < kSamplesPerUnit; ++s) {
            out.push_back(decode_one(param_lo, group[16 + p + s * 4] & 0xf, 4, hist));
        }
        for (int s = 0; s < kSamplesPerUnit; ++s) {
            out.push_back(decode_one(param_hi, (group[16 + p + s * 4] >> 4) & 0xf, 4, hist));
        }
    }
}

// Green Book Figure IV.7.
constexpr int kHeaderOffsetLevelA[4] = {0, 1, 2, 3};

// Green Book IV.3.5, Figure IV.8: left SU0/SU2, right SU1/SU3.
void decode_group_8bit_stereo(const uint8_t* group, History hist[2], std::vector<int16_t>& out) {
    for (int p = 0; p < 2; ++p) {
        uint8_t param_l = group[kHeaderOffsetLevelA[p * 2]]; // unit 2p (left)
        uint8_t param_r = group[kHeaderOffsetLevelA[p * 2 + 1]]; // unit 2p+1 (right)
        for (int s = 0; s < kSamplesPerUnit; ++s) {
            uint8_t data_l = group[16 + (p * 2) + s * 4];
            uint8_t data_r = group[16 + (p * 2 + 1) + s * 4];
            out.push_back(decode_one(param_l, data_l, 8, hist[0]));
            out.push_back(decode_one(param_r, data_r, 8, hist[1]));
        }
    }
}

// Green Book IV.3.5: one history across SU0..SU3.
void decode_group_8bit_mono(const uint8_t* group, History& hist, std::vector<int16_t>& out) {
    for (int j = 0; j < 4; ++j) {
        uint8_t param = group[kHeaderOffsetLevelA[j]];
        for (int s = 0; s < kSamplesPerUnit; ++s) {
            out.push_back(decode_one(param, group[16 + j + s * 4], 8, hist));
        }
    }
}

// IEC 60908 50/15 us de-emphasis (Green Book IV.4.2), bilinear biquad.
void apply_deemphasis(std::vector<int16_t>& pcm, int channels, int sample_rate) {
    constexpr double kTau1 = 50e-6, kTau2 = 15e-6;
    double k = 2.0 * sample_rate;
    double b0 = (1.0 + kTau2 * k) / (1.0 + kTau1 * k);
    double b1 = (1.0 - kTau2 * k) / (1.0 + kTau1 * k);
    double a1 = (1.0 - kTau1 * k) / (1.0 + kTau1 * k);
    double x1[2] = {0, 0}, y1[2] = {0, 0};
    for (std::size_t i = 0; i + std::size_t(channels) <= pcm.size(); i += channels) {
        for (int c = 0; c < channels; ++c) {
            double x0 = pcm[i + c];
            double y0 = b0 * x0 + b1 * x1[c] - a1 * y1[c];
            pcm[i + c] = clamp16(static_cast<int32_t>(y0 + (y0 >= 0 ? 0.5 : -0.5)));
            x1[c] = x0;
            y1[c] = y0;
        }
    }
}
} // namespace

bool decode_xa_adpcm(const uint8_t* sector_audio_data, uint8_t coding, std::vector<int16_t>& out, History history[2]) {
    int bits_per_sample = (coding >> 4) & 3; // 0=4bit(Level B/C), 1=8bit(Level A), 2/3=reserved
    int rate_sel = (coding >> 2) & 3; // 0=37.8kHz, 1=18.9kHz
    int channels = coding & 3; // 0=mono, 1=stereo
    bool emphasis = (coding & 0x40) != 0; // bit 6, Green Book IV.3.2.4/IV.4.2

    if (bits_per_sample > 1 || channels > 1) return false; // reserved codings unsupported
    if (rate_sel != 0 && rate_sel != 1) return false;
    bool is_8bit = (bits_per_sample == 1);
    bool is_mono = (channels == 0);

    // Mono uses history[0] only (same_cdi hardcodes channel 0).
    std::vector<int16_t> native; // decoded at the coding's own native rate, mono or L,R,L,R...
    if (is_mono) {
        for (int g = 0; g < kGroupsPerSector; ++g) {
            const uint8_t* group = sector_audio_data + g * kGroupSize;
            if (is_8bit)
                decode_group_8bit_mono(group, history[0], native);
            else
                decode_group_4bit_mono(group, history[0], native);
        }
    } else {
        for (int g = 0; g < kGroupsPerSector; ++g) {
            const uint8_t* group = sector_audio_data + g * kGroupSize;
            if (is_8bit)
                decode_group_8bit_stereo(group, history, native);
            else
                decode_group_4bit_stereo(group, history, native);
        }
    }

    int native_channels = is_mono ? 1 : 2;
    int native_rate = (rate_sel == 1) ? 18900 : 37800;
    if (emphasis) apply_deemphasis(native, native_channels, native_rate);

    // Mono is duplicated to both channels (zcdi's choice, not spec).
    bool upsample = (rate_sel == 1);
    std::size_t frames = native.size() / native_channels;
    out.reserve(out.size() + frames * 2 * (upsample ? 2 : 1));
    for (std::size_t f = 0; f < frames; ++f) {
        int16_t l = native[f * native_channels];
        int16_t r = is_mono ? l : native[f * native_channels + 1];
        out.push_back(l);
        out.push_back(r);
        if (upsample) {
            out.push_back(l);
            out.push_back(r);
        }
    }
    return true;
}

void resample_adpcm_to_44100(const std::vector<int16_t>& in_37800hz, std::vector<int16_t>& out_44100hz,
                             AdpcmResampleState& state) {
    if (in_37800hz.empty()) return;

    std::vector<int16_t> combined;
    combined.reserve(state.carry.size() + in_37800hz.size());
    combined.insert(combined.end(), state.carry.begin(), state.carry.end());
    combined.insert(combined.end(), in_37800hz.begin(), in_37800hz.end());

    std::size_t frames = combined.size() / 2;
    if (frames < 2) { // not enough to interpolate a single output sample yet
        state.carry = combined;
        return;
    }

    constexpr double kStep = 37800.0 / 44100.0;
    double pos = state.phase;
    while (true) {
        std::size_t i0 = static_cast<std::size_t>(pos);
        if (i0 + 1 >= frames) break; // would need an input frame we don't have yet
        double frac = pos - double(i0);
        int16_t l =
            clamp16(static_cast<int32_t>(combined[i0 * 2] + frac * (combined[(i0 + 1) * 2] - combined[i0 * 2])));
        int16_t r = clamp16(
            static_cast<int32_t>(combined[i0 * 2 + 1] + frac * (combined[(i0 + 1) * 2 + 1] - combined[i0 * 2 + 1])));
        out_44100hz.push_back(l);
        out_44100hz.push_back(r);
        pos += kStep;
    }

    std::size_t consumed_frames = static_cast<std::size_t>(pos);
    if (consumed_frames >= frames) consumed_frames = frames - 1; // always keep >=1 frame for next call's interpolation
    state.phase = pos - double(consumed_frames);
    state.carry.assign(combined.begin() + consumed_frames * 2, combined.end());
}

} // namespace mamecdi
} // namespace zcdi
