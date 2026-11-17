#pragma once
// A Machine's Board, for tests that poke registers or check board state the
// public API doesn't show.
#include <optional>

#include "core/board.h"
#include "core/machine.h"
#include "core/pointing_device.h"
#include "dvc/dvc.h"
#include "support/access_private.h"

ACCESS_PRIVATE_FIELD(zcdi::Machine, zcdi::Board, board_)
ACCESS_PRIVATE_FIELD(zcdi::Board, std::optional<zcdi::PointingDevice>, front_device_)
ACCESS_PRIVATE_FIELD(zcdi::Board, std::optional<zcdi::PointingDevice>, back_device_)
ACCESS_PRIVATE_FIELD(zcdi::PointingDevice, zcdi::PointingDevice::Input, input_)
ACCESS_PRIVATE_FIELD(zcdi::Board, zcdi::dvc::Dvc, dvc_)

namespace zcdi_test {

inline zcdi::Board& board(zcdi::Machine& m) { return access_private::board_(m); }

// The last input set on the device in `socket`; null when the socket is empty.
inline const zcdi::PointingDevice::Input* socket_input(zcdi::Machine& m, zcdi::Socket socket) {
    std::optional<zcdi::PointingDevice>& device = socket == zcdi::Socket::kFront
                                                      ? access_private::front_device_(board(m))
                                                      : access_private::back_device_(board(m));
    return device ? &access_private::input_(*device) : nullptr;
}

}  // namespace zcdi_test
