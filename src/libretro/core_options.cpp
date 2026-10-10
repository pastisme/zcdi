#include "core_options.h"

#include <string_view>

namespace zcdi {
namespace {
constexpr const char* kNvramModeKey = "zcdi_nvram_mode";
constexpr const char* kPreferredRegionKey = "zcdi_preferred_bios";
constexpr const char* kDvcModeKey = "zcdi_dvc_enabled";
constexpr const char* kAutoPlayKey = "zcdi_auto_play";

struct retro_core_option_v2_definition kOptionDefinitions[] = {
    {kNvramModeKey,
     "NVRAM Storage Mode",
     nullptr,
     "Per Disc: the frontend saves the player's NVRAM (settings, game saves) as each disc's save file. Global: one "
     "NVRAM file per BIOS in the save directory, shared by every disc, like a real player's NVRAM. Takes effect when "
     "content is next loaded.",
     nullptr,
     nullptr,
     {
         {"per_disc", "Per Disc (Default)"},
         {"global", "Global"},
         {nullptr, nullptr},
     },
     "per_disc"},
    {kPreferredRegionKey,
     "Video Standard",
     nullptr,
     "Select NTSC or PAL for BIOS file selection and frame timing. Takes effect when content is next loaded.",
     nullptr,
     nullptr,
     {
         {"ntsc", "NTSC (Default)"},
         {"pal", "PAL"},
         {nullptr, nullptr},
     },
     "ntsc"},
    {kDvcModeKey,
     "DVC Enabled",
     nullptr,
     "Enable optional DVC cartridge ROM auto-loading (vmpega.rom) from the system directory. Takes effect when content "
     "is next loaded.",
     nullptr,
     nullptr,
     {
         {"auto", "Auto (Default)"},
         {"off", "Off"},
         {nullptr, nullptr},
     },
     "auto"},
    {kAutoPlayKey,
     "Auto Play",
     nullptr,
     "Automatically click \"PLAY CD-I\" on the boot menu instead of waiting for the player to press it. Takes effect "
     "when content is next loaded.",
     nullptr,
     nullptr,
     {
         {"off", "Off (Default)"},
         {"on", "On"},
         {nullptr, nullptr},
     },
     "off"},
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
};

struct retro_core_options_v2 kOptionsUs = {
    nullptr,
    kOptionDefinitions,
};

struct retro_core_options_v2_intl kOptionsIntl = {
    &kOptionsUs,
    nullptr,
};

const struct retro_variable kLegacyOptions[] = {
    {kNvramModeKey, "NVRAM Storage Mode; per_disc|global"},
    {kPreferredRegionKey, "Video Standard; ntsc|pal"},
    {kDvcModeKey, "DVC Enabled; auto|off"},
    {kAutoPlayKey, "Auto Play; off|on"},
    {nullptr, nullptr},
};

// Empty when unset or without a frontend; valid until the next GET_VARIABLE.
std::string_view option_value(retro_environment_t env, const char* key) {
    struct retro_variable var = {key, nullptr};
    if (!env || !env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return {};
    return var.value;
}
} // namespace

void register_core_options(retro_environment_t env) {
    if (!env) return;

    unsigned version = 0;
    if (env(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
        env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &kOptionsIntl);
        return;
    }

    env(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)kLegacyOptions);
}

NvramStorageMode get_nvram_storage_mode(retro_environment_t env) {
    return option_value(env, kNvramModeKey) == "global" ? NvramStorageMode::kGlobal : NvramStorageMode::kPerDisc;
}

LibretroRegion get_preferred_region(retro_environment_t env) {
    return option_value(env, kPreferredRegionKey) == "pal" ? kPal : kNtsc;
}

DvcMode get_dvc_mode(retro_environment_t env) {
    return option_value(env, kDvcModeKey) == "off" ? DvcMode::kOff : DvcMode::kAuto;
}

bool get_auto_play_enabled(retro_environment_t env) { return option_value(env, kAutoPlayKey) == "on"; }
} // namespace zcdi
