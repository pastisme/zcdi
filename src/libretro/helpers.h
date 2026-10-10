#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "libretro/libretro.h"

#include "disc/disc.h"

namespace zcdi {

class Bus;
class Machine;
class Mixer;

// stderr until a log interface is set.
void set_log_interface(retro_log_printf_t cb);
void log_msg(enum retro_log_level level, const char* fmt, ...);

// On screen where the frontend supports it, and always in the log.
void show_error(retro_environment_t env, const char* text);

// Null if the frontend has none.
const char* system_directory(retro_environment_t env);

// Without `system_dir`, `filename` is used as-is. Empty span (and a message
// to the player) if not found.
std::span<const std::byte> load_bios(const char* system_dir, const char* filename, std::vector<std::byte>& storage,
                                     retro_environment_t env, const struct retro_vfs_interface* vfs_cb);

// Empty span if there is none: the machine then runs without a DVC.
std::span<const std::byte> load_dvc_rom(const char* system_dir, const char* filename, std::vector<std::byte>& storage,
                                        const struct retro_vfs_interface* vfs_cb);

// The save directory, else `system_dir`, else the working directory.
std::filesystem::path global_nvram_path(retro_environment_t env, const char* system_dir, const char* bios_filename);

// True if the frontend re-queries the size before each save.
bool negotiate_variable_state_size(retro_environment_t env);

// Call again whenever the machine is rebuilt: the descriptors point into it.
void set_memory_maps(retro_environment_t env, const Machine& machine);

// Call on cold boot or reset, so old queued audio doesn't play into it.
void reset_frontend_state(Mixer& audio, int sample_rate);

// VFS if given, else stdio. Returns a span over `storage`, empty on failure.
std::span<const std::byte> get_file_content(const std::filesystem::path& path, std::vector<std::byte>& storage,
                                            const struct retro_vfs_interface* vfs_cb);

// Replaces the file. VFS if given, else stdio.
bool put_file_content(const std::filesystem::path& path, std::span<const std::byte> content,
                      const struct retro_vfs_interface* vfs_cb);

void wire_vfs(DiscContext& disc_ctx, const struct retro_vfs_interface* vfs_cb);

} // namespace zcdi
