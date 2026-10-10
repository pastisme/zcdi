#pragma once
#include "disc/disc.h"

namespace zcdi {
std::unique_ptr<Disc> open_chd_disc(std::unique_ptr<DiscContext> disc_ctx);
} // namespace zcdi
