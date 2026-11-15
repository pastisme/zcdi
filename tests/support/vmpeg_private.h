#pragma once
// VMPEG state the timing tests set directly, where reaching it through
// registers and streams would take a whole demuxed program stream. Read
// state through VmpegDevice::state() instead.
#include <cstdint>
#include <vector>

#include "chips/vmpeg/vmpeg.h"
#include "support/private_access.h"

ZCDI_PRIVATE_MEMBER(VmpegPlaybackActive, zcdi::chips::VmpegSaveState, fmv_playback_active_, bool);
ZCDI_PRIVATE_MEMBER(VmpegFmvDclk, zcdi::chips::VmpegSaveState, fmv_dclk_, uint32_t);
ZCDI_PRIVATE_MEMBER(VmpegDtsFifo, zcdi::chips::VmpegSaveState, fmv_dts_fifo_, std::vector<int64_t>);
ZCDI_PRIVATE_MEMBER(VmpegFmvDemux, zcdi::chips::VmpegSaveState, fmv_demux_, zcdi::chips::PsDemux);
ZCDI_PRIVATE_MEMBER(VmpegScrValid, zcdi::chips::VmpegSaveState, fmv_demux_scr_valid_, bool);
ZCDI_PRIVATE_MEMBER(VmpegStartSatisfied, zcdi::chips::VmpegSaveState, fmv_dts_start_satisfied_, bool);

namespace zcdi_test {

// As if the Play delay had elapsed (or Stop/Pause had taken effect).
inline void set_playback_active(zcdi::chips::VmpegDevice& v, bool active) { priv<VmpegPlaybackActive>(v) = active; }

inline void set_fmv_dclk(zcdi::chips::VmpegDevice& v, uint32_t dclk) { priv<VmpegFmvDclk>(v) = dclk; }

// One DTS (90 kHz, 0 = none) per picture not yet shown.
inline std::vector<int64_t>& dts_fifo(zcdi::chips::VmpegDevice& v) { return priv<VmpegDtsFifo>(v); }

// The last pack SCR the FMV demuxer read (90 kHz).
inline void set_demux_scr(zcdi::chips::VmpegDevice& v, int64_t scr) {
    priv<VmpegFmvDemux>(v).scr = scr;
    priv<VmpegScrValid>(v) = true;
}

// Past the wait for the first picture's DTS.
inline void set_dts_start_satisfied(zcdi::chips::VmpegDevice& v) { priv<VmpegStartSatisfied>(v) = true; }

}  // namespace zcdi_test
