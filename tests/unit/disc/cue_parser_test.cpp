#include "doctest.h"

#include <string>

#include "disc/cue_parser.h"

using namespace zcdi;

TEST_SUITE("disc") {

TEST_CASE("single-track cue") {
    const std::string cue =
        "FILE \"game.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:02:00\n";
    CueSheet sheet;
    REQUIRE(parse_cue(cue, sheet));
    CHECK(sheet.files[0].name == "game.bin");
    REQUIRE(sheet.tracks.size() == 1);
    CHECK(sheet.tracks[0].number == 1);
    CHECK(sheet.tracks[0].mode == "MODE2/2352");
    CHECK(sheet.tracks[0].index01_frames == 2 * 75);
}

TEST_CASE("CD-i track followed by CD-DA tracks") {
    const std::string cue =
        "FILE \"disc.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 10:00:00\n"
        "    INDEX 01 10:02:00\n"
        "  TRACK 03 AUDIO\n"
        "    INDEX 01 12:34:56\n";
    CueSheet sheet;
    REQUIRE(parse_cue(cue, sheet));
    REQUIRE(sheet.tracks.size() == 3);
    CHECK(sheet.tracks[0].mode == "MODE2/2352");
    CHECK(sheet.tracks[0].index01_frames == 0);
    CHECK(sheet.tracks[1].number == 2);
    CHECK(sheet.tracks[1].mode == "AUDIO");
    // INDEX 00 (the pregap start) is not INDEX 01.
    CHECK(sheet.tracks[1].index01_frames == (10 * 60 + 2) * 75);
    CHECK(sheet.tracks[2].index01_frames == (12 * 60 + 34) * 75 + 56);
}

TEST_CASE("FILE name forms") {
    CueSheet sheet;
    SUBCASE("quoted, with spaces") {
        REQUIRE(parse_cue("FILE \"Rise of the Robots (Europe).bin\" BINARY\nTRACK 01 MODE2/2352\n", sheet));
        CHECK(sheet.files[0].name == "Rise of the Robots (Europe).bin");
    }
    SUBCASE("quoted Windows path keeps only the file name") {
        REQUIRE(parse_cue("FILE \"C:\\cdi\\games\\game.bin\" BINARY\nTRACK 01 MODE2/2352\n", sheet));
        CHECK(sheet.files[0].name == "game.bin");
    }
    SUBCASE("unquoted, with spaces") {
        REQUIRE(parse_cue("FILE my game.bin BINARY\nTRACK 01 MODE2/2352\n", sheet));
        CHECK(sheet.files[0].name == "my game.bin");
    }
}

TEST_CASE("comments and blank lines are skipped") {
    const std::string cue =
        "; written by some tool\n"
        "// cdrdao style comment\n"
        "\n"
        "FILE \"game.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:10\n";
    CueSheet sheet;
    REQUIRE(parse_cue(cue, sheet));
    CHECK(sheet.files[0].name == "game.bin");
    REQUIRE(sheet.tracks.size() == 1);
    CHECK(sheet.tracks[0].index01_frames == 10);
}

TEST_CASE("CRLF line endings") {
    CueSheet sheet;
    SUBCASE("quoted name") {
        REQUIRE(parse_cue("FILE \"game.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:02:00\r\n", sheet));
        CHECK(sheet.files[0].name == "game.bin");
        REQUIRE(sheet.tracks.size() == 1);
        CHECK(sheet.tracks[0].index01_frames == 2 * 75);
    }
    SUBCASE("unquoted name") {
        REQUIRE(parse_cue("FILE game.bin BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:02:00\r\n", sheet));
        CHECK(sheet.files[0].name == "game.bin");
    }
}

TEST_CASE("a cue with no track, or a track before any FILE, is rejected") {
    CueSheet sheet;
    CHECK_FALSE(parse_cue("", sheet));
    CHECK_FALSE(parse_cue("FILE \"game.bin\" BINARY\n", sheet));
    CHECK_FALSE(parse_cue("TRACK 01 MODE2/2352\nFILE \"game.bin\" BINARY\n", sheet));
}

TEST_CASE("one file per track (Redump Video CD)") {
    const std::string cue =
        "CATALOG 0000000000000\n"
        "FILE \"vcd (Track 1).bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"vcd (Track 2).bin\" BINARY\n"
        "  TRACK 02 MODE2/2352\n"
        "    INDEX 00 00:00:00\n"
        "    INDEX 01 00:02:00\n"
        "    INDEX 02 02:55:73\n";
    CueSheet sheet;
    REQUIRE(parse_cue(cue, sheet));
    REQUIRE(sheet.files.size() == 2);
    CHECK(sheet.files[0].name == "vcd (Track 1).bin");
    CHECK(sheet.files[1].name == "vcd (Track 2).bin");
    CHECK(sheet.files[1].type == "BINARY");
    REQUIRE(sheet.tracks.size() == 2);
    CHECK(sheet.tracks[0].file == 0);
    CHECK(sheet.tracks[1].file == 1);
    CHECK(sheet.tracks[1].index01_frames == 150);  // within its own file
}

TEST_CASE("FILE type and PREGAP") {
    const std::string cue =
        "FILE game.bin BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"music.wav\" WAVE\n"
        "  TRACK 02 AUDIO\n"
        "    PREGAP 00:02:00\n"
        "    INDEX 01 00:00:00\n";
    CueSheet sheet;
    REQUIRE(parse_cue(cue, sheet));
    REQUIRE(sheet.files.size() == 2);
    CHECK(sheet.files[0].type == "BINARY");
    CHECK(sheet.files[1].name == "music.wav");
    CHECK(sheet.files[1].type == "WAVE");
    CHECK(sheet.tracks[0].pregap_frames == 0);
    CHECK(sheet.tracks[1].pregap_frames == 150);
}

}  // TEST_SUITE
