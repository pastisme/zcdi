#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "disc/raw2352_disc.h"

using namespace zcdi;

namespace {

struct MemFile {
    const std::vector<uint8_t>* data = nullptr;
    size_t pos = 0;
};

// Frame i of a file is marked with first_mark + i in its first and last byte.
std::vector<uint8_t> marked_frames(int frames, int first_mark) {
    std::vector<uint8_t> data(size_t(frames) * kRawSectorSize, 0);
    for (int i = 0; i < frames; ++i) {
        data[size_t(i) * kRawSectorSize] = uint8_t(first_mark + i);
        data[size_t(i + 1) * kRawSectorSize - 1] = uint8_t(first_mark + i);
    }
    return data;
}

// The files outlive the disc: they are owned by the test case.
std::unique_ptr<DiscContext> memory_context(const std::string& path,
                                            const std::map<std::string, std::vector<uint8_t>>& files) {
    auto ctx = std::make_unique<DiscContext>(path);
    ctx->open_read = [&files](const std::string& p) -> DiscContext::Handle {
        auto it = files.find(p);
        return it == files.end() ? nullptr : new MemFile{&it->second};
    };
    ctx->read = [](DiscContext::Handle h, void* dst, size_t len) -> size_t {
        auto* f = static_cast<MemFile*>(h);
        size_t n = len < f->data->size() - f->pos ? len : f->data->size() - f->pos;
        std::memcpy(dst, f->data->data() + f->pos, n);
        f->pos += n;
        return n;
    };
    ctx->seek = [](DiscContext::Handle h, int64_t offset, int whence) -> int {
        auto* f = static_cast<MemFile*>(h);
        int64_t size = int64_t(f->data->size());
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(f->pos) : size;
        if (base + offset < 0 || base + offset > size) return -1;
        f->pos = size_t(base + offset);
        return 0;
    };
    ctx->size = [](DiscContext::Handle h) -> uint64_t { return static_cast<MemFile*>(h)->data->size(); };
    ctx->close = [](DiscContext::Handle h) -> int {
        delete static_cast<MemFile*>(h);
        return 0;
    };
    return ctx;
}

int mark_at(Disc& disc, int lba) {
    RawSector s{};
    if (!disc.read_sector(lba, s)) return -1;
    return s.data[0] == s.data[kRawSectorSize - 1] ? s.data[0] : -2;
}

}  // namespace

TEST_SUITE("disc") {

TEST_CASE("one .bin per track: the files are laid end to end") {
    std::map<std::string, std::vector<uint8_t>> files;
    const std::string cue =
        "FILE \"t1.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"t2.bin\" BINARY\n"
        "  TRACK 02 MODE2/2352\n"
        "    INDEX 00 00:00:00\n"
        "    INDEX 01 00:00:03\n"
        "FILE \"t3.bin\" BINARY\n"
        "  TRACK 03 AUDIO\n"
        "    INDEX 01 00:00:00\n";
    files["d/vcd.cue"].assign(cue.begin(), cue.end());
    files["d/t1.bin"] = marked_frames(5, 10);
    files["d/t2.bin"] = marked_frames(7, 20);  // 3 stored pregap frames, then the track
    files["d/t3.bin"] = marked_frames(4, 40);

    std::unique_ptr<Disc> disc = open_raw2352_disc(memory_context("d/vcd.cue", files));
    REQUIRE(disc);

    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 3);
    CHECK(toc[0].track_number == 1);
    CHECK(toc[0].start_lba == 0);
    CHECK_FALSE(toc[0].is_audio);
    CHECK(toc[1].start_lba == 5 + 3);  // INDEX 01, after the stored pregap
    CHECK(toc[2].start_lba == 5 + 7);
    CHECK(toc[2].is_audio);
    CHECK(disc->lead_out_lba() == 5 + 7 + 4);

    for (int lba = 0; lba < 5; ++lba) CHECK(mark_at(*disc, lba) == 10 + lba);
    for (int lba = 5; lba < 12; ++lba) CHECK(mark_at(*disc, lba) == 20 + lba - 5);
    for (int lba = 12; lba < 16; ++lba) CHECK(mark_at(*disc, lba) == 40 + lba - 12);
    CHECK(mark_at(*disc, 16) == -1);
}

TEST_CASE("a FILE path is relative to the cue unless absolute") {
    std::map<std::string, std::vector<uint8_t>> files;
    std::string name;
    std::string bin;
    SUBCASE("same directory") {
        name = "game.bin";
        bin = "d/game.bin";
    }
    SUBCASE("subdirectory") {
        name = "tracks/game.bin";
        bin = "d/tracks/game.bin";
    }
    SUBCASE("subdirectory, Windows separators") {
        name = "tracks\\game.bin";
        bin = "d/tracks/game.bin";
    }
    SUBCASE("absolute") {
        name = "/elsewhere/game.bin";
        bin = "/elsewhere/game.bin";
    }
    const std::string cue = "FILE \"" + name + "\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    files["d/game.cue"].assign(cue.begin(), cue.end());
    files[bin] = marked_frames(3, 0);

    std::unique_ptr<Disc> disc = open_raw2352_disc(memory_context("d/game.cue", files));
    REQUIRE(disc);
    CHECK(mark_at(*disc, 2) == 2);
}

TEST_CASE("a cue whose FILE path doesn't exist is rejected even with the bin next to it") {
    std::map<std::string, std::vector<uint8_t>> files;
    std::string name;
    SUBCASE("missing subdirectory") { name = "tracks/game.bin"; }
    SUBCASE("stale absolute Windows path") { name = "C:\\cdi\\games\\game.bin"; }
    const std::string cue = "FILE \"" + name + "\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    files["d/game.cue"].assign(cue.begin(), cue.end());
    files["d/game.bin"] = marked_frames(3, 0);
    CHECK_FALSE(open_raw2352_disc(memory_context("d/game.cue", files)));
}

