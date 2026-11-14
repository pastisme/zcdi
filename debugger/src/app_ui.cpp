#include "app.h"

#include "widgets/widget.h"

#include "core/machine_inspector.h"

#include "imgui-filebrowser/imfilebrowser.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_audio.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace {

// The paused-only run commands, in Run menu and toolbar order.
struct StepCommand {
    RunCommand command;
    const char* menu;
    const char* button;
    const char* shortcut;
    ImGuiKey key;
    bool shift;
};

constexpr StepCommand kStepCommands[] = {
    {RunCommand::kStepInto, "Step into", "Into", "F11", ImGuiKey_F11, false},
    {RunCommand::kStepOver, "Step over", "Over", "F10", ImGuiKey_F10, false},
    {RunCommand::kStepOut, "Step out", "Out", "Shift+F11", ImGuiKey_F11, true},
    {RunCommand::kStepLine, "Step line", "Line", "F7", ImGuiKey_F7, false},
    {RunCommand::kStepFrame, "Step frame", "Frame", "F6", ImGuiKey_F6, false},
};

} // namespace

void DebuggerApp::browse(ImGui::FileBrowser& browser, const std::string& current) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(current).parent_path();
    if (!current.empty() && std::filesystem::is_directory(dir, ec)) browser.SetDirectory(dir);
    browser.Open();
}

void DebuggerApp::draw_file_browsers() {
    bios_browser_->Display();
    if (bios_browser_->HasSelected()) {
        const std::string path = bios_browser_->GetSelected().string();
        bios_browser_->ClearSelected();
        open_file(path); // vmpega.rom is the DVC cartridge
    }
    disc_browser_->Display();
    if (disc_browser_->HasSelected()) {
        const std::string path = disc_browser_->GetSelected().string();
        disc_browser_->ClearSelected();
        open_disc(path);
    }
}

void DebuggerApp::handle_hotkeys() {
    if (ImGui::GetIO().WantTextInput) return;
    const bool shift = ImGui::GetIO().KeyShift;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) toggle_running();
    // Later rows win: Shift+F11 is Step out, not Step into.
    for (const StepCommand& step : kStepCommands) {
        if (ImGui::IsKeyPressed(step.key, false) && (!step.shift || shift)) state_.command = step.command;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) quick_save();
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) quick_load();
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (enter && ImGui::GetIO().KeyAlt) set_rendering_maximized(!state_.rendering_maximized);
}

void DebuggerApp::set_rendering_maximized(bool on) {
    state_.rendering_maximized = on;
    if (sapp_is_fullscreen() != on) sapp_toggle_fullscreen();
}

