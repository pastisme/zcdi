#pragma once

#include "widgets/widget.h"

#include "imgui_club/imgui_memory_editor.h"

class MemoryWidget : public DebuggerWidget {
public:
    MemoryWidget();
    void draw(AppState& state) override;

private:
    MemoryEditor memory_editor_{};
};
