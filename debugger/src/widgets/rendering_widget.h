#pragma once

#include "widgets/widget.h"

class RenderingWidget : public DebuggerWidget {
public:
    RenderingWidget() : DebuggerWidget("Rendering") {}
    void draw(AppState& state) override;
};
