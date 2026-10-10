#pragma once

#include "widgets/widget.h"

#include <cstdint>
#include <vector>

// The video chip: registers, the ICA and per-line DCA lists in plane RAM,
// and the CLUT. Read-only.
class Mcd212Widget : public DebuggerWidget {
public:
    Mcd212Widget() : DebuggerWidget("MCD212") {}
    void draw(AppState& state) override;
    void release() override;

private:
    void draw_planes(AppState& state);

    bool hide_idle_lines_ = true;

    // Planes tab: one plane decoded on its own into a texture.
    int plane_ = 0;
    bool start_from_ica_ = true;
    uint32_t start_ = 0;
    std::vector<uint32_t> pixels_;
    std::vector<uint32_t> rgba_;
    StreamTexture texture_;
};
