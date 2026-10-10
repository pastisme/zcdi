#include "helpers.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "audio/mixer.h"
#include "core/machine.h"
#include "core/machine_inspector.h"
#include "input_mapping.h"

namespace zcdi {

namespace {
namespace fs = std::filesystem;

retro_log_printf_t log_cb = nullptr;

fs::path as_generic_path(std::string_view s) {
    std::string normalized{s};
    std::ranges::replace(normalized, '\\', '/');
    return fs::path(normalized);
}

// `name` inside `dir`, or `name` as-is without one.
fs::path in_dir(const char* dir, const char* name) {
    if (!dir || !*dir) return name;
    return as_generic_path(dir) / as_generic_path(name);
}
} // namespace

void set_log_interface(retro_log_printf_t cb) { log_cb = cb; }

void log_msg(enum retro_log_level level, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (log_cb) {
        log_cb(level, "%s", buf);
    } else {
        std::fprintf(stderr, "[zcdi] %s", buf);
    }
}

void show_error(retro_environment_t env, const char* text) {
    log_msg(RETRO_LOG_ERROR, "%s\n", text);
    if (!env) return;
    unsigned version = 0;
    if (env(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) && version >= 1) {
        struct retro_message_ext msg = {
            .msg = text,
            .duration = 5000,
            .priority = 3,
            .level = RETRO_LOG_ERROR,
            .target = RETRO_MESSAGE_TARGET_ALL,
            .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
            .progress = -1,
        };
        env(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
    } else {
        struct retro_message msg = {.msg = text, .frames = 300};
        env(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
}

const char* system_directory(retro_environment_t env) {
    const char* dir = nullptr;
    if (env && env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir && *dir) return dir;
    return nullptr;
}

std::span<const std::byte> load_bios(const char* system_dir, const char* filename, std::vector<std::byte>& storage,
                                     retro_environment_t env, const struct retro_vfs_interface* vfs_cb) {
    if (!system_dir) {
        log_msg(RETRO_LOG_WARN, "No system directory from frontend; trying BIOS path as-is: %s\n", filename);
    }
    std::span<const std::byte> content = get_file_content(in_dir(system_dir, filename), storage, vfs_cb);
    if (content.empty()) {
        std::string text = std::string("CD-i BIOS ") + filename + " not found in the system directory";
        show_error(env, text.c_str());
        return {};
    }
    log_msg(RETRO_LOG_INFO, "Found BIOS %s (%zu bytes)\n", filename, content.size());
    return content;
}

std::span<const std::byte> load_dvc_rom(const char* system_dir, const char* filename, std::vector<std::byte>& storage,
                                        const struct retro_vfs_interface* vfs_cb) {
    return get_file_content(in_dir(system_dir, filename), storage, vfs_cb);
}

std::filesystem::path global_nvram_path(retro_environment_t env, const char* system_dir, const char* bios_filename) {
    const char* save_dir = nullptr;
    if (!env || !env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir || !*save_dir) {
        save_dir = system_dir;
    }
    const std::string name = "zcdi_" + fs::path(bios_filename).stem().string() + ".nvram";
    return in_dir(save_dir, name.c_str());
}

bool negotiate_variable_state_size(retro_environment_t env) {
    // The frontend clears what it doesn't support.
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
    return env && env(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks) &&
           (quirks & RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE);
}

void set_memory_maps(retro_environment_t env, const Machine& machine) {
    // 0x500000-0xcfffff isn't listed: a real player bus-errors there.
    const MachineInspector view(machine);
    static retro_memory_descriptor descs[3];
    static retro_memory_map map;
    unsigned count = 0;
    auto add = [&](std::span<const std::byte> ram, size_t start, const char* name) {
        if (ram.empty()) return;
        retro_memory_descriptor& d = descs[count++];
        d = {};
        d.flags = RETRO_MEMDESC_SYSTEM_RAM | RETRO_MEMDESC_BIGENDIAN;
        d.ptr = const_cast<std::byte*>(ram.data());
        d.start = start;
        d.len = ram.size();
        d.addrspace = name;
    };
    add(view.plane0_data(), 0x000000, "RAM0");
    add(view.plane1_data(), 0x200000, "RAM1");
    add(view.dvc_cart_ram_data(), 0xd00000, "DVCRAM");
    map = {descs, count};
    if (env) env(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}

void reset_frontend_state(Mixer& audio, int sample_rate) {
    audio = Mixer(sample_rate);
    input_history() = InputHistory{};
}

std::span<const std::byte> get_file_content(const std::filesystem::path& path, std::vector<std::byte>& storage,
                                            const struct retro_vfs_interface* vfs_cb) {
    const std::string path_str = path.generic_string();
    storage.clear();

    if (vfs_cb && vfs_cb->open) {
        struct retro_vfs_file_handle* f =
            vfs_cb->open(path_str.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
        if (f) {
            int64_t size = vfs_cb->size(f);
            if (size > 0) {
                storage.resize(static_cast<size_t>(size));
                if (vfs_cb->read(f, storage.data(), static_cast<uint64_t>(size)) != size) storage.clear();
            }
            vfs_cb->close(f);
            if (!storage.empty()) {
                return {storage.data(), storage.size()};
            }
        }
    }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize size = f.tellg();
    if (size <= 0) return {};

    storage.resize(static_cast<size_t>(size));
    f.seekg(0);
    if (!static_cast<bool>(f.read(reinterpret_cast<char*>(storage.data()), size))) {
        storage.clear();
        return {};
    }
    return {storage.data(), storage.size()};
}

bool put_file_content(const std::filesystem::path& path, std::span<const std::byte> content,
                      const struct retro_vfs_interface* vfs_cb) {
    const std::string path_str = path.generic_string();

    if (vfs_cb && vfs_cb->open && vfs_cb->write && vfs_cb->close) {
        struct retro_vfs_file_handle* f =
            vfs_cb->open(path_str.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
        if (f) {
            int64_t written = vfs_cb->write(f, content.data(), content.size());
            bool closed = vfs_cb->close(f) == 0;
            return closed && written == static_cast<int64_t>(content.size());
        }
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(f);
}

void wire_vfs(DiscContext& disc_ctx, const struct retro_vfs_interface* vfs_cb) {
    if (!vfs_cb || !vfs_cb->open || !vfs_cb->close || !vfs_cb->size || !vfs_cb->read || !vfs_cb->seek) {
        return;
    }

    disc_ctx.open_read = [vfs_cb](const std::string& file_path) -> DiscContext::Handle {
        return reinterpret_cast<DiscContext::Handle>(
            vfs_cb->open(file_path.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE));
    };
    disc_ctx.read = [vfs_cb](DiscContext::Handle handle, void* ptr, size_t len) -> size_t {
        int64_t got = vfs_cb->read(reinterpret_cast<retro_vfs_file_handle*>(handle), ptr, len);
        if (got <= 0) return 0;
        return static_cast<size_t>(got);
    };
    disc_ctx.seek = [vfs_cb](DiscContext::Handle handle, int64_t offset, int whence) -> int {
        int origin = -1;
        switch (whence) {
            case SEEK_SET: origin = RETRO_VFS_SEEK_POSITION_START; break;
            case SEEK_CUR: origin = RETRO_VFS_SEEK_POSITION_CURRENT; break;
            case SEEK_END: origin = RETRO_VFS_SEEK_POSITION_END; break;
            default: return -1;
        }
        int64_t pos = vfs_cb->seek(reinterpret_cast<retro_vfs_file_handle*>(handle), offset, origin);
        return (pos < 0) ? -1 : 0;
    };
    disc_ctx.size = [vfs_cb](DiscContext::Handle handle) -> uint64_t {
        int64_t size = vfs_cb->size(reinterpret_cast<retro_vfs_file_handle*>(handle));
        return (size < 0) ? static_cast<uint64_t>(-1) : static_cast<uint64_t>(size);
    };
    disc_ctx.close = [vfs_cb](DiscContext::Handle handle) -> int {
        return vfs_cb->close(reinterpret_cast<retro_vfs_file_handle*>(handle));
    };
}

} // namespace zcdi
