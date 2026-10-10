#pragma once

#include "widgets/widget.h"

#include <cstdint>

namespace zcdi {
class MachineInspector;
}

class DisassemblyWidget : public DebuggerWidget {
public:
    DisassemblyWidget() : DebuggerWidget("Disassembly") {}
    void draw(AppState& state) override;

private:
    // The instruction `lines` before `addr`, found by decoding forward from up to 64 bytes back.
    static uint32_t back(const zcdi::MachineInspector& view, uint32_t addr, int lines);

    uint32_t anchor_ = 0; // first line shown
    uint32_t cursor_ = 0; // selected line: F9, run to
    bool follow_pc_ = true;
    char goto_text_[16]{};
};
