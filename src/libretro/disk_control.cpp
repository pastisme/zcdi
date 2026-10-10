#include "disk_control.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/machine.h"
#include "helpers.h"

namespace zcdi {

namespace {

retro_environment_t env_cb = nullptr;
const struct retro_vfs_interface* vfs_cb = nullptr;
Machine* g_machine = nullptr;

// An empty path is a slot the frontend added and hasn't filled yet.
// g_disc_index == size() means the tray is empty.
std::vector<std::string> g_disc_paths;
size_t g_disc_index = 0;

// Puts disc `index` in the tray, or empties it for an index with no image.
bool insert_disc_image(size_t index) {
    if (index >= g_disc_paths.size() || g_disc_paths[index].empty()) {
        g_machine->remove_disc();
        g_disc_index = index < g_disc_paths.size() ? index : g_disc_paths.size();
        return true;
    }
    auto disc_ctx = std::make_unique<DiscContext>(g_disc_paths[index]);
    wire_vfs(*disc_ctx, vfs_cb);
    std::unique_ptr<Disc> disc = open_disc(std::move(disc_ctx));
    if (!g_machine->mount_disc(std::move(disc))) {
        std::string text = "Couldn't open disc image " + g_disc_paths[index];
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
    g_disc_paths[index] = info->path;
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
    if (index >= g_disc_paths.size() || g_disc_paths[index].empty() || !out || len == 0) return false;
    std::snprintf(out, len, "%s", get(g_disc_paths[index]).c_str());
    return true;
}

bool RETRO_CALLCONV disk_get_image_path(unsigned index, char* path, size_t len) {
    return copy_out(index, path, len, [](const std::string& p) { return p; });
}

bool RETRO_CALLCONV disk_get_image_label(unsigned index, char* label, size_t len) {
    return copy_out(index, label, len, [](const std::string& p) { return std::filesystem::path(p).stem().string(); });
}

} // namespace

void register_disk_control(retro_environment_t env, const struct retro_vfs_interface* vfs) {
    env_cb = env;
    vfs_cb = vfs;
    unsigned version = 0;
    if (env(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version) && version >= 1) {
        struct retro_disk_control_ext_callback ext = {
            disk_set_eject_state, disk_get_eject_state,     disk_get_image_index, disk_set_image_index,
            disk_get_num_images,  disk_replace_image_index, disk_add_image_index, nullptr,
            disk_get_image_path,  disk_get_image_label, // no set_initial_image
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
    g_disc_paths.push_back(path);
    return insert_disc_image(0);
}

} // namespace zcdi
