#include "app.h"

#include "widgets/widget.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

void DebuggerApp::remember_disc(const std::string& path) {
    std::erase(recent_discs_, path);
    recent_discs_.insert(recent_discs_.begin(), path);
    if (recent_discs_.size() > kMaxRecentDiscs) recent_discs_.resize(kMaxRecentDiscs);
    ImGui::MarkIniSettingsDirty();
}

// Entry kinds of the ZcdiDbg section.
constexpr std::uintptr_t kIniWindows = 1;
constexpr std::uintptr_t kIniBreakpoints = 2;
constexpr std::uintptr_t kIniRecent = 3;
constexpr std::uintptr_t kIniOptions = 4;

void* DebuggerApp::settings_open(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    if (std::strcmp(name, "Windows") == 0) return reinterpret_cast<void*>(kIniWindows);
    if (std::strcmp(name, "Breakpoints") == 0) return reinterpret_cast<void*>(kIniBreakpoints);
    if (std::strcmp(name, "Recent") == 0) return reinterpret_cast<void*>(kIniRecent);
    if (std::strcmp(name, "Options") == 0) return reinterpret_cast<void*>(kIniOptions);
    return nullptr;
}

// Windows: "Title=0|1". Breakpoints: "hexaddress=0|1" (enabled). Recent: "bios=path", "disc=path".
// Options: "auto_play=0|1", "tv_aspect=0|1", "front_mouse=0|1".
void DebuggerApp::settings_read_line(ImGuiContext*, ImGuiSettingsHandler* handler, void* entry, const char* line) {
    auto* app = static_cast<DebuggerApp*>(handler->UserData);
    if (app && reinterpret_cast<std::uintptr_t>(entry) == kIniRecent) {
        // The first '=': a path may hold more.
        if (std::strncmp(line, "bios=", 5) == 0) app->recent_bios_ = line + 5;
        if (std::strncmp(line, "disc=", 5) == 0 && app->recent_discs_.size() < kMaxRecentDiscs) {
            app->recent_discs_.emplace_back(line + 5);
        }
        return;
    }
    if (app && reinterpret_cast<std::uintptr_t>(entry) == kIniOptions) {
        if (std::strcmp(line, "auto_play=1") == 0) app->state_.auto_play = true;
        if (std::strcmp(line, "tv_aspect=1") == 0) app->state_.tv_aspect = true;
        if (std::strcmp(line, "front_mouse=1") == 0) app->state_.front_mouse = true;
        return;
    }
    const char* eq = std::strrchr(line, '=');
    if (!app || !eq) return;
    const std::string name(line, static_cast<std::size_t>(eq - line));
    if (reinterpret_cast<std::uintptr_t>(entry) == kIniBreakpoints) {
        if (!app->state_.runner) return;
        EmuRunner& runner = *app->state_.runner;
        if (name.rfind("os9:", 0) == 0) {
            runner.os9_breaks.insert(static_cast<uint16_t>(std::strtoul(name.c_str() + 4, nullptr, 16)));
        } else if (name.rfind("vec:", 0) == 0) {
            runner.vector_breaks.insert(static_cast<uint8_t>(std::strtoul(name.c_str() + 4, nullptr, 10)));
        } else {
            char* end = nullptr;
            const unsigned long addr = std::strtoul(name.c_str(), &end, 16);
            if (end && *end == '\0') runner.breakpoints[static_cast<uint32_t>(addr)] = eq[1] != '0';
        }
        return;
    }
    for (std::size_t i = 0; i < app->widgets_.size(); ++i) {
        if (name == app->widgets_[i]->title()) {
            app->widgets_[i]->open = eq[1] != '0';
            app->widgets_were_open_[i] = app->widgets_[i]->open;
        }
    }
}

void DebuggerApp::settings_write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out) {
    auto* app = static_cast<DebuggerApp*>(handler->UserData);
    if (!app) return;
    out->appendf("[%s][Windows]\n", handler->TypeName);
    for (const auto& widget : app->widgets_)
        out->appendf("%s=%d\n", widget->title(), widget->open ? 1 : 0);
    out->append("\n");
    out->appendf("[%s][Options]\nauto_play=%d\ntv_aspect=%d\nfront_mouse=%d\n\n", handler->TypeName,
                 app->state_.auto_play ? 1 : 0, app->state_.tv_aspect ? 1 : 0, app->state_.front_mouse ? 1 : 0);
    if (!app->recent_bios_.empty() || !app->recent_discs_.empty()) {
        out->appendf("[%s][Recent]\n", handler->TypeName);
        if (!app->recent_bios_.empty()) out->appendf("bios=%s\n", app->recent_bios_.c_str());
        for (const std::string& disc : app->recent_discs_)
            out->appendf("disc=%s\n", disc.c_str());
        out->append("\n");
    }
    const EmuRunner* runner = app->state_.runner.get();
    if (!runner || (runner->breakpoints.empty() && runner->os9_breaks.empty() && runner->vector_breaks.empty())) return;
    out->appendf("[%s][Breakpoints]\n", handler->TypeName);
    for (const auto& [addr, enabled] : runner->breakpoints) {
        out->appendf("%06X=%d\n", static_cast<unsigned>(addr), enabled ? 1 : 0);
    }
    for (const uint16_t code : runner->os9_breaks)
        out->appendf("os9:%02X=1\n", static_cast<unsigned>(code));
    for (const uint8_t vector : runner->vector_breaks)
        out->appendf("vec:%u=1\n", static_cast<unsigned>(vector));
    out->append("\n");
}
