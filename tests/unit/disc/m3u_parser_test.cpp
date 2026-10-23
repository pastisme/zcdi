#include "doctest.h"

#include <string>
#include <vector>

#include "disc/m3u_parser.h"

using namespace zcdi;

TEST_SUITE("disc") {

TEST_CASE("m3u with two discs") {
    std::vector<M3uEntry> entries;
    REQUIRE(parse_m3u("Game (Disc 1).chd\nGame (Disc 2).chd\n", entries));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].path == "Game (Disc 1).chd");
    CHECK(entries[1].path == "Game (Disc 2).chd");
    CHECK(entries[0].label.empty());
}

TEST_CASE("m3u comments, blank lines, CRLF and BOM") {
    std::vector<M3uEntry> entries;
    REQUIRE(parse_m3u("\xEF\xBB\xBF#EXTM3U\r\n\r\n#EXTINF:0,Disc 1\r\n  disc1.cue  \r\n# disc2.cue\r\n", entries));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == "disc1.cue");
}

TEST_CASE("m3u labels after '|'") {
    std::vector<M3uEntry> entries;
    REQUIRE(parse_m3u("disc1.chd|Main game\ndisc2.chd | Bonus\n|orphan label\n", entries));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].path == "disc1.chd");
    CHECK(entries[0].label == "Main game");
    CHECK(entries[1].path == "disc2.chd");
    CHECK(entries[1].label == "Bonus");
}

TEST_CASE("m3u without entries fails") {
    std::vector<M3uEntry> entries;
    CHECK_FALSE(parse_m3u("", entries));
    CHECK_FALSE(parse_m3u("#EXTM3U\n\n", entries));
}

}  // TEST_SUITE
