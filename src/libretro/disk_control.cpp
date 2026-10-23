#include "disk_control.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/machine.h"
#include "disc/file_io.h"
#include "disc/m3u_parser.h"
#include "helpers.h"

namespace zcdi {

namespace {

retro_environment_t env_cb = nullptr;
const struct retro_vfs_interface* vfs_cb = nullptr;
Machine* g_machine = nullptr;

// An empty path is a slot the frontend added and hasn't filled yet.
struct DiscImage {
    std::string path;
    std::string label; // from the m3u; empty = the file name
};
// g_disc_index == size() means the tray is empty.
std::vector<DiscImage> g_disc_paths;
size_t g_disc_index = 0;
size_t g_initial_index = 0;
std::string g_initial_path;

// Puts disc `index` in the tray, or empties it for an index with no image.
bool insert_disc_image(size_t index) {
    if (index >= g_disc_paths.size() || g_disc_paths[index].path.empty()) {
        g_machine->remove_disc();
        g_disc_index = index < g_disc_paths.size() ? index : g_disc_paths.size();
        return true;
    }
    auto disc_ctx = std::make_unique<DiscContext>(g_disc_paths[index].path);
    wire_vfs(*disc_ctx, vfs_cb);
    std::unique_ptr<Disc> disc = open_disc(std::move(disc_ctx));
    if (!g_machine->mount_disc(std::move(disc))) {
        std::string text = "Couldn't open disc image " + g_disc_paths[index].path;
        show_error(env_cb, text.c_str());
        return false;
    }
    g_disc_index = index;
    return true;
}

bool RETRO_CALLCONV disk_set_eject_state(bool ejected) {
    if (!g_machine) return false;
    g_machine->set_tray_open(ejected);
    return true;
}

bool RETRO_CALLCONV disk_get_eject_state(void) { return g_machine && g_machine->tray_open(); }

unsigned RETRO_CALLCONV disk_get_image_index(void) { return unsigned(g_disc_index); }

bool RETRO_CALLCONV disk_set_image_index(unsigned index) {
    if (!g_machine || !g_machine->tray_open()) return false;
    return insert_disc_image(index);
}

unsigned RETRO_CALLCONV disk_get_num_images(void) { return unsigned(g_disc_paths.size()); }

bool RETRO_CALLCONV disk_replace_image_index(unsigned index, const struct retro_game_info* info) {
    if (!g_machine || !g_machine->tray_open() || index >= g_disc_paths.size()) return false;
    if (!info || !info->path) {
        g_disc_paths.erase(g_disc_paths.begin() + index);
        if (index == g_disc_index) {
            g_machine->remove_disc();
            g_disc_index = g_disc_paths.size();
        } else if (index < g_disc_index) {
            --g_disc_index;
        }
        return true;
    }
    g_disc_paths[index] = {info->path, {}};
    return index != g_disc_index || insert_disc_image(index);
}

bool RETRO_CALLCONV disk_add_image_index(void) {
    // The empty-tray index moves up with the list.
    if (g_disc_index == g_disc_paths.size()) ++g_disc_index;
    g_disc_paths.emplace_back();
    return true;
}

// Copies what `get` makes of image `index`'s path into the frontend's buffer.
template <class F>
bool copy_out(unsigned index, char* out, size_t len, F&& get) {
    if (index >= g_disc_paths.size() || g_disc_paths[index].path.empty() || !out || len == 0) return false;
    std::snprintf(out, len, "%s", get(g_disc_paths[index]).c_str());
    return true;
}

bool RETRO_CALLCONV disk_get_image_path(unsigned index, char* path, size_t len) {
    return copy_out(index, path, len, [](const DiscImage& d) { return d.path; });
}

bool RETRO_CALLCONV disk_get_image_label(unsigned index, char* label, size_t len) {
    return copy_out(index, label, len, [](const DiscImage& d) {
        return d.label.empty() ? disc_generic_path(d.path).stem().string() : d.label;
    });
}

bool RETRO_CALLCONV disk_set_initial_image(unsigned index, const char* path) {
    if (!path || !*path) return false;
    g_initial_index = index;
    g_initial_path = path;
    return true;
}

bool load_playlist(const std::string& m3u_path) {
    DiscContext ctx(m3u_path);
    wire_vfs(ctx, vfs_cb);
    std::string text;
    std::vector<M3uEntry> entries;
    if (!disc_read_whole_file(m3u_path, text, ctx) || !parse_m3u(text, entries)) {
        std::string msg = "Couldn't read disc playlist " + m3u_path;
        show_error(env_cb, msg.c_str());
        return false;
    }
    for (const M3uEntry& e : entries)
        g_disc_paths.push_back({disc_path_beside(m3u_path, e.path), e.label});
    return true;
}

} // namespace

void register_disk_control(retro_environment_t env, const struct retro_vfs_interface* vfs) {
    env_cb = env;
    vfs_cb = vfs;
    unsigned version = 0;
    if (env(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version) && version >= 1) {
        struct retro_disk_control_ext_callback ext = {
            disk_set_eject_state, disk_get_eject_state,     disk_get_image_index, disk_set_image_index,
            disk_get_num_images,  disk_replace_image_index, disk_add_image_index, disk_set_initial_image,
            disk_get_image_path,  disk_get_image_label,
        };
        env(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &ext);
    } else {
        struct retro_disk_control_callback basic = {
            disk_set_eject_state, disk_get_eject_state,     disk_get_image_index, disk_set_image_index,
            disk_get_num_images,  disk_replace_image_index, disk_add_image_index,
        };
        env(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &basic);
    }
}

void disk_control_reset(Machine* machine) {
    g_machine = machine;
    g_disc_paths.clear();
    g_disc_index = 0;
}

bool disk_control_insert_content(const char* path) {
    const std::string ext = disc_path_ext(path);
    if (ext == "m3u") {
        if (!load_playlist(path)) return false;
    } else {
        g_disc_paths.push_back({path, {}});
    }
    // Only if the playlist still has that disc where it was.
    size_t first = 0;
    if (g_initial_index < g_disc_paths.size() && g_disc_paths[g_initial_index].path == g_initial_path) {
        first = g_initial_index;
    }
    g_initial_index = 0;
    g_initial_path.clear();
    return insert_disc_image(first);
}

} // namespace zcdi
