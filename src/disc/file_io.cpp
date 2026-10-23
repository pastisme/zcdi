#include "disc/file_io.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace zcdi {
DiscFile::~DiscFile() { disc_file_close(this); }

bool disc_file_open_read(const std::string& path, DiscFile& out, const DiscContext& disc_ctx) {
    disc_file_close(&out);

    if (!disc_ctx.open_read || !disc_ctx.read || !disc_ctx.seek || !disc_ctx.size || !disc_ctx.close) return false;
    DiscContext::Handle handle = disc_ctx.open_read(path);
    if (!handle) return false;
    out.handle = handle;
    out.ctx = &disc_ctx;
    return true;
}

uint64_t disc_file_size(DiscFile* file) {
    if (!file || !file->ctx || !file->handle) return static_cast<uint64_t>(-1);
    return file->ctx->size(file->handle);
}

size_t disc_file_read(DiscFile* file, void* ptr, size_t len) {
    if (!file || !file->ctx || !file->handle || !ptr || len == 0) return 0;
    return file->ctx->read(file->handle, ptr, len);
}

int disc_file_seek(DiscFile* file, int64_t offset, int whence) {
    if (!file || !file->ctx || !file->handle) return -1;
    return file->ctx->seek(file->handle, offset, whence);
}

int disc_file_close(DiscFile* file) {
    if (!file) return -1;

    int rc = 0;
    if (file->ctx && file->handle) rc = file->ctx->close(file->handle);
    file->handle = nullptr;
    file->ctx = nullptr;
    return rc;
}

bool disc_read_whole_file(const std::string& path, std::string& out, const DiscContext& disc_ctx) {
    DiscFile file;
    if (!disc_file_open_read(path, file, disc_ctx)) return false;

    uint64_t size64 = disc_file_size(&file);
    if (size64 == static_cast<uint64_t>(-1) || size64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }

    out.assign(static_cast<size_t>(size64), '\0');
    if (!out.empty() && disc_file_read(&file, out.data(), out.size()) != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

std::filesystem::path disc_generic_path(std::string path) {
    std::ranges::replace(path, '\\', '/');
    return std::filesystem::path(path);
}

namespace {
// "C:\..." or "\\server\..." also counts, whatever the host OS.
bool is_absolute(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':';
}
} // namespace

std::string disc_path_beside(const std::string& list_path, const std::string& entry) {
    if (is_absolute(entry)) return entry;
    return (disc_generic_path(list_path).parent_path() / disc_generic_path(entry)).lexically_normal().string();
}

std::string disc_path_ext(const std::string& path) {
    std::string e = disc_generic_path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::ranges::transform(e, e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

} // namespace zcdi
