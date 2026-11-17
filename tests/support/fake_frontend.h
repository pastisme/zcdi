#pragma once
// A libretro frontend in memory: answers the core's environment calls, keeps
// what it was told, and serves files from MemFiles through the VFS (so a test
// never touches the real filesystem). One at a time: libretro callbacks carry
// no user pointer, so they reach the live FakeFrontend through a static.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "libretro/libretro.h"
#include "support/mem_files.h"

namespace zcdi_test {

class FakeFrontend {
public:
    // Answers. A null directory is a frontend without one.
    const char* system_dir = nullptr;
    const char* save_dir = nullptr;
    std::map<std::string, std::string> variables;  // core option values
    unsigned core_options_version = 0;              // 0: legacy SET_VARIABLES only
    unsigned message_version = 0;                   // 0: SET_MESSAGE only
    bool input_bitmasks = false;
    bool variable_state_size = false;  // supports variable-size states
    int savestate_context = -1;        // -1: GET_SAVESTATE_CONTEXT unsupported
    bool has_vfs = true;               // GET_VFS_INTERFACE answered
    unsigned disk_control_version = 1; // 0: the basic disk control interface only
    bool accepts_pixel_format = true;
    MemFiles files;                    // the VFS's files; writes land here

    // What the core told it.
    std::vector<std::string> messages;  // SET_MESSAGE and SET_MESSAGE_EXT texts
    std::vector<std::string> log;       // through log_printf
    const retro_core_options_v2_intl* options_v2 = nullptr;
    std::vector<retro_variable> legacy_options;  // up to the terminator
    std::vector<retro_input_descriptor> input_descriptors;
    std::vector<retro_controller_info> controller_info;
    bool supports_no_game = false;
    retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_disk_control_ext_callback disk{};  // the basic interface fills the first seven
    bool disk_ext = false;
    std::vector<retro_game_geometry> geometries;
    std::vector<unsigned> memory_map_sizes;  // descriptors in each SET_MEMORY_MAPS

    FakeFrontend() { current() = this; }
    ~FakeFrontend() { current() = nullptr; }
    FakeFrontend(const FakeFrontend&) = delete;
    FakeFrontend& operator=(const FakeFrontend&) = delete;

    static bool env(unsigned cmd, void* data) { return current() && current()->handle(cmd, data); }

    static void log_printf(enum retro_log_level, const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (current()) current()->log.emplace_back(buf);
    }

    // VFS v1, over `files`.
    static const retro_vfs_interface* vfs() {
        static const retro_vfs_interface iface = [] {
            retro_vfs_interface v{};
            v.open = vfs_open;
            v.close = vfs_close;
            v.size = vfs_size;
            v.tell = vfs_tell;
            v.seek = vfs_seek;
            v.read = vfs_read;
            v.write = vfs_write;
            v.flush = vfs_flush;
            return v;
        }();
        return &iface;
    }

private:
    struct File {
        std::string path;
        std::size_t pos = 0;
    };

    static FakeFrontend*& current() {
        static FakeFrontend* frontend = nullptr;
        return frontend;
    }