void DebuggerApp::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool have_machine = state_.machine != nullptr;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open disc...")) browse(*disc_browser_, config_.disc_path);
        if (ImGui::BeginMenu("Recent discs", !recent_discs_.empty())) {
            std::string chosen; // opening reorders the list
            for (const std::string& path : recent_discs_) {
                const std::string label = std::filesystem::path(path).filename().string() + "##" + path;
                if (ImGui::MenuItem(label.c_str())) chosen = path;
                ImGui::SetItemTooltip("%s", path.c_str());
            }
            ImGui::EndMenu();
            if (!chosen.empty()) open_disc(chosen);
        }
        if (ImGui::MenuItem("Open BIOS...")) browse(*bios_browser_, config_.bios_path);
        ImGui::Separator();
        const bool whole_frame = have_machine && !state_.runner->mid_frame();
        if (ImGui::MenuItem("Quick save", "F2", false, whole_frame)) quick_save();
        if (ImGui::MenuItem("Quick load", "F4", false, whole_frame && !quick_state_.empty())) quick_load();
        ImGui::Separator();
        const bool tray_open = have_machine && state_.machine->tray_open();
        if (ImGui::MenuItem("Tray open", nullptr, tray_open, have_machine)) state_.machine->set_tray_open(!tray_open);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset", nullptr, false, have_machine)) reset_machine(false);
        if (ImGui::MenuItem("Power cycle", nullptr, false, have_machine)) reset_machine(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) sapp_request_quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Run")) {
        const bool paused = have_machine && !state_.running;
        if (ImGui::MenuItem(state_.running ? "Pause" : "Run", "F5", false, have_machine)) toggle_running();
        ImGui::Separator();
        for (const StepCommand& step : kStepCommands) {
            if (ImGui::MenuItem(step.menu, step.shortcut, false, paused)) state_.command = step.command;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (const auto& widget : widgets_)
            ImGui::MenuItem(widget->title(), nullptr, &widget->open);
        ImGui::Separator();
        if (ImGui::MenuItem("Fullscreen rendering", "Alt+Enter", state_.rendering_maximized)) {
            set_rendering_maximized(!state_.rendering_maximized);
        }
        if (ImGui::MenuItem("Reset layout")) reset_layout_requested_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Options")) {
        // Changing it powers on a new player of that model.
        if (ImGui::MenuItem("NTSC", nullptr, state_.video_standard == zcdi::VideoStandard::kNtsc)) {
            state_.video_standard = zcdi::VideoStandard::kNtsc;
        }
        if (ImGui::MenuItem("PAL", nullptr, state_.video_standard == zcdi::VideoStandard::kPal)) {
            state_.video_standard = zcdi::VideoStandard::kPal;
        }
        ImGui::Separator();
        // Like the libretro core option: read at power-on.
        if (ImGui::MenuItem("Auto Play", nullptr, &state_.auto_play)) ImGui::MarkIniSettingsDirty();
        ImGui::SetItemTooltip("Starts a CD-i disc without the player's menu.\nTakes effect at the next power cycle.");
        if (ImGui::MenuItem("TV aspect ratio", nullptr, &state_.tv_aspect)) ImGui::MarkIniSettingsDirty();
        ImGui::SetItemTooltip(
            "Shows the picture with the shape it has on a TV instead of 4:3,\n"
            "from the player's pixel timing.");
        if (ImGui::MenuItem("Front device: mouse", nullptr, &state_.front_mouse)) ImGui::MarkIniSettingsDirty();
        ImGui::SetItemTooltip(
            "A mouse ('M') instead of a gamepad ('J') in the front socket.\n"
            "Both take the arrows and the captured mouse.\n"
            "Plugging the other one makes the player detect it again.");
        ImGui::MenuItem("Mute audio", nullptr, &state_.muted);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Controls")) help_popup_requested_ = "Controls";
        if (ImGui::MenuItem("License")) help_popup_requested_ = "License";
        ImGui::Separator();
        if (ImGui::MenuItem("About")) help_popup_requested_ = "About zcdi dbg";
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    draw_help_popups();
}

