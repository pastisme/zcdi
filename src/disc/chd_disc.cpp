#include "disc/chd_disc.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#include "disc/file_io.h"
#include "disc/sector.h"

namespace zcdi {

namespace {

// 2352 sector + 96 subcode per unit; subcode isn't modelled.
constexpr uint32_t kFrameSize = CD_FRAME_SIZE;

// chdman TYPE strings that keep sync, header and XA subheader.
bool is_raw_2352(const std::string& type) {
    return type == "MODE1_RAW" || type == "MODE1/2352" || type == "MODE2_RAW" || type == "MODE2/2352" ||
           type == "CDI/2352" || type == "AUDIO";
}

// As MAME cdrom.cpp: logframeofs in disc-wide LBAs, chdframeofs in CHD frames.
struct Track {
    uint32_t frames = 0; // frames physically present for this track
    uint32_t logframeofs = 0;
    uint32_t chdframeofs = 0;
    uint32_t stored_pregap = 0; // frames before INDEX 01 (PGTYPE V...)
    int track_number = 0; // as declared in the CHD's own track metadata
    bool is_audio = false;
};

uint64_t core_file_size_cb(void* file) { return disc_file_size(static_cast<DiscFile*>(file)); }

size_t core_file_read_cb(void* ptr, size_t size, size_t nmemb, void* file) {
    if (size == 0 || nmemb == 0) return 0;
    if (nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    size_t got = disc_file_read(static_cast<DiscFile*>(file), ptr, total);
    return got / size;
}

int core_file_close_cb(void* file) { return disc_file_close(static_cast<DiscFile*>(file)); }

int core_file_seek_cb(void* file, int64_t offset, int whence) {
    return disc_file_seek(static_cast<DiscFile*>(file), offset, whence);
}

const core_file_callbacks kCoreFileCallbacks = {
    core_file_size_cb,
    core_file_read_cb,
    core_file_close_cb,
    core_file_seek_cb,
};

} // namespace

class ChdDisc : public Disc {
public:
    explicit ChdDisc(std::unique_ptr<DiscContext> disc_ctx) : Disc(std::move(disc_ctx)) {}

    bool open() {
        if (!disc_ctx_) return false;
        if (!disc_file_open_read(disc_ctx_->path(), source_file_, *disc_ctx_)) return false;
        chd_file* chd = nullptr;
        if (chd_open_core_file_callbacks(&kCoreFileCallbacks, &source_file_, CHD_OPEN_READ, nullptr, &chd) !=
            CHDERR_NONE) {
            disc_file_close(&source_file_);
            return false;
        }
        chd_.reset(chd);

        const chd_header* header = chd_get_header(chd_.get());
        if (!header || header->unitbytes != kFrameSize || header->hunkbytes == 0 ||
            header->hunkbytes % kFrameSize != 0) {
            return false; // not a CD CHD in the layout we know how to read
        }
        frames_per_hunk_ = header->hunkbytes / kFrameSize;
        hunk_buf_.resize(header->hunkbytes);

        return parse_tracks();
    }

    bool read_sector(int lba, RawSector& out) override {
        if (tracks_.empty() || lba < 0) return false;

        const Track* t = nullptr;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            uint32_t next_ofs = (i + 1 < tracks_.size()) ? tracks_[i + 1].logframeofs : total_logframes_;
            if (static_cast<uint32_t>(lba) < next_ofs) {
                t = &tracks_[i];
                break;
            }
        }
        if (!t) return false;

        if (static_cast<uint32_t>(lba) < t->logframeofs) {
            std::memset(out.data, 0, sizeof(out.data));
            out.sub = Subheader{};
            return true; // synthesized (never physically stored) pregap
        }

        uint32_t chd_frame = t->chdframeofs + (static_cast<uint32_t>(lba) - t->logframeofs);
        if (!read_frame(chd_frame, out.data)) return false;

        finish_sector(lba, out);
        return true;
    }

    int lead_out_lba() const override { return static_cast<int>(total_logframes_); }
    bool cdda_big_endian() const override { return !byteswap_detected(); }

