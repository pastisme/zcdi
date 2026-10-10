#pragma once

#include "widgets/widget.h"

class BreakpointsWidget : public DebuggerWidget {
public:
    BreakpointsWidget() : DebuggerWidget("Breakpoints") {}
    void draw(AppState& state) override;

private:
    char add_text_[16]{};
    int os9_pick_ = 0x52; // F$SysDbg
};