namespace {

struct KeyRow {
    const char* key;
    const char* action;
};

// Rows with a null action are section headings.
constexpr KeyRow kControls[] = {
    {"Emulation", nullptr},
    {"F5", "Run / pause (pause lands at a frame end)"},
    {"F11", "Step into: one instruction"},
    {"F10", "Step over: past a BSR, JSR or OS-9 TRAP call"},
    {"Shift+F11", "Step out: until the current routine returns"},
    {"F7", "Step to the next field line"},
    {"F6", "Run to the end of the frame"},
    {"F2 / F4", "Quick save / quick load (in memory, whole frames only)"},
    {"Alt+Enter", "Fullscreen rendering on / off"},
    {"Esc", "Leave fullscreen (releases a captured mouse first)"},
    {"Files", nullptr},
    {"Drop a file", "A disc image, a BIOS .rom or vmpega.rom (the DVC): the player powers on with it"},
    {"Disassembly", nullptr},
    {"Click gutter, F9", "Toggle a breakpoint on the line"},
    {"Right-click", "Run to here, toggle breakpoint"},
    {"Mouse wheel", "Scroll (stops following PC)"},
    {"CD-i pointer", nullptr},
    {"Arrows", "Move the pointer (speeds up while held)"},
    {"Z or S", "Button 1"},
    {"X", "Button 2"},
    {"A", "Buttons 1 + 2"},
    {"F8", "Capture the mouse (hover Rendering first)"},
    {"F8 or Esc", "Release the mouse"},
    {"Mouse buttons", "Left: button 1, right: button 2, middle: both (captured)"},
    {"Command line", nullptr},
    {"bios=<file>", "BIOS ROM; cdi220b.rom starts PAL, others NTSC"},
    {"disc=<file>", "Disc image (.cue, .chd, ...)"},
};

struct LicenseRow {
    const char* part;
    const char* license;
};

constexpr LicenseRow kLicenses[] = {
    {"zcdi", "GPL-3.0-or-later"},
    {"mamecdi (from MAME)", "BSD-3-Clause"},
    {"Musashi", "MIT"},
    {"libmpeg2", "GPL-2.0-or-later"},
    {"libchdr", "BSD-3-Clause"},
    {"minimp3", "CC0-1.0"},
    {"playcdi", "LGPL-2.0-or-later"},
    {"libretro-common", "MIT"},
    {"Dear ImGui", "MIT"},
    {"sokol", "zlib"},
    {"imgui_club (memory editor)", "MIT"},
    {"imgui-filebrowser", "MIT"},
};

bool begin_help_popup(const char* name) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
}

void end_help_popup() {
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void DebuggerApp::draw_help_popups() {
    if (help_popup_requested_) {
        ImGui::OpenPopup(help_popup_requested_);
        help_popup_requested_ = nullptr;
    }

    if (begin_help_popup("Controls")) {
        const ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("controls", 2, flags)) {
            ImGui::TableSetupColumn("Key");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (const KeyRow& row : kControls) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (!row.action) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram), "%s", row.key);
                    continue;
                }
                ImGui::TextUnformatted(row.key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.action);
            }
            ImGui::EndTable();
        }
        end_help_popup();
    }

    if (begin_help_popup("License")) {
        ImGui::TextUnformatted("zcdi dbg is free software under the GNU General Public License,");
        ImGui::TextUnformatted("version 3 or (at your option) any later version. It comes with");
        ImGui::TextUnformatted("ABSOLUTELY NO WARRANTY. The full text is the LICENSE file at the");
        ImGui::TextUnformatted("root of the source tree.");
        ImGui::Spacing();
        ImGui::TextUnformatted("It includes:");
        if (ImGui::BeginTable("licenses", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const LicenseRow& row : kLicenses) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.part);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", row.license);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Each licence text is in its directory under third_party/ and mamecdi/.");
        end_help_popup();
    }

    if (begin_help_popup("About zcdi dbg")) {
        ImGui::Text("zcdi dbg %d.%d.%d%s", ZCDI_VERSION_MAJOR, ZCDI_VERSION_MINOR, ZCDI_VERSION_PATCH,
                    ZCDI_VERSION_PRERELEASE);
        ImGui::Separator();
        ImGui::TextUnformatted("Debugger for zcdi, a Philips CD-i (Mono-I) emulator with DVC support.");
        ImGui::TextUnformatted("Built on the work of MAME, CDi_MiSTer and CD-i Fan.");
        ImGui::Spacing();
        ImGui::TextDisabled("GPL-3.0-or-later, no warranty (Help > License).");
        ImGui::TextDisabled("Dear ImGui %s, sokol", IMGUI_VERSION);
        end_help_popup();
    }
}

