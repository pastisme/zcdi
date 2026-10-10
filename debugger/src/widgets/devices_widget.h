#pragma once

#include "widgets/widget.h"

// CDIC, VMPEG/DVC, SCC68070 and SLAVE state, read-only.
class DevicesWidget : public DebuggerWidget {
public:
    DevicesWidget() : DebuggerWidget("Devices") {}
    void draw(AppState& state) override;
};
