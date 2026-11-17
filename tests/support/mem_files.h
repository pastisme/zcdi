#pragma once
// Files in memory, so no test needs a real file: name -> contents.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "disc/disc.h"
#include "disc/sector.h"

namespace zcdi_test {

using MemFiles = std::map<std::string, std::vector<uint8_t>>;

template <std::size_t N>
std::vector<uint8_t> bytes_of(const uint8_t (&bytes)[N]) {
    return {bytes, bytes + N};
}

// `frames` raw sectors, the first starting with a sync pattern: a bare .bin image.
inline std::vector<uint8_t> raw_image(int frames) {
    static constexpr uint8_t kSync[zcdi::kSyncSize] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                       0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::vector<uint8_t> raw(std::size_t(frames) * zcdi::kRawSectorSize, 0);
    std::memcpy(raw.data(), kSync, zcdi::kSyncSize);
    return raw;
}

// Opens `path` from a copy of `files`, taken now.
inline std::unique_ptr<zcdi::DiscContext> memory_context(const std::string& path, MemFiles files) {
    struct Open {
        const std::vector<uint8_t>* data;
        std::size_t pos = 0;
    };
    auto shared = std::make_shared<const MemFiles>(std::move(files));
    auto ctx = std::make_unique<zcdi::DiscContext>(path);
    ctx->open_read = [shared](const std::string& p) -> zcdi::DiscContext::Handle {
        auto it = shared->find(p);
        return it == shared->end() ? nullptr : new Open{&it->second};
    };
    ctx->read = [](zcdi::DiscContext::Handle h, void* dst, std::size_t len) -> std::size_t {
        auto* f = static_cast<Open*>(h);
        const std::size_t n = len < f->data->size() - f->pos ? len : f->data->size() - f->pos;
        std::memcpy(dst, f->data->data() + f->pos, n);
        f->pos += n;
        return n;
    };
    ctx->seek = [](zcdi::DiscContext::Handle h, int64_t offset, int whence) -> int {
        auto* f = static_cast<Open*>(h);
        const int64_t size = int64_t(f->data->size());
        const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(f->pos) : size;
        if (base + offset < 0 || base + offset > size) return -1;
        f->pos = std::size_t(base + offset);
        return 0;
    };
    ctx->size = [](zcdi::DiscContext::Handle h) -> uint64_t { return static_cast<Open*>(h)->data->size(); };
    ctx->close = [](zcdi::DiscContext::Handle h) -> int {
        delete static_cast<Open*>(h);
        return 0;
    };
    return ctx;
}

}  // namespace zcdi_test
