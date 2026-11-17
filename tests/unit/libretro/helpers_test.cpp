#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "libretro/helpers.h"
#include "libretro/input_mapping.h"
#include "support/fake_frontend.h"
#include "support/state_roundtrip.h"

using zcdi_test::FakeFrontend;

namespace {

// Absent everywhere: a VFS miss falls back to stdio, which must find nothing either.
constexpr const char* kAbsent = "/zcdi-test-absent/none.bin";

std::vector<std::byte> as_bytes(const std::vector<uint8_t>& v) {
    std::vector<std::byte> out(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) out[i] = std::byte{v[i]};
    return out;
}

bool same(std::span<const std::byte> a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != std::byte{b[i]}) return false;
    }
    return true;
}

// Captures log_msg() for the scope.
struct CapturedLog {
    CapturedLog() { zcdi::set_log_interface(FakeFrontend::log_printf); }
    ~CapturedLog() { zcdi::set_log_interface(nullptr); }
};

}  // namespace

TEST_SUITE("libretro") {

TEST_CASE("a file written through the VFS reads back, and a rewrite replaces it") {
    FakeFrontend frontend;
    const std::vector<uint8_t> first = {1, 2, 3, 4, 5}, second = {9, 8};
    REQUIRE(zcdi::put_file_content("/save/a.nvram", as_bytes(first), FakeFrontend::vfs()));
    CHECK(frontend.files["/save/a.nvram"] == first);
    REQUIRE(zcdi::put_file_content("/save/a.nvram", as_bytes(second), FakeFrontend::vfs()));
    CHECK(frontend.files["/save/a.nvram"] == second);

    std::vector<std::byte> storage;
    CHECK(same(zcdi::get_file_content("/save/a.nvram", storage, FakeFrontend::vfs()), second));
}

TEST_CASE("a missing or empty file reads as nothing") {
    FakeFrontend frontend;
    frontend.files["/save/empty"] = {};
    std::vector<std::byte> storage(3);
    CHECK(zcdi::get_file_content(kAbsent, storage, FakeFrontend::vfs()).empty());
    CHECK(zcdi::get_file_content("/save/empty", storage, FakeFrontend::vfs()).empty());
    CHECK(storage.empty());
}

TEST_CASE("the BIOS is read from the system directory, or from its bare name without one") {
    CapturedLog log;
    FakeFrontend frontend;
    const std::vector<uint8_t> rom = {0x00, 0x07, 0x00, 0x00};
    frontend.files["/sys/cdi220b.rom"] = rom;
    frontend.files["cdi200.rom"] = rom;
    std::vector<std::byte> storage;
    CHECK(same(zcdi::load_bios("/sys", "cdi220b.rom", storage, FakeFrontend::env, FakeFrontend::vfs()), rom));
    CHECK(same(zcdi::load_bios(nullptr, "cdi200.rom", storage, FakeFrontend::env, FakeFrontend::vfs()), rom));
    CHECK(frontend.messages.empty());
}

TEST_CASE("a missing BIOS is reported to the player") {
    CapturedLog log;
    FakeFrontend frontend;
    std::vector<std::byte> storage;
    CHECK(zcdi::load_bios("/zcdi-test-absent", "cdi220b.rom", storage, FakeFrontend::env, FakeFrontend::vfs()).empty());
    REQUIRE(frontend.messages.size() == 1);
    CHECK(frontend.messages[0].find("cdi220b.rom") != std::string::npos);
}

TEST_CASE("the DVC ROM is optional") {
    FakeFrontend frontend;
    const std::vector<uint8_t> rom = {0x4e, 0x75};
    std::vector<std::byte> storage;
    CHECK(zcdi::load_dvc_rom("/zcdi-test-absent", "vmpega.rom", storage, FakeFrontend::vfs()).empty());
    frontend.files["/sys/vmpega.rom"] = rom;
    CHECK(same(zcdi::load_dvc_rom("/sys", "vmpega.rom", storage, FakeFrontend::vfs()), rom));
    CHECK(frontend.messages.empty());
}

TEST_CASE("global NVRAM goes to the save directory, else the system directory, else the working directory") {
    FakeFrontend frontend;
    frontend.save_dir = "/save";
    CHECK(zcdi::global_nvram_path(FakeFrontend::env, "/sys", "cdi220b.rom").generic_string() ==
          "/save/zcdi_cdi220b.nvram");
    frontend.save_dir = nullptr;
    CHECK(zcdi::global_nvram_path(FakeFrontend::env, "/sys", "cdi220b.rom").generic_string() ==
          "/sys/zcdi_cdi220b.nvram");
    CHECK(zcdi::global_nvram_path(FakeFrontend::env, nullptr, "cdi200.rom").generic_string() == "zcdi_cdi200.nvram");
    CHECK(zcdi::global_nvram_path(nullptr, nullptr, "cdi200.rom").generic_string() == "zcdi_cdi200.nvram");
}

TEST_CASE("an empty system directory counts as none") {
    FakeFrontend frontend;
    CHECK(zcdi::system_directory(nullptr) == nullptr);
    CHECK(zcdi::system_directory(FakeFrontend::env) == nullptr);
    frontend.system_dir = "";
    CHECK(zcdi::system_directory(FakeFrontend::env) == nullptr);
    frontend.system_dir = "/sys";
    CHECK(std::string(zcdi::system_directory(FakeFrontend::env)) == "/sys");
}

TEST_CASE("variable-size states only when the frontend keeps its quirk bit") {
    FakeFrontend frontend;
    CHECK_FALSE(zcdi::negotiate_variable_state_size(nullptr));
    CHECK_FALSE(zcdi::negotiate_variable_state_size(FakeFrontend::env));
    frontend.variable_state_size = true;
    CHECK(zcdi::negotiate_variable_state_size(FakeFrontend::env));
}

TEST_CASE("only runahead states come from this session") {
    FakeFrontend frontend;
    CHECK_FALSE(zcdi::state_from_this_session(nullptr));
    CHECK_FALSE(zcdi::state_from_this_session(FakeFrontend::env));  // no savestate context
    for (int context : {int(RETRO_SAVESTATE_CONTEXT_NORMAL), int(RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY)}) {
        frontend.savestate_context = context;
        CHECK_FALSE(zcdi::state_from_this_session(FakeFrontend::env));
    }
    for (int context : {int(RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE),
                        int(RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_BINARY)}) {
        frontend.savestate_context = context;
        CHECK(zcdi::state_from_this_session(FakeFrontend::env));
    }
}

TEST_CASE("errors go on screen through either message interface, and to the log") {
    CapturedLog log;
    FakeFrontend frontend;
    zcdi::show_error(FakeFrontend::env, "first");
    frontend.message_version = 1;
    zcdi::show_error(FakeFrontend::env, "second");
    zcdi::show_error(nullptr, "third");
    CHECK(frontend.messages == std::vector<std::string>{"first", "second"});
    CHECK(frontend.log == std::vector<std::string>{"first\n", "second\n", "third\n"});
}

TEST_CASE("log messages are formatted") {
    CapturedLog log;
    FakeFrontend frontend;
    zcdi::log_msg(RETRO_LOG_INFO, "%s has %d bytes\n", "BIOS", 524288);
    CHECK(frontend.log == std::vector<std::string>{"BIOS has 524288 bytes\n"});
}

TEST_CASE("a frontend reset empties the audio queue and forgets the lightgun's aim") {
    zcdi::Mixer audio(44100);
    const std::vector<uint8_t> fresh = zcdi_test::save_state(audio);
    audio.push_samples(std::vector<int16_t>(2000, 100));
    zcdi::input_history().gun_x = 300;
    zcdi::input_history().gun_y = 200;
    zcdi::reset_frontend_state(audio, 44100);
    CHECK(zcdi_test::first_difference(zcdi_test::save_state(audio), fresh) == -1);
    CHECK(zcdi::input_history().gun_x == -1);
    CHECK(zcdi::input_history().gun_y == -1);
}

TEST_CASE("disc images are read through the VFS when the frontend has one") {
    FakeFrontend frontend;
    frontend.files["/discs/game.bin"] = {10, 11, 12, 13, 14, 15};
    zcdi::DiscContext ctx("/discs/game.bin");
    zcdi::wire_vfs(ctx, FakeFrontend::vfs());

    CHECK(ctx.open_read(kAbsent) == nullptr);
    zcdi::DiscContext::Handle h = ctx.open_read("/discs/game.bin");
    REQUIRE(h);
    CHECK(ctx.size(h) == 6);
    uint8_t buf[4] = {};
    REQUIRE(ctx.seek(h, -2, SEEK_END) == 0);
    CHECK(ctx.read(h, buf, sizeof(buf)) == 2);
    CHECK(buf[0] == 14);
    CHECK(buf[1] == 15);
    REQUIRE(ctx.seek(h, 1, SEEK_SET) == 0);
    REQUIRE(ctx.seek(h, 1, SEEK_CUR) == 0);
    CHECK(ctx.read(h, buf, 1) == 1);
    CHECK(buf[0] == 12);
    CHECK(ctx.seek(h, 0, 99) == -1);  // no such whence
    CHECK(ctx.seek(h, 100, SEEK_SET) == -1);
    CHECK(ctx.close(h) == 0);
}

TEST_CASE("a VFS missing a function leaves disc reads on stdio") {
    FakeFrontend frontend;
    frontend.files["/discs/game.bin"] = {1};
    retro_vfs_interface partial = *FakeFrontend::vfs();
    partial.seek = nullptr;
    zcdi::DiscContext ctx("/discs/game.bin");
    zcdi::wire_vfs(ctx, &partial);
    CHECK(ctx.open_read("/discs/game.bin") == nullptr);  // stdio can't see the VFS's files
}

}  // TEST_SUITE
