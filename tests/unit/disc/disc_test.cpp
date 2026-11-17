// Picking a disc reader by file name, and DiscContext's default file access.
#include "doctest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "disc/chd_disc.h"
#include "disc/disc.h"
#include "disc/sector.h"
#include "fixtures/chd_two_track_fixture.h"
#include "support/mem_files.h"

using namespace zcdi;
using zcdi_test::bytes_of;
using zcdi_test::memory_context;
using zcdi_test::raw_image;

namespace {

std::unique_ptr<Disc> open_named(const std::string& name, std::vector<uint8_t> bytes) {
    return open_disc(memory_context(name, {{name, std::move(bytes)}}));
}

}  // namespace

TEST_SUITE("disc") {

TEST_CASE("the extension picks the reader, in any case") {
    const std::vector<uint8_t> chd = bytes_of(zcdi_test::kChdTwoTrackFixtureBytes);
    for (const char* name : {"game.chd", "GAME.CHD"}) {
        CAPTURE(name);
        std::unique_ptr<Disc> disc = open_named(name, chd);
        REQUIRE(disc);
        CHECK(disc->lead_out_lba() == 9);
    }
    for (const char* name : {"game.bin", "game.iso", "Game.Bin"}) {
        CAPTURE(name);
        std::unique_ptr<Disc> disc = open_named(name, raw_image(160));
        REQUIRE(disc);
        CHECK(disc->lead_out_lba() == 160 - 150);  // a bare image keeps the 150-sector pregap
    }
    const std::string cue = "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    std::unique_ptr<Disc> disc = open_disc(memory_context(
        "game.cue", {{"game.cue", std::vector<uint8_t>(cue.begin(), cue.end())}, {"game.bin", raw_image(160)}}));
    CHECK(disc);
}

TEST_CASE("other names and a missing context open nothing") {
    for (const char* name : {"game.gdi", "game.m3u", "game.cdi", "game"}) {
        CAPTURE(name);
        CHECK_FALSE(open_named(name, raw_image(160)));
    }
    CHECK_FALSE(open_disc(nullptr));
}

TEST_CASE("a file named .chd that isn't a CHD is refused") {
    CHECK_FALSE(open_named("game.chd", raw_image(160)));
}

TEST_CASE("CD-DA in a CHD is big-endian") {
    std::unique_ptr<Disc> disc = open_named("game.chd", bytes_of(zcdi_test::kChdTwoTrackFixtureBytes));
    REQUIRE(disc);
    CHECK(disc->cdda_big_endian());
}

TEST_CASE("a cue sheet that can't be sized or read whole opens nothing") {
    const std::string cue = "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    const zcdi_test::MemFiles files = {{"game.cue", std::vector<uint8_t>(cue.begin(), cue.end())},
                                       {"game.bin", raw_image(160)}};
    std::unique_ptr<DiscContext> unsized = memory_context("game.cue", files);
    unsized->size = [](DiscContext::Handle) { return uint64_t(-1); };
    CHECK_FALSE(open_disc(std::move(unsized)));

    std::unique_ptr<DiscContext> short_read = memory_context("game.cue", files);
    short_read->read = [](DiscContext::Handle, void*, size_t) -> size_t { return 0; };
    CHECK_FALSE(open_disc(std::move(short_read)));
}

TEST_CASE("the default file access refuses a missing file and null handles") {
    DiscContext ctx("/zcdi-test-absent/none.chd");
    CHECK(ctx.open_read(ctx.path()) == nullptr);
    uint8_t buf[4] = {};
    CHECK(ctx.read(nullptr, buf, sizeof(buf)) == 0);
    CHECK(ctx.seek(nullptr, 0, SEEK_SET) == -1);
    CHECK(ctx.size(nullptr) == uint64_t(-1));
    CHECK(ctx.close(nullptr) == -1);
}

}  // TEST_SUITE
