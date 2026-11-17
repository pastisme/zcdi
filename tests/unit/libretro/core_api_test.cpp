// The libretro entry points, driven as a frontend would, with every file served from memory.
#include "doctest.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/machine.h"
#include "fixtures/chd_two_track_fixture.h"
#include "libretro/libretro.h"
#include "support/fake_frontend.h"
#include "support/fake_machine.h"
#include "support/mem_files.h"

using zcdi_test::FakeFrontend;

namespace {

constexpr const char* kSystemDir = "/system";
constexpr const char* kNtscBios = "/system/cdi200.rom";
constexpr const char* kPalBios = "/system/cdi220b.rom";
constexpr uint32_t kCounter = 0x1000;  // bumped by the BIOS loop
constexpr uint32_t kBoots = 0x1004;    // bumped once per pass through the reset vector

// The fake BIOS loop, after counting the boot.
std::vector<uint8_t> bios() {
    const uint8_t code[] = {
        0x00, 0x07, 0x00, 0x00,  // reset SSP $70000
        0x00, 0x40, 0x00, 0x08,  // reset PC $400008
        0x52, 0xb8, 0x10, 0x04,  // addq.l #1,$1004.w
        0x52, 0x80,              // loop: addq.l #1,d0
        0x21, 0xc0, 0x10, 0x00,  //   move.l d0,$1000.w
        0x60, 0xf8,              //   bra.s loop
    };
    const std::vector<std::byte> rom = zcdi_test::bios_with_program(code);
    std::vector<uint8_t> out(rom.size());
    std::memcpy(out.data(), rom.data(), rom.size());
    return out;
}

std::vector<uint8_t> chd() { return zcdi_test::bytes_of(zcdi_test::kChdTwoTrackFixtureBytes); }

// What the core handed to the video and audio callbacks.
struct Output {
    int video_frames = 0;
    unsigned width = 0, height = 0;
    size_t pitch = 0;
    std::vector<size_t> audio_batches;  // frames per batch
};
Output g_out;

void RETRO_CALLCONV video(const void*, unsigned width, unsigned height, size_t pitch) {
    ++g_out.video_frames;
    g_out.width = width;
    g_out.height = height;
    g_out.pitch = pitch;
}
size_t RETRO_CALLCONV audio_batch(const int16_t*, size_t frames) {
    g_out.audio_batches.push_back(frames);
    return frames;
}
void RETRO_CALLCONV input_poll() {}
int16_t RETRO_CALLCONV input_state(unsigned, unsigned, unsigned, unsigned) { return 0; }

// One core session: set up the frontend's answers, then start(); torn down as RetroArch does.
struct Core {
    FakeFrontend fe;

    Core() {
        g_out = {};
        fe.system_dir = kSystemDir;
        fe.files[kNtscBios] = bios();
        fe.files[kPalBios] = bios();
    }
    ~Core() {
        retro_unload_game();
        retro_deinit();
        retro_set_environment(nullptr);  // logs go back to stderr
    }

    void start() {
        retro_set_environment(FakeFrontend::env);
        retro_set_video_refresh(video);
        retro_set_audio_sample_batch(audio_batch);
        retro_set_input_poll(input_poll);
        retro_set_input_state(input_state);
        retro_init();
    }
    bool load(const char* path = nullptr) {
        retro_game_info game{};
        game.path = path;
        return retro_load_game(path ? &game : nullptr);
    }

    static uint32_t ram32(uint32_t address) {
        const auto* ram = static_cast<const uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
        return uint32_t(ram[address]) << 24 | uint32_t(ram[address + 1]) << 16 | uint32_t(ram[address + 2]) << 8 |
               ram[address + 3];
    }
    bool logged(const std::string& text) const {
        for (const std::string& line : fe.log)
            if (line.find(text) != std::string::npos) return true;
        return false;
    }
    bool told(const std::string& text) const {
        for (const std::string& m : fe.messages)
            if (m.find(text) != std::string::npos) return true;
        return false;
    }

    std::string image_path(unsigned index) const {
        char buf[64] = {};
        return fe.disk.get_image_path(index, buf, sizeof(buf)) ? buf : "<none>";
    }
    std::string image_label(unsigned index) const {
        char buf[64] = {};
        return fe.disk.get_image_label(index, buf, sizeof(buf)) ? buf : "<none>";
    }
};

}  // namespace

