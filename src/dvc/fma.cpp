#include "dvc/dvc.h"

#include <algorithm>

namespace zcdi {
namespace dvc {

using namespace vmpeg;
using namespace mpeg;

namespace {

// 10^(-db/20) * 256 * 9/16; >60 (incl. mute bit 7) is 0.
uint8_t db_to_linear(uint8_t db) {
    static constexpr uint8_t kTable[61] = {
        144, 128, 114, 102, 91, 81, 72, 64, 57, 51, 46, 41, 36, 32, 29, 26, 23, 20, 18, 16, 14,
        13,  11,  10,  9,   8,  7,  6,  6,  5,  5,  4,  4,  3,  3,  3,  2,  2,  2,  2,  1,  1,
        1,   1,   1,   1,   1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    };
    return db < 61 ? kTable[db] : 0;
}

} // namespace

void Dvc::handle_fma_cmd_write(uint16_t value) {
    if (value & kFmaCmdStartDma) {
        // Clears underflow (Green Book 8.2.4.3.3).
        fma_status_register_ &= ~kFmaStatusUnderflow;
        dma_for_fma_ = true;
        dma_active_ = true;
    }
    if (value & kFmaCmdStop) {
        fma_status_register_ = 0; // also ends decoding
        fma_es_bytes_.clear(); // input FIFO reset
        fma_sniff_bytes_.clear();
        fma_mpeg_audio_header_ = 0; // reset with it
        audio_reset_pending_ = true;
        fma_unplayed_samples_ = 0;
        reset_fma_playback_timer();
        fma_demux_mode_ = kDemuxModeUnknown;
        fma_demux_.reset();
        fma_ps_seen_ = false;
    }
}

void Dvc::handle_fma_dspd_write(uint8_t dspd) {
    const bool targeting_volume = (fma_dsp_mode_ == kDspModeVolume && fma_dsp_target_ == kDspTargetVolume);
    switch (fma_dspa_) {
        case 0: fma_dsp_mode_ = dspd; break;
        case 1:
            fma_dsp_target_ = dspd;
            // Strictly only when the target already held it; the same for well-formed sequences.
            if (fma_dsp_mode_ == kDspModeVolume && dspd == kDspTargetVolume) fma_dsp_write_index_ = 0;
            break;
        default: break;
    }
    if (targeting_volume && fma_dspa_ == kDspaVolume) {
        uint8_t linear = db_to_linear(dspd);
        switch (fma_dsp_write_index_) {
            case 0: mpeg_volume_r2r_ = linear; break;
            case 1: mpeg_volume_l2r_ = linear; break;
            case 2: mpeg_volume_r2l_ = linear; break;
            case 3: mpeg_volume_l2l_ = linear; break;
            default: break;
        }
        fma_dsp_write_index_ = uint8_t((fma_dsp_write_index_ + 1) & 0x3);
    }
}

void Dvc::set_audio_program_end() {
    fma_interrupt_status_register_ |= kFmaIntIsoEnd;
    fma_status_register_ |= kFmaStatusFinished;
}

void Dvc::set_audio_decoding_started() {
    fma_status_register_ |= kFmaStatusDecoding;
    fma_interrupt_status_register_ |= kFmaIntDecoding;
}

void Dvc::set_audio_underflow() {
    fma_status_register_ |= kFmaStatusUnderflow;
    fma_interrupt_status_register_ |= kFmaIntUnf;
    fma_status_register_ &= ~kFmaStatusDecoding;
    reset_fma_playback_timer();
}

// UNF at the first field ending with the decoded audio played out.
void Dvc::audio_field(int32_t decoded, int32_t field_samples) {
    const bool played_out = fma_unplayed_samples_ <= field_samples;
    fma_unplayed_samples_ = std::max(0, fma_unplayed_samples_ - field_samples) + decoded;
    if (audio_dsp_enabled() && decoded == 0 && played_out) {
        fma_unplayed_samples_ = 0;
        set_audio_underflow();
    }
}

void Dvc::reset_fma_playback_timer() { fma_start_valid_ = false; }

void Dvc::fma_input_byte(uint8_t b) {
    fma_demux_byte(b);
    if (fma_demux_mode_ == kDemuxModeElementary) {
        fma_es_bytes_.push_back(b);
    } else if (fma_demux_mode_ == kDemuxModeUnknown) {
        // No PACK within the first sectors: a bare audio elementary stream.
        fma_sniff_bytes_.push_back(b);
        if (fma_sniff_bytes_.size() >= kMaxFmaPsSniffBytes) {
            fma_demux_mode_ = kDemuxModeElementary;
            fma_es_bytes_.insert(fma_es_bytes_.end(), fma_sniff_bytes_.begin(), fma_sniff_bytes_.end());
            fma_sniff_bytes_.clear();
        }
    }
}

void Dvc::fma_demux_byte(uint8_t b) {
    PsDemux& d = fma_demux_;
    if (d.state == PsDemux::kScan) {
        if (!d.scan(b)) return;
        if (b == kPackStart) {
            fma_ps_seen_ = true;
            if (fma_demux_mode_ == kDemuxModeUnknown) {
                fma_demux_mode_ = kDemuxModeProgram;
                fma_sniff_bytes_.clear();
            }
        } else if (b == kProgramEnd) {
            if (fma_demux_mode_ == kDemuxModeProgram) set_audio_program_end();
        }
        d.begin(b);
        return;
    }
    const uint8_t events = d.step(b, uint8_t(kAudioStream + (fma_stream_number_ & 0xf)));
    if ((events & PsDemux::kPts) && !fma_start_valid_) {
        fma_start_dclk_ = fma_dclk_ + uint32_t(d.timestamp() >> 1) - uint32_t(d.scr >> 1);
        fma_start_valid_ = true;
    }
    if ((events & PsDemux::kPayloadByte) && fma_demux_mode_ == kDemuxModeProgram) fma_es_bytes_.push_back(b);
}

void Dvc::set_audio_frame_decoded(uint32_t header) {
    fma_status_register_ |= kFmaStatusFrame;
    fma_interrupt_status_register_ |= kFmaIntFrame;
    // CSU follows the pending flag, so an unread one clears.
    fma_interrupt_status_register_ = (fma_interrupt_status_register_ & ~unsigned(kFmaIntCsu)) |
                                     (fma_stream_change_pending_ ? unsigned(kFmaIntCsu) : 0u);
    fma_stream_change_pending_ = false;
    if (header != 0) fma_mpeg_audio_header_ = header;
}

} // namespace dvc
} // namespace zcdi
