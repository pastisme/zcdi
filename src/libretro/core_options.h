#pragma once
#include "libretro/libretro.h"

#include "slave/slave.h" // VideoStandard

namespace zcdi {
void register_core_options(retro_environment_t env);

enum class NvramStorageMode {
    kPerDisc,
    kGlobal,
};

enum class DvcMode {
    kAuto,
    kOff,
};

NvramStorageMode get_nvram_storage_mode(retro_environment_t env);
DvcMode get_dvc_mode(retro_environment_t env);

// The playcdi.mod overlay (Bus::set_auto_play_enabled). Off by default.
bool get_auto_play_enabled(retro_environment_t env);

enum LibretroRegion : unsigned {
    kNtsc = RETRO_REGION_NTSC,
    kPal = RETRO_REGION_PAL,
};

// Picks the BIOS file and timing. NTSC by default.
LibretroRegion get_preferred_region(retro_environment_t env);

// libretro's regions are 0/1, the SLAVE's standards 1/2.
constexpr VideoStandard to_video_standard(LibretroRegion region) { return static_cast<VideoStandard>(region + 1); }
static_assert(to_video_standard(kNtsc) == VideoStandard::kNtsc);
static_assert(to_video_standard(kPal) == VideoStandard::kPal);
} // namespace zcdi
