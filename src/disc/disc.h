#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "disc/sector.h"

namespace zcdi {

// File hooks every backend reads through; stdio by default. Replace them
// before open_disc() to read from another file layer (e.g. a frontend VFS).
class DiscContext {
public:
    using Handle = void*;
    using OpenReadFn = std::function<Handle(const std::string&)>;
    using ReadFn = std::function<size_t(Handle, void*, size_t)>;
    using SeekFn = std::function<int(Handle, int64_t, int)>;
    using SizeFn = std::function<uint64_t(Handle)>;
    using CloseFn = std::function<int(Handle)>;

    explicit DiscContext(std::string path);

    const std::string& path() const { return path_; }

    OpenReadFn open_read;
    ReadFn read;
    SeekFn seek;
    SizeFn size;
    CloseFn close;

private:
    const std::string path_;
};

// One raw 2352-byte Mode 2 sector plus its parsed subheader.
struct RawSector {
    uint8_t data[kRawSectorSize];
    Subheader sub;
};

// For CDIC's Fetch TOC. track_number is 1-based; start_lba is INDEX 01, in
// read_sector()'s LBA space.
struct TocTrack {
    int track_number = 0;
    bool is_audio = false;
    int start_lba = 0;
};

class Disc {
public:
    explicit Disc(std::unique_ptr<DiscContext> disc_ctx) : disc_ctx_(std::move(disc_ctx)) {}

    virtual ~Disc() = default;
    virtual bool read_sector(int lba, RawSector& out) = 0;
    virtual int lead_out_lba() const = 0;
    // The byteswapped-dump latch; save states must restore it.
    bool byteswap_detected() const { return byteswap_latched_; }
    void set_byteswap_detected(bool latched) { byteswap_latched_ = latched; }
    virtual bool cdda_big_endian() const { return false; }
    virtual std::vector<TocTrack> toc_tracks() const { return {{1, false, 0}}; }

protected:
    // Byteswap latch, Green Book descrambling, subheader parse.
    void finish_sector(int lba, RawSector& out);

    std::unique_ptr<DiscContext> disc_ctx_;

private:
    bool byteswap_latched_ = false;
};

// Opens a .chd, .cue, .bin or .iso, chosen by extension. Every track must be
// raw 2352-byte sectors: 2048-byte ISO9660 images and stripped CHD tracks
// are rejected. Returns nullptr if unsupported or unreadable.
std::unique_ptr<Disc> open_disc(std::unique_ptr<DiscContext> disc_ctx);

} // namespace zcdi