TEST_CASE("single .bin: track 1's INDEX 01 is LBA 0, later tracks by INDEX 01") {
    std::map<std::string, std::vector<uint8_t>> files;
    const std::string cue =
        "FILE \"disc.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:02\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 00:00:06\n"
        "    INDEX 01 00:00:08\n";
    files["disc.cue"].assign(cue.begin(), cue.end());
    files["disc.bin"] = marked_frames(12, 0);

    std::unique_ptr<Disc> disc = open_raw2352_disc(memory_context("disc.cue", files));
    REQUIRE(disc);
    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 2);
    CHECK(toc[0].start_lba == 0);
    CHECK(toc[1].start_lba == 6);
    CHECK(disc->lead_out_lba() == 10);
    CHECK(mark_at(*disc, 0) == 2);
    CHECK(mark_at(*disc, 9) == 11);
    CHECK(mark_at(*disc, -2) == 0);  // track 1's stored pregap
}

TEST_CASE("PREGAP adds frames that read as zeros") {
    std::map<std::string, std::vector<uint8_t>> files;
    const std::string cue =
        "FILE \"a.bin\" BINARY\n"
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"b.bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    PREGAP 00:00:02\n"
        "    INDEX 01 00:00:00\n";
    files["x.cue"].assign(cue.begin(), cue.end());
    files["a.bin"] = marked_frames(3, 1);
    files["b.bin"] = marked_frames(3, 50);

    std::unique_ptr<Disc> disc = open_raw2352_disc(memory_context("x.cue", files));
    REQUIRE(disc);
    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 2);
    CHECK(toc[1].start_lba == 3 + 2);
    CHECK(disc->lead_out_lba() == 8);
    CHECK(mark_at(*disc, 2) == 3);
    CHECK(mark_at(*disc, 3) == 0);
    CHECK(mark_at(*disc, 4) == 0);
    CHECK(mark_at(*disc, 5) == 50);
}

TEST_CASE("CD-i Ready: LBA 0 is the file start, in track 1's pregap") {
    std::map<std::string, std::vector<uint8_t>> files;
    const std::string cue =
        "FILE \"ready.bin\" BINARY\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 00 00:00:00\n"
        "    INDEX 01 00:00:04\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:06\n";
    files["ready.cue"].assign(cue.begin(), cue.end());
    // The application's Mode 2 sectors (headers 00:02:00 on), then audio.
    std::vector<uint8_t>& bin = files["ready.bin"] = marked_frames(8, 0);
    for (int lba = 0; lba < 4; ++lba) {
        uint8_t* sector = bin.data() + size_t(lba) * kRawSectorSize;
        sector[kSyncSize + 0] = 0x00;
        sector[kSyncSize + 1] = 0x02;
        sector[kSyncSize + 2] = uint8_t(lba);
        sector[kSyncSize + 3] = 2;
    }

    std::unique_ptr<Disc> disc = open_raw2352_disc(memory_context("ready.cue", files));
    REQUIRE(disc);
    RawSector s{};
    REQUIRE(disc->read_sector(0, s));
    CHECK(s.data[kSyncSize + 1] == 0x02);
    CHECK(s.data[kSyncSize + 3] == 2);
    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 2);
    CHECK(toc[0].is_audio);
    CHECK(toc[0].start_lba == 4);
    CHECK(toc[1].start_lba == 6);
    CHECK(disc->lead_out_lba() == 8);
    CHECK(mark_at(*disc, 4) == 4);
}

TEST_CASE("cues that aren't raw 2352-byte data are rejected") {
    std::map<std::string, std::vector<uint8_t>> files;
    files["a.bin"] = marked_frames(3, 1);
    SUBCASE("WAVE file") {
        const std::string cue = "FILE \"a.bin\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n";
        files["x.cue"].assign(cue.begin(), cue.end());
    }
    SUBCASE("stripped 2048-byte track") {
        const std::string cue = "FILE \"a.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n";
        files["x.cue"].assign(cue.begin(), cue.end());
    }
    SUBCASE("missing file") {
        const std::string cue = "FILE \"a.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                                "FILE \"gone.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n";
        files["x.cue"].assign(cue.begin(), cue.end());
    }
    CHECK_FALSE(open_raw2352_disc(memory_context("x.cue", files)));
}

TEST_CASE("bare .bin/.iso: a 2048-byte ISO9660 image is rejected by its missing sync") {
    std::map<std::string, std::vector<uint8_t>> files;
    // 147 ISO sectors are 128 raw frames: the size check alone passes.
    files["game.iso"] = std::vector<uint8_t>(size_t(147) * 2048, 0);
    REQUIRE(files["game.iso"].size() % kRawSectorSize == 0);
    CHECK_FALSE(open_raw2352_disc(memory_context("game.iso", files)));

    static constexpr uint8_t kSync[kSyncSize] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::vector<uint8_t> raw = marked_frames(160, 0);
    SUBCASE("sync at the file start") {
        std::memcpy(raw.data(), kSync, kSyncSize);
    }
    SUBCASE("sync only at LBA 0, after a 150-frame pregap") {
        std::memcpy(raw.data() + size_t(150) * kRawSectorSize, kSync, kSyncSize);
    }
    SUBCASE("byteswapped sync") {
        byteswap_sector(raw.data(), kRawSectorSize);
        std::memcpy(raw.data(), kSync, kSyncSize);
        byteswap_sector(raw.data(), kRawSectorSize);
    }
    files["game.iso"] = raw;
    CHECK(open_raw2352_disc(memory_context("game.iso", files)));
}

}  // TEST_SUITE
