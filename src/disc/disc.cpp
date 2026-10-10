#include "disc/disc.h"

#include <cstdio>
#include <cstring>

#include "disc/chd_disc.h"
#include "disc/file_io.h"
#include "disc/raw2352_disc.h"

namespace zcdi {

void Disc::finish_sector(int lba, RawSector& out) {
    // Latched on the first swapped sync; CD-DA sectors are swapped too.
    if (!byteswap_latched_ && sector_looks_byteswapped(out.data)) byteswap_latched_ = true;
    if (byteswap_latched_) byteswap_sector(out.data, kRawSectorSize);

    // Descrambled only if that makes it valid, as same_cdi/MAME.
    if (!sector_looks_valid(out.data, lba)) {
        uint8_t descrambled[kRawSectorSize]{};
        std::memcpy(descrambled, out.data, kRawSectorSize);
        descramble_sector(descrambled);
        if (sector_looks_valid(descrambled, lba)) std::memcpy(out.data, descrambled, kRawSectorSize);
    }

    out.sub.file = out.data[kSyncSize + kHeaderSize + 0];
    out.sub.channel = out.data[kSyncSize + kHeaderSize + 1];
    out.sub.submode = out.data[kSyncSize + kHeaderSize + 2];
    out.sub.coding = out.data[kSyncSize + kHeaderSize + 3];
}

DiscContext::DiscContext(std::string path) : path_(std::move(path)) {
    open_read = [](const std::string& file_path) -> Handle { return std::fopen(file_path.c_str(), "rb"); };
    read = [](Handle handle, void* ptr, size_t len) -> size_t {
        if (!handle || !ptr || len == 0) return 0;
        return std::fread(ptr, 1, len, static_cast<FILE*>(handle));
    };
    seek = [](Handle handle, int64_t offset, int whence) -> int {
        if (!handle) return -1;
#if defined(_WIN32)
        return _fseeki64(static_cast<FILE*>(handle), offset, whence);
#else
        return fseeko(static_cast<FILE*>(handle), static_cast<off_t>(offset), whence);
#endif
    };
    size = [](Handle handle) -> uint64_t {
        if (!handle) return static_cast<uint64_t>(-1);
        FILE* file = static_cast<FILE*>(handle);
#if defined(_WIN32)
        __int64 pos = _ftelli64(file);
        if (pos < 0) return static_cast<uint64_t>(-1);
        if (_fseeki64(file, 0, SEEK_END) != 0) return static_cast<uint64_t>(-1);
        __int64 end = _ftelli64(file);
        if (end < 0) return static_cast<uint64_t>(-1);
        if (_fseeki64(file, pos, SEEK_SET) != 0) return static_cast<uint64_t>(-1);
        return static_cast<uint64_t>(end);
#else
        off_t pos = ftello(file);
        if (pos < 0) return static_cast<uint64_t>(-1);
        if (fseeko(file, 0, SEEK_END) != 0) return static_cast<uint64_t>(-1);
        off_t end = ftello(file);
        if (end < 0) return static_cast<uint64_t>(-1);
        if (fseeko(file, pos, SEEK_SET) != 0) return static_cast<uint64_t>(-1);
        return static_cast<uint64_t>(end);
#endif
    };
    close = [](Handle handle) -> int {
        if (!handle) return -1;
        return std::fclose(static_cast<FILE*>(handle));
    };
}

std::unique_ptr<Disc> open_disc(std::unique_ptr<DiscContext> disc_ctx) {
    if (!disc_ctx) return nullptr;
    const std::string e = disc_path_ext(disc_ctx->path());
    if (e == "chd") return open_chd_disc(std::move(disc_ctx));
    // .iso is a bare .bin: a 2048-byte ISO9660 image fails the size check.
    if (e == "cue" || e == "bin" || e == "iso") return open_raw2352_disc(std::move(disc_ctx));
    return nullptr; // everything else: unsupported by design
}

} // namespace zcdi