TEST_SUITE("libretro") {

TEST_CASE("the core describes itself") {
    CHECK(retro_api_version() == RETRO_API_VERSION);
    retro_system_info info{};
    retro_get_system_info(&info);
    CHECK(std::string(info.library_name) == "zcdi");
    CHECK(std::string(info.valid_extensions) == "chd|cue|bin|iso|m3u");
    CHECK(info.need_fullpath);  // the disc readers open by path, through the VFS
    CHECK_FALSE(retro_load_game_special(0, nullptr, 0));
}

TEST_CASE("the environment gets the log, the VFS, no-game support, the options and disk control") {
    Core core;
    core.fe.core_options_version = 2;
    core.start();
    CHECK(core.fe.supports_no_game);
    CHECK(core.fe.options_v2 != nullptr);
    CHECK(core.fe.disk_ext);
    REQUIRE(core.load());
    CHECK(core.logged("Found BIOS cdi200.rom"));  // through the frontend's log, read through its VFS
    CHECK(core.fe.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888);
    CHECK_FALSE(core.fe.controller_info.empty());
    CHECK_FALSE(core.fe.input_descriptors.empty());
}

TEST_CASE("a frontend with only the basic disk control interface gets that one") {
    Core core;
    core.fe.disk_control_version = 0;
    core.start();
    CHECK_FALSE(core.fe.disk_ext);
    CHECK(core.fe.disk.get_num_images != nullptr);
    CHECK(core.fe.disk.get_image_path == nullptr);
}

TEST_CASE("without content, the BIOS boots with an empty tray") {
    Core core;
    core.start();
    REQUIRE(core.load());
    CHECK(core.fe.disk.get_num_images() == 0);
    CHECK(core.fe.disk.get_image_index() == 0);  // == the image count: the empty tray
    CHECK_FALSE(core.fe.disk.get_eject_state());
    retro_run();
    CHECK(core.ram32(kBoots) == 1);
}

TEST_CASE("the preferred BIOS option picks the region, its BIOS and its frame rate") {
    struct Case {
        const char* option;
        unsigned region;
        double fps;
    };
    for (const Case& c : {Case{"ntsc", RETRO_REGION_NTSC, 60.0}, Case{"pal", RETRO_REGION_PAL, 50.0}}) {
        CAPTURE(c.option);
        Core core;
        core.fe.variables["zcdi_preferred_bios"] = c.option;
        core.start();
        REQUIRE(core.load());
        CHECK(retro_get_region() == c.region);
        retro_system_av_info av{};
        retro_get_system_av_info(&av);
        CHECK(av.timing.fps == c.fps);
        CHECK(av.timing.sample_rate == 44100);
        CHECK(av.geometry.base_width == 768);
        CHECK(av.geometry.max_height == 560);  // two woven fields
        CHECK(av.geometry.aspect_ratio == doctest::Approx(4.0 / 3.0));
    }
}

TEST_CASE("without its BIOS the load fails and the player is told") {
    Core core;
    core.fe.files.erase(kNtscBios);
    core.fe.system_dir = "/zcdi-test-absent";  // not on disk either
    core.start();
    CHECK_FALSE(core.load());
    CHECK(core.told("CD-i BIOS cdi200.rom not found"));
    CHECK(retro_serialize_size() == 0);
    CHECK(retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0);
}

TEST_CASE("a frontend refusing XRGB8888 can't load the core") {
    Core core;
    core.fe.accepts_pixel_format = false;
    core.start();
    CHECK_FALSE(core.load());
}

TEST_CASE("each run is one frame of video and audio, and a geometry change is announced once") {
    Core core;
    core.start();
    REQUIRE(core.load());
    for (int i = 0; i < 3; ++i) retro_run();
    CHECK(core.ram32(kCounter) != 0);
    CHECK(g_out.video_frames == 3);
    CHECK(g_out.pitch == 768 * sizeof(uint32_t));
    REQUIRE(core.fe.geometries.size() == 1);  // the shown area differs from the 768x280 announced
    CHECK(core.fe.geometries[0].base_width == g_out.width);
    CHECK(core.fe.geometries[0].base_height == g_out.height);
    CHECK(g_out.audio_batches == std::vector<size_t>(3, 44100 / 60));
}

TEST_CASE("without a machine, a run still gives the frame's audio but no picture") {
    Core core;
    core.start();
    retro_run();
    CHECK(g_out.video_frames == 0);
    CHECK(g_out.audio_batches == std::vector<size_t>{44100 / 60});
}

TEST_CASE("RAM and NVRAM are exposed, and the memory map goes before the machine") {
    Core core;
    core.start();
    REQUIRE(core.load());
    CHECK(retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) > 0);
    CHECK(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) > 0);  // NVRAM kept per disc
    CHECK(retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM) == 0);
    REQUIRE_FALSE(core.fe.memory_map_sizes.empty());
    CHECK(core.fe.memory_map_sizes.back() > 0);
    retro_unload_game();
    CHECK(core.fe.memory_map_sizes.back() == 0);
}