    bool handle(unsigned cmd, void* data) {
        switch (cmd) {
            case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
                const char* dir = cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY ? system_dir : save_dir;
                if (!dir) return false;
                *static_cast<const char**>(data) = dir;
                return true;
            }
            case RETRO_ENVIRONMENT_GET_VARIABLE: {
                auto* var = static_cast<retro_variable*>(data);
                auto it = variables.find(var->key);
                var->value = it == variables.end() ? nullptr : it->second.c_str();
                return it != variables.end();
            }
            case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
                *static_cast<unsigned*>(data) = core_options_version;
                return true;
            case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
                if (core_options_version < 2) return false;
                options_v2 = static_cast<const retro_core_options_v2_intl*>(data);
                return true;
            case RETRO_ENVIRONMENT_SET_VARIABLES:
                legacy_options.clear();
                for (auto* v = static_cast<const retro_variable*>(data); v->key; ++v) legacy_options.push_back(*v);
                return true;
            case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
                *static_cast<unsigned*>(data) = message_version;
                return true;
            case RETRO_ENVIRONMENT_SET_MESSAGE:
                messages.emplace_back(static_cast<const retro_message*>(data)->msg);
                return true;
            case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
                if (message_version < 1) return false;
                messages.emplace_back(static_cast<const retro_message_ext*>(data)->msg);
                return true;
            case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return input_bitmasks;
            case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
                input_descriptors.clear();
                for (auto* d = static_cast<const retro_input_descriptor*>(data); d->description; ++d) {
                    input_descriptors.push_back(*d);
                }
                return true;
            case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
                controller_info.clear();
                for (auto* p = static_cast<const retro_controller_info*>(data); p->types; ++p) {
                    controller_info.push_back(*p);
                }
                return true;
            case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: {
                // The frontend acknowledges the core's variable size by setting FRONT_VARIABLE_SIZE.
                auto& quirks = *static_cast<uint64_t*>(data);
                if (variable_state_size && (quirks & RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE)) {
                    quirks |= RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
                }
                return true;
            }
            case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
                if (savestate_context < 0) return false;
                *static_cast<int*>(data) = savestate_context;
                return true;
            case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
                static_cast<retro_log_callback*>(data)->log = log_printf;
                return true;
            case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
                auto* info = static_cast<retro_vfs_interface_info*>(data);
                if (!has_vfs || info->required_interface_version > 1) return false;
                info->iface = const_cast<retro_vfs_interface*>(vfs());
                return true;
            }
            case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
                supports_no_game = *static_cast<const bool*>(data);
                return true;
            case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
                if (!accepts_pixel_format) return false;
                pixel_format = *static_cast<const retro_pixel_format*>(data);
                return true;
            case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
                *static_cast<unsigned*>(data) = disk_control_version;
                return true;
            case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
                if (disk_control_version < 1) return false;
                disk = *static_cast<const retro_disk_control_ext_callback*>(data);
                disk_ext = true;
                return true;
            case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
                const auto& basic = *static_cast<const retro_disk_control_callback*>(data);
                disk = {};
                disk.set_eject_state = basic.set_eject_state;
                disk.get_eject_state = basic.get_eject_state;
                disk.get_image_index = basic.get_image_index;
                disk.set_image_index = basic.set_image_index;
                disk.get_num_images = basic.get_num_images;
                disk.replace_image_index = basic.replace_image_index;
                disk.add_image_index = basic.add_image_index;
                disk_ext = false;
                return true;
            }
            case RETRO_ENVIRONMENT_SET_GEOMETRY:
                geometries.push_back(*static_cast<const retro_game_geometry*>(data));
                return true;
            case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
                memory_map_sizes.push_back(static_cast<const retro_memory_map*>(data)->num_descriptors);
                return true;
            default: return false;
        }
    }

    static File* file(retro_vfs_file_handle* h) { return reinterpret_cast<File*>(h); }
    static std::vector<uint8_t>& bytes(retro_vfs_file_handle* h) { return current()->files[file(h)->path]; }

    static retro_vfs_file_handle* RETRO_CALLCONV vfs_open(const char* path, unsigned mode, unsigned) {
        if (!current()) return nullptr;
        if (mode == RETRO_VFS_FILE_ACCESS_WRITE) {
            current()->files[path].clear();
        } else if (!current()->files.count(path)) {
            return nullptr;
        }
        return reinterpret_cast<retro_vfs_file_handle*>(new File{path});
    }
    static int RETRO_CALLCONV vfs_close(retro_vfs_file_handle* h) {
        delete file(h);
        return 0;
    }
    static int64_t RETRO_CALLCONV vfs_size(retro_vfs_file_handle* h) { return int64_t(bytes(h).size()); }
    static int64_t RETRO_CALLCONV vfs_tell(retro_vfs_file_handle* h) { return int64_t(file(h)->pos); }
    static int64_t RETRO_CALLCONV vfs_seek(retro_vfs_file_handle* h, int64_t offset, int origin) {
        const int64_t size = int64_t(bytes(h).size());
        const int64_t base = origin == RETRO_VFS_SEEK_POSITION_START     ? 0
                             : origin == RETRO_VFS_SEEK_POSITION_CURRENT ? int64_t(file(h)->pos)
                                                                         : size;
        if (base + offset < 0 || base + offset > size) return -1;
        file(h)->pos = std::size_t(base + offset);
        return int64_t(file(h)->pos);
    }
    static int64_t RETRO_CALLCONV vfs_read(retro_vfs_file_handle* h, void* dst, uint64_t len) {
        const std::vector<uint8_t>& data = bytes(h);
        const std::size_t n = std::min<std::size_t>(std::size_t(len), data.size() - file(h)->pos);
        std::memcpy(dst, data.data() + file(h)->pos, n);
        file(h)->pos += n;
        return int64_t(n);
    }
    static int64_t RETRO_CALLCONV vfs_write(retro_vfs_file_handle* h, const void* src, uint64_t len) {
        std::vector<uint8_t>& data = bytes(h);
        const auto* p = static_cast<const uint8_t*>(src);
        data.insert(data.end(), p, p + len);
        file(h)->pos = data.size();
        return int64_t(len);
    }
    static int RETRO_CALLCONV vfs_flush(retro_vfs_file_handle*) { return 0; }
};

// Scripted input_state answers, (port, device, index, id) -> value; one at a time.
class FakeInput {
public:
    FakeInput() { current() = this; }
    ~FakeInput() { current() = nullptr; }
    FakeInput(const FakeInput&) = delete;
    FakeInput& operator=(const FakeInput&) = delete;

    void set(unsigned port, unsigned device, unsigned index, unsigned id, int16_t value) {
        values_[{port, device, index, id}] = value;
    }
    void clear() { values_.clear(); }

    static int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) {
        if (!current()) return 0;
        auto it = current()->values_.find({port, device, index, id});
        return it == current()->values_.end() ? 0 : it->second;
    }

private:
    static FakeInput*& current() {
        static FakeInput* input = nullptr;
        return input;
    }
    std::map<std::tuple<unsigned, unsigned, unsigned, unsigned>, int16_t> values_;
};

}  // namespace zcdi_test
