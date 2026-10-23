#include "disc/raw2352_disc.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <vector>

#include "disc/cue_parser.h"
#include "disc/file_io.h"
#include "disc/sector.h"

namespace zcdi {

namespace {
// Same byteswap/descramble attempts as read_sector, without the latch.
// Leaves the file position wherever it ended.
bool sector_at_offset_looks_like_lba0(DiscFile& bin, uint64_t byte_offset) {
    uint8_t buf[kRawSectorSize];
    if (disc_file_seek(&bin, static_cast<int64_t>(byte_offset), SEEK_SET) != 0) return false;
    if (disc_file_read(&bin, buf, kRawSectorSize) != kRawSectorSize) return false;
    if (sector_looks_byteswapped(buf)) byteswap_sector(buf, kRawSectorSize);
    if (sector_looks_valid(buf, 0)) return true;
    descramble_sector(buf);
    return sector_looks_valid(buf, 0);
}

// A raw sector starts with the sync field (scrambling leaves it alone).
bool sector_at_offset_has_sync(DiscFile& bin, uint64_t byte_offset) {
    static constexpr uint8_t kSync[kSyncSize] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    uint8_t buf[kSyncSize];
    if (disc_file_seek(&bin, static_cast<int64_t>(byte_offset), SEEK_SET) != 0) return false;
    if (disc_file_read(&bin, buf, kSyncSize) != kSyncSize) return false;
    return std::memcmp(buf, kSync, kSyncSize) == 0 || sector_looks_byteswapped(buf);
}

bool is_raw_2352_mode(const std::string& mode) {
    return mode == "AUDIO" || (mode.size() > 5 && mode.compare(mode.size() - 5, 5, "/2352") == 0);
}

// The cue's files end to end plus PREGAP gaps, as chdman lays out a CHD.
class Raw2352Disc : public Disc {
public:
    explicit Raw2352Disc(std::unique_ptr<DiscContext> disc_ctx) : Disc(std::move(disc_ctx)) {}

    bool open() {
        if (!disc_ctx_) return false;
        const DiscContext& ctx = *disc_ctx_;
        const std::string& path = ctx.path();
        bool have_cue = disc_path_ext(path) == "cue";

        CueSheet sheet;
        if (have_cue) {
            std::string text;
            if (!disc_read_whole_file(path, text, ctx)) return false;
            if (!parse_cue(text, sheet)) return false;
        } else {
            // One Mode 2 track; most such dumps keep the 150-sector pregap.
            sheet.files.push_back({.name = disc_generic_path(path).filename().string(), .type = "BINARY"});
            sheet.tracks.push_back({.number = 1, .mode = "MODE2/2352", .file = 0, .index01_frames = 150});
        }

        // Raw 2352-byte sectors in little-endian files only.
        for (const CueFile& f : sheet.files) {
            if (f.type != "BINARY") return false;
        }
        for (const CueTrack& t : sheet.tracks) {
            if (!is_raw_2352_mode(t.mode)) return false;
        }

        std::vector<int> file_frames;
        for (const CueFile& f : sheet.files) {
            files_.push_back(std::make_unique<DiscFile>());
            std::string bin_path = have_cue ? disc_path_beside(path, f.name) : path;
            if (!disc_file_open_read(bin_path, *files_.back(), ctx)) return false;
            uint64_t size = disc_file_size(files_.back().get());
            if (size == static_cast<uint64_t>(-1) || size == 0 || size % kRawSectorSize != 0) return false;
            if (size / kRawSectorSize > static_cast<uint64_t>(INT_MAX / 2)) return false;
            file_frames.push_back(static_cast<int>(size / kRawSectorSize));
        }

        // A bare file whose size happens to be a multiple of 2352 (a 2048-byte
        // ISO9660 image of 147n sectors) has no sync at the start or at LBA 0.
        if (!have_cue && !sector_at_offset_has_sync(*files_[0], 0) &&
            !sector_at_offset_has_sync(*files_[0], uint64_t(sheet.tracks.front().index01_frames) * kRawSectorSize)) {
            return false;
        }

        // LBA 0 is the file start when only that sector looks like LBA 0: a
        // bare .bin with no pregap, or a CD-i Ready disc.
        CueTrack& first = sheet.tracks.front();
        const bool lba0_at_file_start =
            !sector_at_offset_looks_like_lba0(*files_[0], uint64_t(first.index01_frames) * kRawSectorSize) &&
            sector_at_offset_looks_like_lba0(*files_[0], 0);
        if (lba0_at_file_start && !have_cue) first.index01_frames = 0;

        // PREGAP goes just before INDEX 01; stored data before it stays put.
        int disc_frame = 0;
        std::vector<int> track_start;
        for (int fi = 0; fi < int(sheet.files.size()); ++fi) {
            int file_frame = 0;
            for (const CueTrack& t : sheet.tracks) {
                if (t.file != fi) continue;
                int index01 = std::min(t.index01_frames, file_frames[fi]);
                if (index01 < file_frame) return false; // INDEX 01 before the previous track's
                add_span(disc_frame, fi, file_frame, index01 - file_frame);
                add_span(disc_frame, kGap, 0, t.pregap_frames);
                file_frame = index01;
                track_start.push_back(disc_frame);
            }
            add_span(disc_frame, fi, file_frame, file_frames[fi] - file_frame);
        }
        lba0_frame_ = lba0_at_file_start ? 0 : track_start.front();
        end_frame_ = disc_frame;
        for (size_t i = 0; i < sheet.tracks.size(); ++i) {
            toc_.push_back({.track_number = sheet.tracks[i].number,
                            .is_audio = sheet.tracks[i].mode == "AUDIO",
                            .start_lba = track_start[i] - lba0_frame_});
        }
        return true;
    }

