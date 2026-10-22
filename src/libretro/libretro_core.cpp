#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "libretro/libretro.h"

#include "audio/mixer.h"
#include "core/machine.h"
#include "core/machine_inspector.h"
#include "core/version.h"
#include "core_options.h"
#include "disk_control.h"
#include "helpers.h"
#include "input_mapping.h"
#include "savestate.h"

// The C ABI boundary: deliberately outside namespace zcdi, calls stay qualified.
namespace {

constexpr int kWidth = 768; // Double Resolution pixels, see Mcd212::kFbWidth
constexpr int kHeight = 280;
constexpr int kMaxHeight = 560; // interlaced fields woven, see Mcd212::framebuffer()
constexpr float kAspect = 4.0f / 3.0f; // as CDi_MiSTer's scaler shows it
constexpr int kSampleRate = 44100;

retro_environment_t env_cb = nullptr;
const struct retro_vfs_interface* vfs_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t audio_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t input_poll_cb = nullptr;
retro_input_state_t input_state_cb = nullptr;

std::unique_ptr<zcdi::Machine> g_machine;
// Last reported through SET_GEOMETRY.
int g_out_width = kWidth;
int g_out_height = kHeight;
std::unique_ptr<zcdi::Mixer> g_audio;

struct RegionProfile {
    const char* bios_filename;
    zcdi::LibretroRegion region;

    zcdi::VideoStandard video_standard() const { return zcdi::to_video_standard(region); }
    // Known before the machine exists, as retro_get_system_av_info() needs it.
    double refresh_rate() const { return zcdi::refresh_rate(video_standard()); }
};

// Indexed by LibretroRegion.
constexpr std::array<RegionProfile, 2> kRegionProfiles = {{
    {"cdi200.rom", zcdi::kNtsc},
    {"cdi220b.rom", zcdi::kPal},
}};

constexpr const char* kDvcRomFilename = "vmpega.rom";

const RegionProfile* g_region_profile = &kRegionProfiles[zcdi::kNtsc];

// Global: the core keeps one file per BIOS and hides SAVE_RAM.
zcdi::NvramStorageMode g_nvram_storage = zcdi::NvramStorageMode::kPerDisc;
std::filesystem::path g_global_nvram_path;

zcdi::StateSizeReporter g_state_size;

std::span<const std::byte> memory_region(unsigned id) {
    if (!g_machine) return {};
    const zcdi::MachineInspector view(*g_machine);
    switch (id) {
        case RETRO_MEMORY_SYSTEM_RAM: return view.plane0_data();
        case RETRO_MEMORY_SAVE_RAM:
            if (g_nvram_storage == zcdi::NvramStorageMode::kGlobal) return {};
            return view.nvram_data();
        default: return {};
    }
}

} // namespace

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;

    zcdi::set_log_interface(nullptr);
    vfs_cb = nullptr;
    if (!cb) return;

    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) zcdi::set_log_interface(logging.log);

    struct retro_vfs_interface_info vfs_info;
    vfs_info.required_interface_version = 1;
    vfs_info.iface = nullptr;
    if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info)) vfs_cb = vfs_info.iface;

    // No content: the shell menu with an empty tray, as a real player.
    bool no_rom = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
    zcdi::register_core_options(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name = "zcdi";
    info->library_version = zcdi::version.sem_ver.c_str();
    info->valid_extensions = "chd|cue|bin|iso|m3u";
    info->need_fullpath = true; // disc backends open by path
    info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width = kWidth;
    info->geometry.base_height = kHeight;
    info->geometry.max_width = kWidth;
    info->geometry.max_height = kMaxHeight;
    info->geometry.aspect_ratio = kAspect;
    info->timing.fps = g_region_profile->refresh_rate();
    info->timing.sample_rate = kSampleRate;
}

// What doesn't depend on the content; the machine is built per load, from the options.
RETRO_API void retro_init(void) {
    if (env_cb) {
        zcdi::register_disk_control(env_cb, vfs_cb);
        zcdi::query_input_bitmasks(env_cb);
    }
    g_state_size = zcdi::StateSizeReporter(zcdi::negotiate_variable_state_size(env_cb));
    g_audio = std::make_unique<zcdi::Mixer>(kSampleRate);
}

