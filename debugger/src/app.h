#pragma once

#include "app_state.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct sapp_event;
struct ImGuiContext;
struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

class DebuggerWidget;
namespace ImGui {
class FileBrowser;
}

class DebuggerApp {
public:
    static constexpr const char* kBaseWindowTitle = "zcdi dbg";

    DebuggerApp();
    ~DebuggerApp();

    void set_launch_config(std::string bios_path, std::string disc_path);
    void init();
    void frame();
    void cleanup();
    void event(const sapp_event* ev);

private:
    // imgui.ini section holding which windows are open.
    static void* settings_open(ImGuiContext*, ImGuiSettingsHandler*, const char* name);
    static void settings_read_line(ImGuiContext*, ImGuiSettingsHandler* handler, void*, const char* line);
    static void settings_write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out);

    void draw_menu_bar();
    void draw_help_popups();
    void draw_toolbar();
    void draw_status_bar();
    void draw_dockspace();
    void handle_hotkeys();
    // Opens `browser` in the folder of `current`, if any.
    void browse(ImGui::FileBrowser& browser, const std::string& current);
    void draw_file_browsers();
    // vmpega.rom is the DVC ROM, another .rom a BIOS, anything else a disc.
    void open_file(const std::string& path);
    void open_bios(const std::string& path);
    void open_dvc_rom(const std::string& path);
    // The chosen one, else vmpega.rom beside the BIOS.
    std::filesystem::path dvc_rom_path() const;
    void open_disc(const std::string& path);
    void remember_disc(const std::string& path);
    // Alt+Enter: Rendering alone, fullscreen.
    void set_rendering_maximized(bool on);
    void toggle_running();
    void execute_command();
    // Runs one frame; false if a breakpoint or step stopped it.
    bool emulate_frame();
    void update_emu_fps();
    void on_stop(EmuRunner::Stop why);
    void copy_framebuffer();
    void load_symbols();
    void reset_machine(bool cold);
    void quick_save();
    void quick_load();
    void upload_frame();
    void compact_audio_pending();
    void pump_audio_pending();
    void queue_audio_frame();
    void step_emulation_frame();
    void setup_frame_texture(int width, int height);
    void start_machine();

    LaunchConfig config_{};
    AppState state_{};
    std::vector<std::unique_ptr<DebuggerWidget>> widgets_;
    // The one widget shown while maximized; owned by widgets_.
    DebuggerWidget* rendering_ = nullptr;
    std::vector<bool> widgets_were_open_;
    std::string ini_path_;
    std::vector<uint8_t> quick_state_;
    std::map<uint32_t, bool> saved_breakpoints_;
    std::set<uint16_t> saved_os9_breaks_;
    std::set<uint8_t> saved_vector_breaks_;
    bool reset_layout_requested_ = false;
    std::unique_ptr<ImGui::FileBrowser> bios_browser_;
    std::unique_ptr<ImGui::FileBrowser> disc_browser_;
    // Kept in imgui.ini; the BIOS is used when bios= is not given.
    std::string recent_bios_;
    std::vector<std::string> recent_discs_; // most recent first
    double emu_fps_ = 0.0;
    int emu_frames_counted_ = 0;
    std::chrono::steady_clock::time_point emu_fps_since_{};
    // Dropped on the window, opened at the next frame.
    std::string dropped_path_;
    // Help menu items open these; OpenPopup must run outside the menu.
    const char* help_popup_requested_ = nullptr;
};
