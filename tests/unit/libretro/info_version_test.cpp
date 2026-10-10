#include "doctest.h"

#include <fstream>
#include <string>

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

    CHECK(display_version == zcdi::version.sem_ver);
}
