#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "disc/chd_disc.h"
#include "fixtures/chd_two_track_fixture.h"

using namespace zcdi;

namespace {

struct MemFile {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
};

std::unique_ptr<DiscContext> fixture_context() {
    auto ctx = std::make_unique<DiscContext>("two_track.chd");
    ctx->open_read = [](const std::string&) -> DiscContext::Handle {
        return new MemFile{zcdi_test::kChdTwoTrackFixtureBytes, sizeof(zcdi_test::kChdTwoTrackFixtureBytes)};
    };
    ctx->read = [](DiscContext::Handle h, void* dst, size_t len) -> size_t {
        auto* f = static_cast<MemFile*>(h);
        size_t n = len < f->size - f->pos ? len : f->size - f->pos;
        std::memcpy(dst, f->data + f->pos, n);
        f->pos += n;
        return n;
    };
    ctx->seek = [](DiscContext::Handle h, int64_t offset, int whence) -> int {
        auto* f = static_cast<MemFile*>(h);
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(f->pos) : int64_t(f->size);
        if (base + offset < 0 || base + offset > int64_t(f->size)) return -1;
        f->pos = size_t(base + offset);
        return 0;
    };
    ctx->size = [](DiscContext::Handle h) -> uint64_t { return static_cast<MemFile*>(h)->size; };
    ctx->close = [](DiscContext::Handle h) -> int {
        delete static_cast<MemFile*>(h);
        return 0;
    };
    return ctx;
}

}  // namespace

TEST_SUITE("disc") {

TEST_CASE("CHD tracks after the first skip chdman's padding frames") {
    std::unique_ptr<Disc> disc = open_chd_disc(fixture_context());
    REQUIRE(disc);

    auto toc = disc->toc_tracks();
    REQUIRE(toc.size() == 2);
    CHECK(toc[0].start_lba == 0);
    CHECK(toc[1].start_lba == 6);  // padding has no LBA
    CHECK(disc->lead_out_lba() == 9);

    // Track 1's 6 frames are followed by 2 padding frames in the CHD, so
    // track 2's first frame (LBA 6) is CHD frame 8.
    for (int lba = 0; lba < 9; ++lba) {
        CAPTURE(lba);
        RawSector s{};
        REQUIRE(disc->read_sector(lba, s));
        CHECK(s.data[0] == 0x10 + lba);
        CHECK(s.data[kRawSectorSize - 1] == 0x10 + lba);
    }
}

}  // TEST_SUITE
