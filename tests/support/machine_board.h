#pragma once
// A Machine's Board, for tests that poke registers or check board state the
// public API doesn't show.
#include "core/board.h"
#include "core/machine.h"
#include "support/private_access.h"

ZCDI_PRIVATE_MEMBER(MachineBoard, zcdi::Machine, board_, zcdi::Board);

namespace zcdi_test {

inline zcdi::Board& board(zcdi::Machine& m) { return priv<MachineBoard>(m); }

}  // namespace zcdi_test