void DebuggerApp::draw_toolbar() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    const float height = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
    if (ImGui::BeginViewportSideBar("##toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        const bool have_machine = state_.machine != nullptr;
        ImGui::BeginDisabled(!have_machine);
        if (ImGui::Button(state_.running ? "Pause" : " Run ")) toggle_running();
        ImGui::BeginDisabled(state_.running);
        for (const StepCommand& step : kStepCommands) {
            ImGui::SameLine();
            if (ImGui::Button(step.button)) state_.command = step.command;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        const bool mid_frame = state_.runner->mid_frame();
        ImGui::BeginDisabled(mid_frame);
        if (ImGui::Button("Save")) quick_save();
        ImGui::SameLine();
        ImGui::BeginDisabled(quick_state_.empty());
        if (ImGui::Button("Load")) quick_load();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) reset_machine(false);
        ImGui::SameLine();
        const bool tray_open = have_machine && state_.machine->tray_open();
        if (ImGui::Button(tray_open ? "Close tray" : "Open tray")) state_.machine->set_tray_open(!tray_open);
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void DebuggerApp::draw_status_bar() {
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
                                    flags)) {
        if (ImGui::BeginMenuBar()) {
            if (state_.machine) {
                const zcdi::MachineInspector view(*state_.machine);
                ImGui::TextUnformatted(state_.running ? "Running" : "Paused");
                ImGui::Separator();
                if (state_.runner->mid_frame()) {
                    ImGui::Text("Frame %d line %d", view.frame(), view.line());
                } else {
                    ImGui::Text("Frame %d", view.frame());
                }
                ImGui::Separator();
                ImGui::Text("%s %.0f Hz", state_.built_video_standard == zcdi::VideoStandard::kPal ? "PAL" : "NTSC",
                            state_.machine->refresh_rate());
                ImGui::Separator();
                ImGui::TextUnformatted(state_.bios_name.c_str());
                ImGui::Separator();
                ImGui::TextUnformatted(state_.machine->tray_open() ? "Tray open" : "Tray closed");
                ImGui::Separator();
                ImGui::TextUnformatted(state_.dvc_rom_loaded ? "DVC" : "No DVC");
                ImGui::Separator();
            }
            if (!(state_.audio_backend_ok && saudio_isvalid())) {
                ImGui::TextUnformatted("No audio, timer paced");
                ImGui::Separator();
            }
            if (state_.mouse_captured_for_game) {
                ImGui::TextUnformatted("Mouse captured (F8/Esc)");
                ImGui::Separator();
            }
            if (state_.machine && !state_.runner->hook_seen() && zcdi::MachineInspector(*state_.machine).frame() > 1) {
                ImGui::TextUnformatted("No CPU hook in this core: breakpoints off");
                ImGui::Separator();
            }
            if (state_.machine) {
                ImGui::Text("%.0f FPS", emu_fps_);
                ImGui::Separator();
            }
            ImGui::TextUnformatted(state_.stop_text.empty() ? state_.status.c_str() : state_.stop_text.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// Rendering in the middle, CPU state on the right, memory underneath.
void DebuggerApp::draw_dockspace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace_id = ImGui::GetID(kDockspaceName);
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
    if (reset_layout_requested_ || !node || (!node->IsSplitNode() && node->Windows.Size == 0)) {
        reset_layout_requested_ = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);
        ImGuiID center = dockspace_id;
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
        ImGuiID right_bottom = right;
        const ImGuiID right_top = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.30f, nullptr, &right_bottom);
        ImGui::DockBuilderDockWindow("Rendering", center);
        ImGui::DockBuilderDockWindow("CPU", right_top);
        ImGui::DockBuilderDockWindow("Disassembly", right_bottom);
        ImGui::DockBuilderDockWindow("Stack", right_bottom);
        ImGui::DockBuilderDockWindow("Breakpoints", right_bottom);
        ImGui::DockBuilderDockWindow("Memory", bottom);
        ImGui::DockBuilderDockWindow("MCD212", bottom);
        ImGui::DockBuilderDockWindow("Devices", bottom);
        ImGui::DockBuilderDockWindow("Trace", bottom);
        ImGui::DockBuilderFinish(dockspace_id);
        for (const auto& widget : widgets_)
            widget->open = true;
    }
    ImGui::DockSpaceOverViewport(dockspace_id, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
}
