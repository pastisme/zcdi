#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "libretro/core_options.h"
#include "support/fake_frontend.h"

using zcdi_test::FakeFrontend;

namespace {

// "Desc; first|second" -> "first", the legacy interface's default.
std::string legacy_default(const retro_variable& v) {
    const std::string s = v.value;
    const std::size_t start = s.find("; ") + 2;
    return s.substr(start, s.find('|') - start);
}

}  // namespace

TEST_SUITE("libretro") {

TEST_CASE("core options are offered as v2 when the frontend has it, else as legacy variables") {
    FakeFrontend v2;
    v2.core_options_version = 2;
    zcdi::register_core_options(FakeFrontend::env);
    REQUIRE(v2.options_v2);
    CHECK(v2.legacy_options.empty());

    FakeFrontend v1;
    v1.core_options_version = 1;
    zcdi::register_core_options(FakeFrontend::env);
    CHECK_FALSE(v1.options_v2);
    CHECK(v1.legacy_options.size() == 4);

    zcdi::register_core_options(nullptr);
}

TEST_CASE("both option interfaces list the same keys with the same defaults") {
    FakeFrontend legacy;
    zcdi::register_core_options(FakeFrontend::env);
    const std::vector<retro_variable> variables = legacy.legacy_options;

    FakeFrontend v2;
    v2.core_options_version = 2;
    zcdi::register_core_options(FakeFrontend::env);
    REQUIRE(v2.options_v2);
    const retro_core_option_v2_definition* defs = v2.options_v2->us->definitions;

    std::size_t n = 0;
    for (; defs[n].key; ++n) {
        CAPTURE(defs[n].key);
        REQUIRE(n < variables.size());
        CHECK(std::strcmp(defs[n].key, variables[n].key) == 0);
        CHECK(defs[n].default_value == legacy_default(variables[n]));
    }
    CHECK(n == variables.size());
}

TEST_CASE("unset options read as their defaults") {
    FakeFrontend frontend;
    for (retro_environment_t env : {retro_environment_t(FakeFrontend::env), retro_environment_t(nullptr)}) {
        CHECK(zcdi::get_nvram_storage_mode(env) == zcdi::NvramStorageMode::kPerDisc);
        CHECK(zcdi::get_preferred_region(env) == zcdi::kNtsc);
        CHECK(zcdi::get_dvc_mode(env) == zcdi::DvcMode::kAuto);
        CHECK_FALSE(zcdi::get_auto_play_enabled(env));
    }
}

TEST_CASE("set options read back as chosen, and an unknown value as the default") {
    FakeFrontend frontend;
    frontend.variables = {
        {"zcdi_nvram_mode", "global"},
        {"zcdi_preferred_bios", "pal"},
        {"zcdi_dvc_enabled", "off"},
        {"zcdi_auto_play", "on"},
    };
    CHECK(zcdi::get_nvram_storage_mode(FakeFrontend::env) == zcdi::NvramStorageMode::kGlobal);
    CHECK(zcdi::get_preferred_region(FakeFrontend::env) == zcdi::kPal);
    CHECK(zcdi::get_dvc_mode(FakeFrontend::env) == zcdi::DvcMode::kOff);
    CHECK(zcdi::get_auto_play_enabled(FakeFrontend::env));

    for (auto& [key, value] : frontend.variables) value = "bogus";
    CHECK(zcdi::get_nvram_storage_mode(FakeFrontend::env) == zcdi::NvramStorageMode::kPerDisc);
    CHECK(zcdi::get_preferred_region(FakeFrontend::env) == zcdi::kNtsc);
    CHECK(zcdi::get_dvc_mode(FakeFrontend::env) == zcdi::DvcMode::kAuto);
    CHECK_FALSE(zcdi::get_auto_play_enabled(FakeFrontend::env));
}

}  // TEST_SUITE