TEST_CASE("a saved state brings the machine back") {
    Core core;
    core.fe.variable_state_size = true;
    core.start();
    REQUIRE(core.load());
    retro_run();
    std::vector<uint8_t> state(retro_serialize_size());
    REQUIRE(retro_serialize(state.data(), state.size()));
    const uint32_t saved = core.ram32(kCounter);
    retro_run();
    REQUIRE(core.ram32(kCounter) != saved);
    CHECK(retro_unserialize(state.data(), state.size()));
    CHECK(core.ram32(kCounter) == saved);
}

TEST_CASE("a frontend without variable-size states gets one fixed size, padded with zeros") {
    Core core;
    core.start();
    REQUIRE(core.load());
    const size_t size = retro_serialize_size();
    CHECK(size == size_t(16) << 20);
    retro_run();
    CHECK(retro_serialize_size() == size);
    std::vector<uint8_t> state(size, 0xee);
    REQUIRE(retro_serialize(state.data(), state.size()));
    CHECK(state.back() == 0);
}

TEST_CASE("a state that doesn't fit or doesn't parse is refused") {
    Core core;
    core.start();
    REQUIRE(core.load());
    std::vector<uint8_t> small(64);
    CHECK_FALSE(retro_serialize(small.data(), small.size()));
    CHECK(core.logged("doesn't support variable-size states"));
    CHECK_FALSE(retro_serialize(nullptr, 0));
    CHECK_FALSE(retro_unserialize(small.data(), small.size()));
    CHECK(core.logged("Save state rejected"));
    CHECK_FALSE(retro_unserialize(nullptr, 0));
}

TEST_CASE("reset goes through the reset vector again, keeping RAM") {
    Core core;
    core.start();
    REQUIRE(core.load());
    retro_run();
    retro_reset();
    retro_run();
    CHECK(core.ram32(kBoots) == 2);
}

TEST_CASE("global NVRAM is read at load and written back at unload, and hidden from SAVE_RAM") {
    const std::string kNvram = "/saves/zcdi_cdi200.nvram";
    Core core;
    core.fe.save_dir = "/saves";
    core.fe.variables["zcdi_nvram_mode"] = "global";
    core.start();
    REQUIRE(core.load());
    CHECK(core.logged("not found, starting blank"));
    CHECK(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0);
    retro_unload_game();
    REQUIRE(core.fe.files.count(kNvram));
    std::vector<uint8_t> nvram = core.fe.files[kNvram];
    REQUIRE_FALSE(nvram.empty());

    for (std::size_t i = 0; i < 64; ++i) nvram[i] = uint8_t(i * 7);
    core.fe.files[kNvram] = nvram;
    REQUIRE(core.load());
    retro_unload_game();
    CHECK(std::vector<uint8_t>(core.fe.files[kNvram].begin(), core.fe.files[kNvram].begin() + 64) ==
          std::vector<uint8_t>(nvram.begin(), nvram.begin() + 64));
}

TEST_CASE("disk control: a disc loads into the tray, and swaps need the tray open") {
    Core core;
    core.fe.files["/content/game.chd"] = chd();
    core.fe.files["/content/other.bin"] = zcdi_test::raw_image(160);
    core.start();
    REQUIRE(core.load("/content/game.chd"));
    retro_disk_control_ext_callback& disk = core.fe.disk;
    CHECK(disk.get_num_images() == 1);
    CHECK(disk.get_image_index() == 0);
    CHECK(core.image_path(0) == "/content/game.chd");
    CHECK(core.image_label(0) == "game");
    CHECK(core.image_path(1) == "<none>");

    CHECK_FALSE(disk.set_image_index(0));  // tray closed
    REQUIRE(disk.set_eject_state(true));
    CHECK(disk.get_eject_state());

    // A new slot, filled: the empty-tray index moves up with the list.
    CHECK(disk.add_image_index());
    CHECK(disk.get_num_images() == 2);
    CHECK(core.image_path(1) == "<none>");  // added, not filled yet
    retro_game_info other{};
    other.path = "/content/other.bin";
    CHECK(disk.replace_image_index(1, &other));
    CHECK(disk.set_image_index(1));
    CHECK(disk.get_image_index() == 1);
    CHECK(core.image_label(1) == "other");

    // Past the list: an empty tray.
    CHECK(disk.set_image_index(2));
    CHECK(disk.get_image_index() == 2);
    CHECK(disk.set_eject_state(false));
    CHECK_FALSE(disk.get_eject_state());
}

