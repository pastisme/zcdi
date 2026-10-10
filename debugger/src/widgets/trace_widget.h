#pragma once

#include "widgets/widget.h"

// The last instructions and exception entries the runner recorded.
class TraceWidget : public DebuggerWidget {
public:
    TraceWidget() : DebuggerWidget("Trace") {}
    void draw(AppState& state) override;

private:
    void draw_instructions(AppState& state);
    void draw_exceptions(AppState& state);
    void draw_timeline(AppState& state);

    bool hide_traps_ = false;
    int seen_stop_ = -1;
    bool scroll_to_end_ = false;
};