    bool read_sector(int lba, RawSector& out) override {
        if (lba < -lba0_frame_ || lba >= end_frame_ - lba0_frame_) return false;
        const int disc_frame = lba + lba0_frame_;
        const Span* span = nullptr;
        for (const Span& s : spans_) {
            if (disc_frame < s.disc_frame + s.frames) {
                span = &s;
                break;
            }
        }
        if (!span) return false;

        if (span->file == kGap) {
            std::memset(out.data, 0, kRawSectorSize);
        } else {
            DiscFile* file = files_[span->file].get();
            int64_t offset = int64_t(span->file_frame + disc_frame - span->disc_frame) * kRawSectorSize;
            if (disc_file_seek(file, offset, SEEK_SET) != 0) return false;
            if (disc_file_read(file, out.data, kRawSectorSize) != kRawSectorSize) return false;
        }

        finish_sector(lba, out);
        return true;
    }

    int lead_out_lba() const override { return end_frame_ - lba0_frame_; }
    std::vector<TocTrack> toc_tracks() const override { return toc_; }

private:
    static constexpr int kGap = -1;
    struct Span {
        int disc_frame = 0;
        int frames = 0;
        int file = kGap;
        int file_frame = 0;
    };

    void add_span(int& disc_frame, int file, int file_frame, int frames) {
        if (frames <= 0) return;
        spans_.push_back({.disc_frame = disc_frame, .frames = frames, .file = file, .file_frame = file_frame});
        disc_frame += frames;
    }

    std::vector<std::unique_ptr<DiscFile>> files_;
    std::vector<Span> spans_;
    std::vector<TocTrack> toc_;
    int lba0_frame_ = 0; // disc frame of LBA 0 (track 1's INDEX 01)
    int end_frame_ = 0;
};

} // namespace

std::unique_ptr<Disc> open_raw2352_disc(std::unique_ptr<DiscContext> disc_ctx) {
    auto disc = std::make_unique<Raw2352Disc>(std::move(disc_ctx));
    if (!disc->open()) return nullptr;
    return disc;
}

} // namespace zcdi
