#pragma once

#include "app_state.h"

class DebuggerWidget {
public:
    explicit DebuggerWidget(const char* title) : title_(title) {}
    virtual ~DebuggerWidget() = default;
    virtual void draw(AppState& state) = 0;
    // Before sokol shuts down: free GPU resources the widget made.
    virtual void release() {}

    // The window name; also its key in the View menu and imgui.ini.
    const char* title() const { return title_; }
    // Cleared by the window's close button, set again from the View menu.
    bool open = true;

protected:
    // Begins the window; false, with it ended, when there is no machine.
    bool begin_machine_window(const AppState& state) {
        ImGui::Begin(title(), &open);
        if (state.machine) return true;
        ImGui::TextUnformatted("No machine");
        ImGui::End();
        return false;
    }

private:
    const char* title_;
};