TEST_CASE("disk control: an image that can't be opened leaves the current one") {
    Core core;
    core.fe.files["/content/game.chd"] = chd();
    core.start();
    REQUIRE(core.load("/content/game.chd"));
    retro_disk_control_ext_callback& disk = core.fe.disk;
    retro_game_info missing{};
    missing.path = "/content/missing.chd";
    CHECK_FALSE(disk.replace_image_index(0, &missing));  // tray closed
    REQUIRE(disk.set_eject_state(true));
    CHECK_FALSE(disk.replace_image_index(0, &missing));
    CHECK(core.told("Couldn't open disc image /content/missing.chd"));
    CHECK(core.image_path(0) == "/content/game.chd");
    CHECK_FALSE(disk.replace_image_index(5, &missing));
}

TEST_CASE("disk control: removing an image keeps the index on the same disc") {
    Core core;
    core.fe.files["/content/set.m3u"] = [] {
        const std::string m3u = "a.chd\nb.chd\nc.chd\n";
        return std::vector<uint8_t>(m3u.begin(), m3u.end());
    }();
    for (const char* name : {"/content/a.chd", "/content/b.chd", "/content/c.chd"}) core.fe.files[name] = chd();
    core.start();
    REQUIRE(core.load("/content/set.m3u"));
    retro_disk_control_ext_callback& disk = core.fe.disk;
    REQUIRE(disk.set_eject_state(true));
    REQUIRE(disk.set_image_index(1));

    CHECK(disk.replace_image_index(0, nullptr));  // before the current one
    CHECK(disk.get_num_images() == 2);
    CHECK(disk.get_image_index() == 0);
    CHECK(core.image_path(0) == "/content/b.chd");

    CHECK(disk.replace_image_index(1, nullptr));  // after it
    CHECK(disk.get_image_index() == 0);

    CHECK(disk.replace_image_index(0, nullptr));  // the current one: the tray empties
    CHECK(disk.get_num_images() == 0);
    CHECK(disk.get_image_index() == 0);
}

TEST_CASE("an m3u playlist lists its discs beside it, with their labels, from the initial image") {
    const std::string m3u = "# a set\ndisc1.chd|Side A\nsub/disc2.chd\n";
    for (const char* initial : {"/content/sub/disc2.chd", "/content/elsewhere.chd"}) {
        CAPTURE(initial);
        Core core;
        core.fe.files["/content/set.m3u"] = std::vector<uint8_t>(m3u.begin(), m3u.end());
        core.fe.files["/content/disc1.chd"] = chd();
        core.fe.files["/content/sub/disc2.chd"] = chd();
        core.start();
        retro_disk_control_ext_callback& disk = core.fe.disk;
        CHECK_FALSE(disk.set_initial_image(1, ""));
        REQUIRE(disk.set_initial_image(1, initial));
        REQUIRE(core.load("/content/set.m3u"));
        CHECK(disk.get_num_images() == 2);
        CHECK(core.image_label(0) == "Side A");
        CHECK(core.image_path(1) == "/content/sub/disc2.chd");
        CHECK(core.image_label(1) == "disc2");
        // Only if the playlist still has that disc at that index.
        CHECK(disk.get_image_index() == (std::string(initial) == "/content/sub/disc2.chd" ? 1u : 0u));
    }
}

TEST_CASE("content that can't be read fails the load and tells the player") {
    SUBCASE("a missing playlist") {
        Core core;
        core.start();
        CHECK_FALSE(core.load("/content/none.m3u"));
        CHECK(core.told("Couldn't read disc playlist /content/none.m3u"));
    }
    SUBCASE("a missing image") {
        Core core;
        core.start();
        CHECK_FALSE(core.load("/content/none.chd"));
        CHECK(core.told("Couldn't open disc image /content/none.chd"));
    }
}

TEST_CASE("disk control does nothing once the game is unloaded") {
    Core core;
    core.fe.files["/content/game.chd"] = chd();
    core.start();
    REQUIRE(core.load("/content/game.chd"));
    retro_unload_game();
    retro_disk_control_ext_callback& disk = core.fe.disk;
    CHECK_FALSE(disk.set_eject_state(true));
    CHECK_FALSE(disk.get_eject_state());
    CHECK(disk.get_num_images() == 0);
    CHECK_FALSE(disk.set_image_index(0));
}

}  // TEST_SUITE