RETRO_API void retro_deinit(void) {
    zcdi::disk_control_reset(nullptr);
    g_machine.reset();
    g_audio.reset();
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { zcdi::set_port_device(port, device); }

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb && !env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
    if (env_cb) {
        zcdi::set_controller_info(env_cb);
        zcdi::set_input_descriptors(env_cb);
    }
    g_state_size = zcdi::StateSizeReporter(g_state_size.variable()); // a new fixed size per content

    const zcdi::LibretroRegion preferred_bios_region = zcdi::get_preferred_region(env_cb);
    g_region_profile = &kRegionProfiles[preferred_bios_region];
    const char* system_dir = zcdi::system_directory(env_cb);

    std::vector<std::byte> bios_storage;
    std::span<const std::byte> raw_bios_content =
        zcdi::load_bios(system_dir, g_region_profile->bios_filename, bios_storage, env_cb, vfs_cb);
    if (raw_bios_content.empty()) return false;

    std::vector<std::byte> dvc_storage;
    std::span<const std::byte> raw_dvc_content;
    if (zcdi::get_dvc_mode(env_cb) == zcdi::DvcMode::kAuto) {
        raw_dvc_content = zcdi::load_dvc_rom(system_dir, kDvcRomFilename, dvc_storage, vfs_cb);
    }
    zcdi::log_msg(RETRO_LOG_INFO, "DVC rom%s found\n", (raw_dvc_content.empty() ? " not" : ""));

    g_nvram_storage = zcdi::get_nvram_storage_mode(env_cb);
    std::vector<std::byte> nvram_storage;
    std::span<const std::byte> raw_nvram_content;
    if (g_nvram_storage == zcdi::NvramStorageMode::kGlobal) {
        g_global_nvram_path = zcdi::global_nvram_path(env_cb, system_dir, g_region_profile->bios_filename);
        raw_nvram_content = zcdi::get_file_content(g_global_nvram_path, nvram_storage, vfs_cb);
        zcdi::log_msg(RETRO_LOG_INFO, "Global NVRAM %s%s\n", g_global_nvram_path.generic_string().c_str(),
                      raw_nvram_content.empty() ? " not found, starting blank" : "");
    }

    g_machine = std::make_unique<zcdi::Machine>(
        raw_bios_content, g_region_profile->video_standard(),
        zcdi::Machine::InitParams{
            .dvc = raw_dvc_content, .nvram = raw_nvram_content, .auto_play = zcdi::get_auto_play_enabled(env_cb)});
    zcdi::log_msg(RETRO_LOG_INFO, "Refresh rate %.1fHz\n", g_machine->refresh_rate());
    g_out_width = kWidth;
    g_out_height = kHeight;
    zcdi::set_memory_maps(env_cb, *g_machine);

    g_machine->cold_boot();
    zcdi::reset_frontend_state(*g_audio, kSampleRate);
    zcdi::disk_control_reset(g_machine.get());
    if (game && game->path && !zcdi::disk_control_insert_content(game->path)) return false;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

RETRO_API void retro_reset(void) {
    if (!g_machine) return;
    g_machine->reset();
    zcdi::reset_frontend_state(*g_audio, kSampleRate);
}

RETRO_API void retro_unload_game(void) {
    if (g_machine && g_nvram_storage == zcdi::NvramStorageMode::kGlobal) {
        const zcdi::MachineInspector view(*g_machine);
        if (!zcdi::put_file_content(g_global_nvram_path, view.nvram_data(), vfs_cb)) {
            zcdi::log_msg(RETRO_LOG_ERROR, "Couldn't write global NVRAM %s\n",
                          g_global_nvram_path.generic_string().c_str());
        }
    }
    zcdi::disk_control_reset(nullptr);
    g_machine.reset();
}

RETRO_API void retro_run(void) {
    if (input_poll_cb) input_poll_cb();

    if (g_machine) {
        zcdi::poll_pointer_input(input_state_cb, *g_machine);
        g_machine->run_frame();
        std::string uart = g_machine->bus().drain_uart_output();
        if (!uart.empty()) {
            zcdi::log_msg(RETRO_LOG_INFO, "%s", uart.c_str());
        }
        const int width = g_machine->bus().video_active_width();
        const int height = g_machine->bus().video_active_height();
        if (width != g_out_width || height != g_out_height) {
            g_out_width = width;
            g_out_height = height;
            struct retro_game_geometry geometry = {
                .base_width = unsigned(width),
                .base_height = unsigned(height),
                .max_width = kWidth,
                .max_height = kMaxHeight,
                .aspect_ratio = kAspect,
            };
            if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
        }
        const zcdi::AudioStreams audio = g_machine->drain_audio();
        g_audio->push_samples(audio.cdic);
        g_audio->push_dvc_samples(audio.dvc);
    }

    // No copy: the framebuffer is unchanged until the next run_frame().
    if (video_cb && g_machine) {
        video_cb(g_machine->bus().video_framebuffer(), unsigned(g_out_width), unsigned(g_out_height),
                 static_cast<size_t>(kWidth) * sizeof(uint32_t));
    }

    const int frames = static_cast<int>(kSampleRate / g_region_profile->refresh_rate());
    const int16_t* render_buf = g_audio->render(frames);
    if (audio_batch_cb) audio_batch_cb(render_buf, frames);
}

RETRO_API size_t retro_serialize_size(void) {
    if (!g_machine || !g_audio) return 0;
    return g_state_size.report(zcdi::core_state_size(*g_machine, *g_audio, zcdi::input_history()));
}

RETRO_API bool retro_serialize(void* data, size_t len) {
    if (!g_machine || !g_audio || !data) return false;
    uint8_t* out = static_cast<uint8_t*>(data);
    size_t written = zcdi::save_core_state(*g_machine, *g_audio, zcdi::input_history(), {out, len});
    if (written == 0) {
        zcdi::log_msg(RETRO_LOG_ERROR, "Save state needs %zu bytes, frontend buffer has %zu%s\n",
                      zcdi::core_state_size(*g_machine, *g_audio, zcdi::input_history()), len,
                      g_state_size.variable() ? "" : " (the frontend doesn't support variable-size states)");
        return false;
    }
    std::memset(out + written, 0, len - written);
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t len) {
    if (!g_machine || !g_audio || !data) return false;
    if (!zcdi::load_core_state(*g_machine, *g_audio, zcdi::input_history(), {static_cast<const uint8_t*>(data), len})) {
        zcdi::log_msg(RETRO_LOG_ERROR,
                      "Save state rejected: damaged, or not from this core version, BIOS, DVC ROM, options or disc\n");
        return false;
    }
    return true;
}

// No CD-i cheat code format to parse; frontend memory cheats go through the memory maps.
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

RETRO_API void* retro_get_memory_data(unsigned id) { return const_cast<std::byte*>(memory_region(id).data()); }

RETRO_API size_t retro_get_memory_size(unsigned id) { return memory_region(id).size(); }

RETRO_API unsigned retro_get_region(void) { return g_region_profile->region; }

} // extern "C"
