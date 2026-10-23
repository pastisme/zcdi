#include "doctest.h"

#include <string>

#include "disc/file_io.h"

using namespace zcdi;

TEST_SUITE("disc") {

TEST_CASE("cue and m3u entries resolve against the list's folder unless absolute") {
    SUBCASE("relative to the list's folder") {
        CHECK(disc_path_beside("/roms/cdi/Game.m3u", "Game (Disc 1).chd") == "/roms/cdi/Game (Disc 1).chd");
        CHECK(disc_path_beside("/roms/cdi/Game.cue", "tracks/Game.bin") == "/roms/cdi/tracks/Game.bin");
        CHECK(disc_path_beside("/roms/cdi/Game.m3u", "discs/../Game.cue") == "/roms/cdi/Game.cue");
    }
    SUBCASE("Windows separators in the entry") {
        CHECK(disc_path_beside("/roms/cdi/Game.m3u", "discs\\d1.chd") == "/roms/cdi/discs/d1.chd");
    }
    SUBCASE("absolute entries kept") {
        CHECK(disc_path_beside("/roms/cdi/Game.m3u", "/other/d1.chd") == "/other/d1.chd");
        CHECK(disc_path_beside("/roms/cdi/Game.cue", "C:\\cdi\\d1.bin") == "C:\\cdi\\d1.bin");
        CHECK(disc_path_beside("/roms/cdi/Game.cue", "\\\\server\\cdi\\d1.bin") == "\\\\server\\cdi\\d1.bin");
    }
    SUBCASE("list without folder") {
        CHECK(disc_path_beside("Game.m3u", "d1.chd") == "d1.chd");
    }
}

}  // TEST_SUITE
