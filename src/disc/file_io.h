#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "disc/disc.h"

namespace zcdi {

// An open file, closed (through its context) when it goes out of scope.
struct DiscFile {
    DiscContext::Handle handle = nullptr;
    const DiscContext* ctx = nullptr;

    DiscFile() = default;
    DiscFile(const DiscFile&) = delete;
    DiscFile& operator=(const DiscFile&) = delete;
    ~DiscFile();
};

bool disc_file_open_read(const std::string& path, DiscFile& out, const DiscContext& disc_ctx);
uint64_t disc_file_size(DiscFile* file);
size_t disc_file_read(DiscFile* file, void* ptr, size_t len);
int disc_file_seek(DiscFile* file, int64_t offset, int whence);
int disc_file_close(DiscFile* file);

bool disc_read_whole_file(const std::string& path, std::string& out, const DiscContext& disc_ctx);

// `path` with '\\' read as '/', as cue sheets from Windows use it.
std::filesystem::path disc_generic_path(std::string path);
// Lower-case extension without the dot, e.g. "cue"; empty if none.
std::string disc_path_ext(const std::string& path);

} // namespace zcdi
