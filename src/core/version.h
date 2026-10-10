#pragma once

#include <string>
#include <string_view>

#ifndef ZCDI_VERSION_MAJOR
#error ZCDI_VERSION_MAJOR must be defined
#endif

#ifndef ZCDI_VERSION_MINOR
#error ZCDI_VERSION_MINOR must be defined
#endif

#ifndef ZCDI_VERSION_PATCH
#error ZCDI_VERSION_PATCH must be defined
#endif

static_assert(ZCDI_VERSION_MAJOR >= 0);
static_assert(ZCDI_VERSION_MINOR >= 0);
static_assert(ZCDI_VERSION_PATCH >= 0);

namespace zcdi {
inline const struct {
    int major = ZCDI_VERSION_MAJOR;
    int minor = ZCDI_VERSION_MINOR;
    int patch = ZCDI_VERSION_PATCH;

    constexpr std::string_view prerelease() const {
#ifdef ZCDI_VERSION_PRERELEASE
        return ZCDI_VERSION_PRERELEASE;
#else
        return {};
#endif
    }

    constexpr std::string_view metadata() const {
#ifdef ZCDI_VERSION_METADATA
        return ZCDI_VERSION_METADATA;
#else
        return {};
#endif
    }

    const std::string sem_ver = [this] {
        std::string result = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);

        if (!prerelease().empty()) result += "-" + std::string(prerelease());

        if (!metadata().empty()) result += "+" + std::string(metadata());

        return result;
    }();
} version;
} // namespace zcdi
