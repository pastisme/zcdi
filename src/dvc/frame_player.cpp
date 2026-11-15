#include "dvc/dvc.h"

#include <algorithm>

namespace zcdi {
namespace dvc {

using namespace vmpeg;

namespace {

// Pack SCR past the head picture's DTS by more than this (90 kHz): play at the shortest period.
constexpr int64_t kOutOfSyncThreshold = 15000;
// Pictures without a DTS keep the last desync for this many pictures.
constexpr uint8_t kDesyncHoldPictures = 3;

// dclk[20:6] - DTS[21:7], 15-bit signed.
int dts_desync(uint32_t dclk, int64_t dts) {
    int d = int((dclk >> 6) & 0x7fff) - int((uint64_t(dts) >> 7) & 0x7fff);
    d &= 0x7fff;
    return d >= 0x4000 ? d - 0x8000 : d;
}

} // namespace

void Dvc::notify_vsync() {
    // Each call is one vsync edge.
    fmv_interrupt_status_register_ |= kIntVsync;
    fmv_line_ = 0;
    ndat_line_pending_ = false;
    if (!register_update_latch_) return;

    // Every RegsUpd is taken at vsync, with VCUP and DCL.
    register_update_latch_ = false;
    fmv_interrupt_status_register_ |= kIntVcup | kIntDcl;
    if (register_update_scroll_) {
        latched_window_ = live_window();
        window_update_.deferred_pending = false;
        window_update_.next_pending = false;
        window_update_.next_late = false;
    } else { // waits for the next picture
        window_update_.next = live_window();
        window_update_.next_pending = true;
        window_update_.next_late = window_update_.late;
    }
}

void Dvc::apply_window_updates(bool picture_selected) {
    // After the last picture, updates go on at vsync: no picture will take them.
    const bool held = !picture_selected && window_update_.last_picture_held;
    if (picture_selected || !fmv_decoder_active_) window_update_.last_picture_held = false;
    if (!picture_selected && !held) return;
    if (window_update_.deferred_pending) {
        latched_window_ = window_update_.deferred;
        window_update_.deferred_pending = false;
    }
    if (window_update_.next_pending) {
        // Written after the window started scanning out: keep it for the picture after.
        if (picture_selected && window_update_.next_late) {
            window_update_.deferred = window_update_.next;
            window_update_.deferred_pending = true;
        } else {
            latched_window_ = window_update_.next;
        }
        window_update_.next_pending = false;
    }
}

bool Dvc::in_vertical_blank() const {
    return fmv_line_ < fmv_first_active_line_ || fmv_line_ >= fmv_first_active_line_ + fmv_active_lines_;
}

// The window's first line has been drawn this field.
bool Dvc::window_scan_started() const {
    return fmv_show_on_next_video_frame_ && !in_vertical_blank() &&
           fmv_line_ >= fmv_first_active_line_ + int(latched_window_.display_offset_x);
}

void Dvc::end_of_line(int line, int first_active_line, int active_lines) {
    fmv_line_ = line + 1;
    fmv_first_active_line_ = first_active_line;
    fmv_active_lines_ = active_lines;
    if (ndat_line_pending_ && fmv_line_ == 11) {
        ndat_line_pending_ = false;
        fmv_interrupt_status_register_ |= kIntNdat;
    }
    // The picture starts display with the first active line.
    if (line + 1 == first_active_line && fmv_display_events_pending_ != 0) {
        fmv_interrupt_status_register_ |= fmv_display_events_pending_;
        fmv_display_events_pending_ = 0;
    }
}

// SCRPOS X and Y are latched swapped, 9 bits each.
FmvWindow Dvc::live_window() const {
    return {
        .display_offset_y = uint16_t(video_ctrl_x_display_ & 0x1ff),
        .display_offset_x = uint16_t(video_ctrl_y_display_ & 0x1ff),
        .window_offset_y = uint16_t(video_ctrl_decoder_offset_y_ & 0x1ff),
        .window_offset_x = uint16_t(video_ctrl_decoder_offset_x_ & 0x1ff),
        .width = uint16_t(video_ctrl_window_width_ & 0x1ff),
        .height = uint16_t(video_ctrl_window_height_ & 0x1ff),
    };
}

void Dvc::set_next_picture(int width, int height, int picture_rate_code) {
    if (!fmv_decoder_active_) return;
    image_width_ = uint16_t(width);
    image_height_ = uint16_t(height);
    image_rt_ = uint16_t(picture_rate_code & 0xff);
}

void Dvc::set_video_frame_ready(int width, int height) {
    fmv_display_events_pending_ |= kIntPic;
    display_width_ = uint16_t(width);
    display_height_ = uint16_t(height);
}

void Dvc::set_video_frame_decoded(int picture_rate_code) {
    // 90000 / picture rate, indexed by the sequence header's picture_rate code.
    static constexpr uint16_t kPeriod90khz[16] = {
        100, 3753, 3750, 3600, 3003, 3000, 1800, 1501, 1500, 100, 100, 100, 100, 100, 100, 100,
    };
    // The same period in 30 MHz clocks.
    static constexpr uint32_t kPeriod30mhz[16] = {
        10000,  1251251, 1250000, 1200000, 1001001, 1000000, 600000, 500500,
        500000, 10000,   10000,   10000,   10000,   10000,   10000,  10000,
    };
    player_frame_period_ = kPeriod30mhz[picture_rate_code & 0xf];
    if (!fmv_dsp_enable_) return;
    decoder_frameperiod_90khz_ = kPeriod90khz[picture_rate_code & 0xf];
}

void Dvc::check_buffer_underflow(size_t output_pictures, int decoder_pictures) {
    // The DTS FIFO holds input + decoder + output, so the input count includes the picture being decoded.
    const int input_pictures = int(fmv_dts_fifo_.size()) - int(output_pictures) - decoder_pictures;
    if (fmv_playback_active_ && ndat_armed_ && output_pictures <= 1 && decoder_pictures < 2 && input_pictures <= 1) {
        ndat_armed_ = false;
        ndat_line_pending_ = true;
    }
}

void Dvc::set_video_gop_start() { fmv_display_events_pending_ |= kIntGop; }

void Dvc::set_video_sequence_start() { fmv_display_events_pending_ |= kIntSeq; }

void Dvc::set_video_sequence_end() { fmv_interrupt_status_register_ |= kIntEsi; }

void Dvc::set_video_program_end() { fmv_interrupt_status_register_ |= kIntEii; }

void Dvc::set_video_last_picture() {
    fmv_display_events_pending_ |= kIntEod;
    window_update_.last_picture_held = true;
}

void Dvc::set_video_decoder_timecode(uint32_t timecode) {
    if (fmv_dsp_enable_) fmv_decoder_timecode_ = timecode;
}

void Dvc::set_video_display_timecode(uint32_t timecode) {
    if (fmv_dsp_enable_) fmv_display_timecode_ = timecode;
}

void Dvc::set_video_display_status(uint8_t status) {
    if (fmv_dsp_enable_) fmv_display_video_status_ = status;
}

void Dvc::set_video_display_picture_rate(int picture_rate_code) {
    if (fmv_dsp_enable_) display_frameperiod_rawhdr_ = uint8_t(picture_rate_code & 0xff);
}

bool Dvc::video_display_enabled() {
    if (!fmv_playback_active_) {
        fmv_dts_start_satisfied_ = false;
        return false;
    }
    if (!fmv_dts_start_satisfied_) {
        bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
        if (!head_has_dts || dts_desync(fmv_dclk_, fmv_dts_fifo_.front()) > -30) fmv_dts_start_satisfied_ = true;
    }
    bool synchronous = (fmv_system_control_register_ & kSysScrSynchronous) != 0;
    return !synchronous || fmv_dts_start_satisfied_;
}

void Dvc::note_picture_displayed() {
    if (fmv_dts_fifo_.empty()) return;
    // Predicted DTS: the last DTS plus a picture period per picture after it.
    const int64_t head = fmv_dts_fifo_.front();
    if (head != 0 && decoder_frameperiod_90khz_ != 0) {
        player_predicted_dts_ = head + decoder_frameperiod_90khz_;
        player_predicted_dts_valid_ = true;
    } else if (player_predicted_dts_valid_ && decoder_frameperiod_90khz_ != 0) {
        player_predicted_dts_ += decoder_frameperiod_90khz_;
    } else {
        player_predicted_dts_valid_ = false;
    }
    fmv_dts_fifo_.erase(fmv_dts_fifo_.begin());
}

// Its DTS leaves the DTS FIFO like a shown picture's.
void Dvc::note_picture_discarded() { note_picture_displayed(); }

bool Dvc::frame_player_field(uint32_t field_clocks, size_t pictures_ready) {
    bool show = false;
    const bool counting = video_display_enabled(); // also keeps the start latch
    // Step: armed at one vsync, shown at the next, playing or not.
    auto arm_step = [&] {
        if (player_step_latch_ && pictures_ready > size_t(show)) {
            player_step_latch_ = false;
            player_latch_pending_ = true;
        }
    };
    // Playback starting arms NDAT; nothing disarms it but NDAT.
    if (fmv_playback_active_ && !player_playback_active_delayed_) ndat_armed_ = true;
    if (!fmv_playback_active_) {
        player_frame_cnt_ = 0;
        show = player_latch_pending_ && pictures_ready > 0;
        player_latch_pending_ = false;
        apply_window_updates(show);
        arm_step();
        return show;
    }
    const bool synchronous = (fmv_system_control_register_ & kSysScrSynchronous) != 0;
    const bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
    // Far out of sync: only forces the shortest period.
    const bool out_of_sync =
        head_has_dts && fmv_demux_scr_valid_ && fmv_demux_.scr - fmv_dts_fifo_.front() > kOutOfSyncThreshold;
    if (counting) {
        player_frame_cnt_ += field_clocks;
        for (;;) {
            // Slow motion multiplies the period.
            const int64_t period = int64_t(player_frame_period_) * (fmv_slow_motion_ + 1);
            // Speed control stays within 1/8 of the period, as measured on a VMPEG.
            const int64_t shortest = period - period / 8, longest = period + period / 8;
            int64_t top = period - 1 - int64_t(player_desync_q_) * 2048;
            if (top < shortest || out_of_sync) top = shortest;
            if (top > longest) top = longest;
            if (top < 1) top = 1;
            if (player_frame_cnt_ < top) break;
            player_frame_cnt_ -= top + 1;
            if (period <= 1200) continue; // no picture latched at periods this short
            player_latch_pending_ = true;
            player_playback_active_delayed_ = true;
            if (pictures_ready == 0) continue; // desync is measured only with a picture waiting
            // dclk at the wrap, player_frame_cnt_ 30 MHz clocks before the field end.
            const uint32_t dclk_at_wrap = fmv_dclk_ - uint32_t(player_frame_cnt_ * 3 / 2000);
            update_player_desync(dclk_at_wrap, synchronous);
        }
    }
    if (player_latch_pending_) {
        player_latch_pending_ = false;
        show = pictures_ready > 0;
    }
    apply_window_updates(show);
    arm_step();
    return show;
}

// Desync at a picture period wrap, against the head's DTS
// or, without one, the predicted DTS; a measurement holds for 3 more pictures.
void Dvc::update_player_desync(uint32_t dclk, bool synchronous) {
    const bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
    const bool use_predicted = !fmv_dts_fifo_.empty() && !head_has_dts && player_predicted_dts_valid_;
    const bool syncing = synchronous && fmv_slow_motion_ == 0; // no sync in slow motion
    if (syncing && (head_has_dts || use_predicted)) {
        player_desync_q_ = dts_desync(dclk, head_has_dts ? fmv_dts_fifo_.front() : player_predicted_dts_);
        player_desync_hold_ = kDesyncHoldPictures;
    } else if (syncing && player_desync_hold_ > 0) {
        --player_desync_hold_;
    } else {
        player_desync_q_ = 0;
    }
}

Dvc::DisplayWindow Dvc::display_window() const {
    // display_offset_y holds X.
    const FmvWindow& w = latched_window_;
    return {
        .scrpos_x = w.display_offset_y,
        .scrpos_y = w.display_offset_x,
        .decwin_w = w.width,
        .decwin_h = w.height,
        .decoff_x = w.window_offset_x,
        .decoff_y = w.window_offset_y,
        .show = fmv_show_on_next_video_frame_,
        .vcd_pixel_clock = vcd_pixel_clock_,
    };
}

} // namespace dvc
} // namespace zcdi
