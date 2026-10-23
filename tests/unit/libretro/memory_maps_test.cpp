#include "doctest.h"

#include <memory>
#include <vector>

#include "libretro/helpers.h"
#include "libretro/libretro.h"
#include "support/fake_machine.h"

namespace {

// What the frontend copied from the last SET_MEMORY_MAPS.
std::vector<retro_memory_descriptor> g_frontend_map;

bool capture_env(unsigned cmd, void* data) {
    if (cmd != RETRO_ENVIRONMENT_SET_MEMORY_MAPS) return false;
    const auto* map = static_cast<const retro_memory_map*>(data);
    g_frontend_map.assign(map->descriptors, map->descriptors + map->num_descriptors);
    return true;
}

}  // namespace

TEST_SUITE("libretro") {

TEST_CASE("the memory map handed to the frontend is emptied before the machine goes") {
    std::unique_ptr<zcdi::Machine> machine = zcdi_test::build_fake_machine();
    zcdi::set_memory_maps(capture_env, *machine);
    REQUIRE_FALSE(g_frontend_map.empty());
    zcdi::clear_memory_maps(capture_env);
    CHECK(g_frontend_map.empty());
}

}  // TEST_SUITE
