#pragma once

#include "widgets/widget.h"

class StackWidget : public DebuggerWidget {
public:
    StackWidget() : DebuggerWidget("Stack") {}
    void draw(AppState& state) override;
};
