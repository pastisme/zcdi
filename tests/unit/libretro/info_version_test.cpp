#include "doctest.h"

#include <fstream>
#include <string>
#include <string_view>

#include "core/version.h"

TEST_CASE("the .info display_version matches the library version") {
    std::ifstream info(ZCDI_LIBRETRO_INFO_PATH);
    REQUIRE(info);

    const std::string key = "display_version = \"";
    std::string display_version;
    for (std::string line; std::getline(info, line);) {
        if (line.starts_with(key) && line.ends_with('"')) {
            display_version = line.substr(key.size(), line.size() - key.size() - 1);
            break;
        }
    }

    // Build metadata (+commit) is not part of the .info version.
    const std::string_view sem_ver = zcdi::version.sem_ver;
    CHECK(display_version == sem_ver.substr(0, sem_ver.find('+')));
}