    std::vector<TocTrack> toc_tracks() const override {
        std::vector<TocTrack> out;
        out.reserve(tracks_.size());
        for (const Track& t : tracks_) {
            // The TOC gives INDEX 01. A CD-i Ready disc's application sits
            // in track 1's stored pregap, before its audio.
            out.push_back({.track_number = t.track_number,
                           .is_audio = t.is_audio,
                           .start_lba = static_cast<int>(t.logframeofs + t.stored_pregap)});
        }
        return out;
    }

private:
    bool parse_tracks() {
        uint32_t log_ofs = 0, chd_ofs = 0;

        for (uint32_t i = 0;; ++i) {
            char meta[256];
            uint32_t result_len = 0, result_tag = 0;
            uint8_t flags = 0;

            chd_error err = chd_get_metadata(chd_.get(), CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta), &result_len,
                                             &result_tag, &flags);
            bool have_pregap_fields = true;
            if (err != CHDERR_NONE) {
                err = chd_get_metadata(chd_.get(), CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta), &result_len,
                                       &result_tag, &flags);
                have_pregap_fields = false;
            }
            if (err != CHDERR_NONE) break; // no more tracks

            int track_num = 0, frames = 0, pregap = 0, postgap = 0;
            char type[32] = {}, subtype[32] = {}, pgtype[32] = {}, pgsub[32] = {};

            if (have_pregap_fields) {
                if (std::sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &track_num, type, subtype, &frames, &pregap, pgtype,
                                pgsub, &postgap) != 8) {
                    return false;
                }
            } else {
                if (std::sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &track_num, type, subtype, &frames) != 4) {
                    return false;
                }
            }

            if (!is_raw_2352(type)) return false; // stripped sector data -- can't recover the subheader

            // A stored ('V') pregap is read as track content, never skipped:
            // CD-i Ready discs keep the application there.
            const Track t{
                .frames = static_cast<uint32_t>(frames),
                .logframeofs = log_ofs,
                .chdframeofs = chd_ofs,
                .stored_pregap = pgtype[0] == 'V' ? static_cast<uint32_t>(pregap) : 0u,
                .track_number = track_num,
                .is_audio = std::string(type) == "AUDIO",
            };

            log_ofs += t.frames;
            // chdman's padding (MAME cdrom.cpp extraframes) has no LBA.
            chd_ofs += (t.frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING * CD_TRACK_PADDING;
            log_ofs += static_cast<uint32_t>(postgap); // postgap counts against the next track

            tracks_.push_back(t);
        }

        total_logframes_ = log_ofs;
        return !tracks_.empty();
    }

    bool read_frame(uint32_t chd_frame, uint8_t* dest2352) {
        uint32_t hunk = chd_frame / frames_per_hunk_;
        uint32_t frame_in_hunk = chd_frame % frames_per_hunk_;
        if (!have_cached_hunk_ || hunk != cached_hunk_) {
            if (chd_read(chd_.get(), hunk, hunk_buf_.data()) != CHDERR_NONE) return false;
            cached_hunk_ = hunk;
            have_cached_hunk_ = true;
        }
        std::memcpy(dest2352, hunk_buf_.data() + static_cast<size_t>(frame_in_hunk) * kFrameSize, CD_MAX_SECTOR_DATA);
        return true;
    }

    struct ChdCloser {
        void operator()(chd_file* chd) const { chd_close(chd); }
    };
    DiscFile source_file_;
    // After source_file_, so it is closed first: it reads through it.
    std::unique_ptr<chd_file, ChdCloser> chd_;
    std::vector<uint8_t> hunk_buf_;
    uint32_t frames_per_hunk_ = 0;
    bool have_cached_hunk_ = false;
    uint32_t cached_hunk_ = 0;
    std::vector<Track> tracks_;
    uint32_t total_logframes_ = 0;
};

std::unique_ptr<Disc> open_chd_disc(std::unique_ptr<DiscContext> disc_ctx) {
    auto disc = std::make_unique<ChdDisc>(std::move(disc_ctx));
    if (!disc->open()) return nullptr;
    return disc;
}

} // namespace zcdi
