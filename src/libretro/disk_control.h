#pragma once
#include "libretro/libretro.h"

namespace zcdi {

class Machine;

// Call from retro_init(); `vfs` (may be null) opens the images.
void register_disk_control(retro_environment_t env, const struct retro_vfs_interface* vfs);

// Call whenever the machine is rebuilt or destroyed.
void disk_control_reset(Machine* machine);

// False, after telling the player, if the image can't be opened.
bool disk_control_insert_content(const char* path);

} // namespace zcdi
