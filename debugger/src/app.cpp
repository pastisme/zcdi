#include "app.h"

#include "core/serial_pointer.h"

#include "widgets/breakpoints_widget.h"
#include "widgets/cpu_widget.h"
#include "widgets/devices_widget.h"
#include "widgets/disassembly_widget.h"
#include "widgets/mcd212_widget.h"
#include "widgets/memory_widget.h"
#include "widgets/rendering_widget.h"
#include "widgets/stack_widget.h"
#include "widgets/trace_widget.h"
#include "widgets/widget.h"

#include "names.h"

#include "core/machine_inspector.h"
#include "core/state_archive.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui-filebrowser/imfilebrowser.h"

#include "sokol/sokol_app.h"
#include "sokol/sokol_audio.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_imgui.h"
#include "sokol/sokol_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <span>
#include <system_error>
#include <utility>

namespace {

constexpr int kAudioSampleRate = 44100;
constexpr int kAudioChannels = 2;
constexpr int kMaxEmuFramesPerRender = 8;

enum class SlogLevel : uint32_t {
    Panic = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
};

void log(SlogLevel level, const std::string& message, std::source_location at = std::source_location::current()) {
    slog_func("zcdi_dbg", static_cast<uint32_t>(level), 0, message.c_str(), at.line(), at.file_name(), nullptr);
}

std::vector<std::byte> read_binary_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    file.seekg(0, std::ios::end);
    std::streamsize size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> out(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) return {};
    return out;
}

constexpr const char* kDockspaceName = "zcdi_dockspace";

// The PAL BIOS of the libretro core (libretro_core.cpp kRegionProfiles).
constexpr const char* kPalBios = "cdi220b.rom";

// The BIOS picks the player model, as in the libretro core.
zcdi::VideoStandard video_standard_for(const std::string& bios_path) {
    return std::filesystem::path(bios_path).filename() == kPalBios ? zcdi::VideoStandard::kPal
                                                                   : zcdi::VideoStandard::kNtsc;
}

constexpr std::size_t kMaxRecentDiscs = 8;

// The DVC cartridge ROM, looked for beside the BIOS (as the libretro core does).
constexpr const char* kDvcRom = "vmpega.rom";

bool is_dvc_rom(const std::string& path) {
    std::string name = std::filesystem::path(path).filename().string();
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name == kDvcRom;
}

// Lower-case, with the dot: ".cue".
std::string lower_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

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

// Per-user config dir, so the layout follows the user, not the launch dir.
std::string ini_file_path() {
    std::filesystem::path dir;
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) dir = std::filesystem::path(appdata) / "zcdi";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        dir = std::filesystem::path(xdg) / "zcdi";
    } else if (const char* home = std::getenv("HOME")) {
        dir = std::filesystem::path(home) / ".config" / "zcdi";
    }
#endif
    if (dir.empty()) return "zcdi_dbg.ini";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / "zcdi_dbg.ini").string();
}

} // namespace

DebuggerApp::DebuggerApp() {
    widgets_.push_back(std::make_unique<CpuWidget>());
    widgets_.push_back(std::make_unique<DisassemblyWidget>());
    widgets_.push_back(std::make_unique<StackWidget>());
    widgets_.push_back(std::make_unique<BreakpointsWidget>());
    widgets_.push_back(std::make_unique<MemoryWidget>());
    widgets_.push_back(std::make_unique<Mcd212Widget>());
    widgets_.push_back(std::make_unique<DevicesWidget>());
    widgets_.push_back(std::make_unique<TraceWidget>());
    widgets_.push_back(std::make_unique<RenderingWidget>());
    rendering_ = widgets_.back().get();
    widgets_were_open_.assign(widgets_.size(), true);
}

DebuggerApp::~DebuggerApp() = default;

void DebuggerApp::set_launch_config(std::string bios_path, std::string disc_path) {
    config_.bios_path = std::move(bios_path);
    state_.video_standard = video_standard_for(config_.bios_path);
    config_.disc_path = std::move(disc_path);
    log(SlogLevel::Info, "Parsed args bios='" + config_.bios_path + "' disc='" + config_.disc_path + "'");
}

void DebuggerApp::compact_audio_pending() {
    if (state_.audio_pending_head == 0) return;
    if (state_.audio_pending_head >= state_.audio_pending.size()) {
        state_.audio_pending.clear();
        state_.audio_pending_head = 0;
        return;
    }
    if (state_.audio_pending_head >= 4096) {
        state_.audio_pending.erase(
            state_.audio_pending.begin(),
            state_.audio_pending.begin() + static_cast<std::ptrdiff_t>(state_.audio_pending_head));
        state_.audio_pending_head = 0;
    }
}

void DebuggerApp::pump_audio_pending() {
    if (!saudio_isvalid()) return;
    while (state_.audio_pending_head < state_.audio_pending.size()) {
        const int queued_frames =
            static_cast<int>((state_.audio_pending.size() - state_.audio_pending_head) / kAudioChannels);
        if (queued_frames <= 0) break;
        const int free_frames = saudio_expect();
        if (free_frames <= 0) break;
        const int push_frames = (queued_frames < free_frames) ? queued_frames : free_frames;
        const int pushed = saudio_push(state_.audio_pending.data() + state_.audio_pending_head, push_frames);
        if (pushed <= 0) break;
        state_.audio_pending_head += static_cast<std::size_t>(pushed * kAudioChannels);
    }
    compact_audio_pending();
}

void DebuggerApp::queue_audio_frame() {
    if (!state_.machine || !state_.mixer) return;
    zcdi::AudioStreams streams = state_.machine->drain_audio();
    state_.mixer->push_samples(streams.cdic);
    state_.mixer->push_dvc_samples(streams.dvc);
    const int out_frames = state_.audio_frames_per_emu_frame;
    const int16_t* pcm = state_.mixer->render(out_frames);
    const std::size_t append_count = static_cast<std::size_t>(out_frames * kAudioChannels);
    state_.audio_pending.reserve(state_.audio_pending.size() + append_count);
    // Muted still queues silence: the queue paces emulation.
    for (std::size_t i = 0; i < append_count; ++i) {
        state_.audio_pending.push_back(state_.muted ? 0.0f : static_cast<float>(pcm[i]) / 32768.0f);
    }
}

void DebuggerApp::step_emulation_frame() {
    if (!state_.machine) return;

    const bool dpad_active = state_.input.keyboard_left || state_.input.keyboard_right || state_.input.keyboard_up ||
                             state_.input.keyboard_down;
    state_.input.keyboard_dpad_held_frames = dpad_active ? (state_.input.keyboard_dpad_held_frames + 1) : 0;
    if (dpad_active) {
        const int step = zcdi::SerialPointer::gamepad_speed_n_step(state_.input.keyboard_dpad_held_frames);
        if (state_.input.keyboard_left) state_.input.dx -= step;
        if (state_.input.keyboard_right) state_.input.dx += step;
        if (state_.input.keyboard_up) state_.input.dy -= step;
        if (state_.input.keyboard_down) state_.input.dy += step;
    }

    const bool button1 = state_.input.mouse_button1 || state_.input.keyboard_key_z || state_.input.keyboard_key_s;
    const bool button2 = state_.input.mouse_button2 || state_.input.keyboard_key_x;
    const bool button12 = state_.input.mouse_button12 || state_.input.keyboard_key_a;

    state_.machine->bus().update_pointer(state_.input.dx, state_.input.dy, button1, button2, button12);
    state_.input.dx = 0;
    state_.input.dy = 0;

    state_.machine->run_frame();
    queue_audio_frame();
    copy_framebuffer();

    std::string uart = state_.machine->bus().drain_uart_output();
    if (!uart.empty()) {
        std::fwrite(uart.data(), 1, uart.size(), stdout);
        std::fflush(stdout);
    }
}

// Mid-frame, the lines drawn so far over the previous frame's.
void DebuggerApp::copy_framebuffer() {
    if (!state_.machine) return;
    state_.active_w = state_.machine->bus().video_active_width();
    state_.active_h = state_.machine->bus().video_active_height();
    state_.display_aspect = static_cast<float>(state_.machine->bus().video_display_aspect());
    const uint32_t* src = state_.machine->bus().video_framebuffer();
    if (!src || state_.framebuffer.empty()) return;
    for (std::size_t i = 0; i < state_.framebuffer.size(); ++i)
        state_.framebuffer[i] = xrgb_to_rgba(src[i]);
}

void DebuggerApp::upload_frame() { state_.frame_texture.update(state_.framebuffer); }

void DebuggerApp::toggle_running() { state_.command = state_.running ? RunCommand::kPause : RunCommand::kRun; }

bool DebuggerApp::emulate_frame() {
    const EmuRunner::Stop why = state_.runner->run(1);
    if (why == EmuRunner::Stop::kFrames) {
        ++emu_frames_counted_;
        return true;
    }
    on_stop(why);
    return false;
}

// Whole frames run in the last second, as the status bar shows it.
void DebuggerApp::update_emu_fps() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - emu_fps_since_).count();
    if (elapsed < 1.0) return;
    emu_fps_ = emu_frames_counted_ / elapsed;
    emu_frames_counted_ = 0;
    emu_fps_since_ = now;
}

void DebuggerApp::on_stop(EmuRunner::Stop why) {
    state_.running = false;
    if (why == EmuRunner::Stop::kBreakpoint) set_rendering_maximized(false); // show where it broke
    state_.runner->begin(EmuRunner::Mode::kNone);
    state_.runner->note_paused();
    ++state_.stop_serial;
    const zcdi::MachineInspector view(*state_.machine);
    const uint32_t pc = view.pc();
    char text[160]{};
    const std::string note = why == EmuRunner::Stop::kBreakpoint && !state_.runner->stop_note.empty()
                                 ? "Break on " + state_.runner->stop_note
                                 : "";
    const char* what = why == EmuRunner::Stop::kBreakpoint ? (note.empty() ? "Breakpoint" : note.c_str())
                       : why == EmuRunner::Stop::kStep     ? "Stopped"
                                                           : "Paused";
    const std::string where = state_.symbols.where(pc);
    if (state_.runner->mid_frame()) {
        std::snprintf(text, sizeof(text), "%s at %06X%s%s, line %d", what, static_cast<unsigned>(pc),
                      where.empty() ? "" : " ", where.c_str(), view.line());
        copy_framebuffer();
    } else {
        std::snprintf(text, sizeof(text), "%s at frame end, PC %06X%s%s", what, static_cast<unsigned>(pc),
                      where.empty() ? "" : " ", where.c_str());
    }
    state_.stop_text = text;
}

// Run commands from keys, menus and widgets; stepping only while paused.
void DebuggerApp::execute_command() {
    const RunCommand command = state_.command;
    state_.command = RunCommand::kNone;
    if (!state_.machine || command == RunCommand::kNone) return;
    EmuRunner& runner = *state_.runner;
    if (command == RunCommand::kPause) {
        if (state_.running) on_stop(EmuRunner::Stop::kFrames);
        return;
    }
    if (state_.running) return;
    const zcdi::MachineInspector view(*state_.machine);
    auto run_paced = [&](EmuRunner::Mode mode, uint32_t target) {
        runner.begin(mode, target);
        state_.running = true;
        state_.fallback_accum_sec = 0.0;
        state_.stop_text.clear();
    };
    // At most one frame, then paused wherever it stopped.
    auto step = [&](EmuRunner::Mode mode) {
        runner.begin(mode);
        if (emulate_frame()) on_stop(EmuRunner::Stop::kFrames);
        upload_frame();
    };
    switch (command) {
        case RunCommand::kRun: run_paced(EmuRunner::Mode::kNone, 0); break;
        case RunCommand::kStepFrame: step(EmuRunner::Mode::kNone); break;
        case RunCommand::kStepOver: {
            // BSR, JSR, TRAP (+ function word): run to the next instruction.
            const uint32_t pc = view.pc();
            const uint16_t op = view.peek16(pc);
            uint32_t next = 0;
            if ((op & 0xff00) == 0x6100 || (op & 0xffc0) == 0x4e80) next = pc + view.disassemble(pc).size;
            if (is_trap_opcode(op)) next = pc + 4;
            if (next != 0) {
                run_paced(EmuRunner::Mode::kStepOver, next);
                break;
            }
            step(EmuRunner::Mode::kStepInto);
            break;
        }
        case RunCommand::kStepInto:
        case RunCommand::kStepLine:
            step(command == RunCommand::kStepInto ? EmuRunner::Mode::kStepInto : EmuRunner::Mode::kStepLine);
            break;
        case RunCommand::kStepOut: run_paced(EmuRunner::Mode::kStepOut, 0); break;
        case RunCommand::kRunTo: run_paced(EmuRunner::Mode::kRunTo, state_.command_target); break;
        case RunCommand::kNone:
        case RunCommand::kPause: break;
    }
}

void DebuggerApp::reset_machine(bool cold) {
    if (!state_.machine) return;
    state_.runner->finish_frame();
    if (cold) {
        state_.machine->bus().set_auto_play_enabled(state_.auto_play);
        state_.machine->cold_boot();
        state_.runner->attach(state_.machine.get());
    } else {
        state_.machine->reset();
    }
    *state_.mixer = zcdi::Mixer(kAudioSampleRate);
    state_.audio_pending.clear();
    state_.audio_pending_head = 0;
    state_.status = cold ? "Power cycled" : "Reset";
}

// A slot in memory, like libretro's: machine, then the mixer's ZCST part.
void DebuggerApp::quick_save() {
    // States hold whole frames only.
    if (!state_.machine || !state_.mixer || state_.runner->mid_frame()) return;
    quick_state_.clear();
    zcdi::StateWriter w(quick_state_);
    state_.machine->save_state(w);
    state_.mixer->state_io(w);
    if (w.failed()) {
        quick_state_.clear();
        state_.status = "Quick save failed";
        return;
    }
    state_.status = "Quick saved at frame " + std::to_string(zcdi::MachineInspector(*state_.machine).frame());
}

void DebuggerApp::quick_load() {
    if (!state_.machine || !state_.mixer || quick_state_.empty() || state_.runner->mid_frame()) return;
    zcdi::StateReader mixer_part(quick_state_);
    mixer_part.skip_section(zcdi::state_tag("ZCST"));
    zcdi::Mixer mixer = *state_.mixer;
    mixer.state_io(mixer_part);
    zcdi::StateReader r(quick_state_);
    if (mixer_part.failed() || !state_.machine->load_state(r)) {
        state_.status = "Quick load failed";
        return;
    }
    *state_.mixer = std::move(mixer);
    state_.audio_pending.clear();
    state_.audio_pending_head = 0;
    // Show the loaded state's picture: the framebuffer isn't in the state.
    if (!state_.running) state_.command = RunCommand::kStepFrame;
    state_.status = "Quick loaded frame " + std::to_string(zcdi::MachineInspector(*state_.machine).frame());
}

void DebuggerApp::setup_frame_texture(int width, int height) {
    state_.frame_w = width;
    state_.frame_h = height;
    state_.active_w = width;
    state_.active_h = height;
    state_.framebuffer.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0xff000000u);
    state_.frame_texture.create(width, height, "zcdi-framebuffer");
}

void DebuggerApp::load_symbols() {
    std::vector<std::string> paths;
    if (!config_.bios_path.empty()) {
        // Optional: listings named after the ROMs, beside them.
        std::error_code ec;
        for (const std::filesystem::path& rom : {std::filesystem::path(config_.bios_path), dvc_rom_path()}) {
            const std::filesystem::path candidate = rom.parent_path() / (rom.stem().string() + ".asm");
            if (std::filesystem::is_regular_file(candidate, ec)) paths.push_back(candidate.string());
        }
    }
    for (const std::string& path : paths) {
        const bool ok = state_.symbols.load(path);
        log(ok ? SlogLevel::Info : SlogLevel::Warning, (ok ? "Symbols: " : "Couldn't read symbols: ") + path);
    }
}

void DebuggerApp::start_machine() {
    if (config_.bios_path.empty()) {
        state_.status = "No BIOS: File > Open BIOS, or drop a .rom on the window";
        return;
    }

    const std::string& bios_path = config_.bios_path;
    auto bios = read_binary_file(bios_path);
    if (bios.empty()) {
        state_.status = "Failed to read BIOS file: " + bios_path;
        return;
    }
    state_.bios_name = std::filesystem::path(bios_path).filename().string();
    log(SlogLevel::Info, "BIOS: " + bios_path);

    const std::filesystem::path dvc_path = dvc_rom_path();
    auto dvc = read_binary_file(dvc_path.string());
    state_.dvc_rom_loaded = !dvc.empty();
    if (state_.dvc_rom_loaded) {
        log(SlogLevel::Info, "DVC ROM loaded: " + dvc_path.string());
    } else {
        log(SlogLevel::Warning, "DVC ROM not found: " + dvc_path.string());
    }

    state_.mixer = std::make_unique<zcdi::Mixer>(kAudioSampleRate);
    quick_state_.clear(); // a state only loads into the machine it came from

    // Musashi is one global CPU: drop the old machine before building the next.
    state_.runner->finish_frame();
    state_.runner->attach(nullptr);
    state_.machine.reset();
    state_.machine = std::make_unique<zcdi::Machine>(
        std::span<const std::byte>(bios.data(), bios.size()), state_.video_standard,
        zcdi::Machine::InitParams{.dvc = std::span<const std::byte>(dvc.data(), dvc.size()),
                                  .auto_play = state_.auto_play});
    state_.built_video_standard = state_.video_standard;
    state_.machine->cold_boot();

    if (!config_.disc_path.empty() &&
        !state_.machine->mount_disc(zcdi::open_disc(std::make_unique<zcdi::DiscContext>(config_.disc_path)))) {
        state_.status = "Failed to mount disc: " + config_.disc_path;
    } else {
        state_.status = "Running";
    }

    int w = state_.machine->bus().video_width();
    int h = state_.machine->bus().video_height();
    state_.audio_frames_per_emu_frame = static_cast<int>(kAudioSampleRate / state_.machine->refresh_rate());
    if (state_.audio_frames_per_emu_frame <= 0) state_.audio_frames_per_emu_frame = 1;
    state_.fallback_accum_sec = 0.0;
    if (w <= 0 || h <= 0) {
        w = 768;
        h = 560;
    }
    setup_frame_texture(w, h);
    state_.running = true;
    state_.stop_text.clear();
    std::string window_title = DebuggerApp::kBaseWindowTitle;
    if (!config_.disc_path.empty()) {
        window_title += " (";
        window_title += std::filesystem::path(config_.disc_path).stem().string();
        window_title += ")";
    }
    sapp_set_window_title(window_title.c_str());
}

void DebuggerApp::init() {
    sg_desc sg_desc{};
    sg_desc.environment = sglue_environment();
    sg_desc.logger.func = slog_func;
    sg_setup(&sg_desc);

    saudio_desc sa_desc{};
    sa_desc.sample_rate = kAudioSampleRate;
    sa_desc.num_channels = kAudioChannels;
    sa_desc.logger.func = slog_func;
    saudio_setup(&sa_desc);
    state_.audio_backend_ok = saudio_isvalid();
    if (!state_.audio_backend_ok) {
        state_.status = "Audio backend unavailable (ALSA device missing), running timer-paced";
    }

    ini_path_ = ini_file_path();
    simgui_desc_t simgui_desc{};
    simgui_desc.ini_filename = ini_path_.c_str();
    simgui_setup(&simgui_desc);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Registered before the first NewFrame, which loads the ini.
    ImGuiSettingsHandler handler{};
    handler.TypeName = "ZcdiDbg";
    handler.TypeHash = ImHashStr("ZcdiDbg");
    handler.ReadOpenFn = &DebuggerApp::settings_open;
    handler.ReadLineFn = &DebuggerApp::settings_read_line;
    handler.WriteAllFn = &DebuggerApp::settings_write_all;
    handler.UserData = this;
    ImGui::AddSettingsHandler(&handler);

    state_.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    state_.pass_action.colors[0].clear_value = {0.08f, 0.08f, 0.1f, 1.0f};

    sapp_set_window_title(DebuggerApp::kBaseWindowTitle);
    state_.runner = std::make_unique<EmuRunner>([this]() { step_emulation_frame(); });
    // Now rather than at the first frame: the remembered BIOS is needed below.
    ImGui::LoadIniSettingsFromDisk(ini_path_.c_str());

    const ImGuiFileBrowserFlags browser_flags = ImGuiFileBrowserFlags_CloseOnEsc | ImGuiFileBrowserFlags_ConfirmOnEnter;
    bios_browser_ = std::make_unique<ImGui::FileBrowser>(browser_flags);
    bios_browser_->SetTitle("Open BIOS");
    // Case-sensitive outside Windows, and older dumps are often upper-case.
    bios_browser_->SetTypeFilters({".rom", ".ROM"});
    disc_browser_ = std::make_unique<ImGui::FileBrowser>(browser_flags);
    disc_browser_->SetTitle("Open disc");
    // What zcdi::open_disc() reads.
    disc_browser_->SetTypeFilters({".cue", ".chd", ".bin", ".iso", ".CUE", ".CHD", ".BIN", ".ISO"});

    std::error_code ec;
    if (config_.bios_path.empty() && std::filesystem::is_regular_file(recent_bios_, ec)) {
        config_.bios_path = recent_bios_;
        state_.video_standard = video_standard_for(config_.bios_path);
    }
    if (!config_.bios_path.empty()) recent_bios_ = config_.bios_path;
    if (!config_.disc_path.empty()) remember_disc(config_.disc_path);
    load_symbols();
    start_machine();
    if (config_.bios_path.empty()) browse(*bios_browser_, "");
}

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

void DebuggerApp::open_file(const std::string& path) {
    if (lower_extension(path) != ".rom") {
        open_disc(path);
    } else if (is_dvc_rom(path)) {
        open_dvc_rom(path);
    } else {
        open_bios(path);
    }
}

std::filesystem::path DebuggerApp::dvc_rom_path() const {
    if (!config_.dvc_path.empty()) return config_.dvc_path;
    return std::filesystem::path(config_.bios_path).parent_path() / kDvcRom;
}

// The cartridge goes in and the player powers on again with it.
void DebuggerApp::open_dvc_rom(const std::string& path) {
    config_.dvc_path = path;
    if (config_.bios_path.empty()) {
        state_.status = "DVC ROM chosen: open a BIOS to start";
        browse(*bios_browser_, "");
        return;
    }
    state_.symbols = Symbols{};
    load_symbols();
    start_machine();
}

// A new player: symbols beside the BIOS are reloaded.
void DebuggerApp::open_bios(const std::string& path) {
    config_.bios_path = path;
    state_.video_standard = video_standard_for(path);
    recent_bios_ = path;
    ImGui::MarkIniSettingsDirty();
    state_.symbols = Symbols{};
    load_symbols();
    start_machine();
}

// Powers the player on again with the disc in, as launching with disc= does.
void DebuggerApp::open_disc(const std::string& path) {
    config_.disc_path = path;
    remember_disc(path);
    if (config_.bios_path.empty()) {
        state_.status = "Disc chosen: open a BIOS to start";
        browse(*bios_browser_, "");
        return;
    }
    start_machine();
}

void DebuggerApp::remember_disc(const std::string& path) {
    std::erase(recent_discs_, path);
    recent_discs_.insert(recent_discs_.begin(), path);
    if (recent_discs_.size() > kMaxRecentDiscs) recent_discs_.resize(kMaxRecentDiscs);
    ImGui::MarkIniSettingsDirty();
}

void DebuggerApp::frame() {
    if (!dropped_path_.empty()) open_file(std::exchange(dropped_path_, {}));
    if (state_.machine && state_.video_standard != state_.built_video_standard) start_machine();

    bool emu_stepped = false;
    execute_command();
    if (state_.machine && state_.running) {
        if (state_.audio_backend_ok && saudio_isvalid()) {
            pump_audio_pending();
            const int low_water_frames = state_.audio_frames_per_emu_frame * 2;
            for (int i = 0; i < kMaxEmuFramesPerRender; ++i) {
                if (saudio_expect() < low_water_frames) break;
                emu_stepped = true;
                const bool go_on = emulate_frame();
                pump_audio_pending();
                if (!go_on) break;
            }
        } else {
            const double frame_dt = sapp_frame_duration();
            double dt = frame_dt;
            if (dt < 0.0) dt = 0.0;
            if (dt > 0.25) dt = 0.25;
            const double target_step = 1.0 / state_.machine->refresh_rate();
            state_.fallback_accum_sec += dt;
            for (int i = 0; i < kMaxEmuFramesPerRender; ++i) {
                if (state_.fallback_accum_sec < target_step) break;
                state_.fallback_accum_sec -= target_step;
                emu_stepped = true;
                if (!emulate_frame()) break;
            }
        }
    }
    if (emu_stepped) upload_frame();
    update_emu_fps();
    if (state_.runner->breakpoints != saved_breakpoints_ || state_.runner->os9_breaks != saved_os9_breaks_ ||
        state_.runner->vector_breaks != saved_vector_breaks_) {
        saved_breakpoints_ = state_.runner->breakpoints;
        saved_os9_breaks_ = state_.runner->os9_breaks;
        saved_vector_breaks_ = state_.runner->vector_breaks;
        ImGui::MarkIniSettingsDirty();
    }

    simgui_frame_desc_t frame_desc{};
    frame_desc.width = sapp_width();
    frame_desc.height = sapp_height();
    frame_desc.delta_time = sapp_frame_duration();
    frame_desc.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&frame_desc);

    handle_hotkeys();
    const bool maximized = state_.rendering_maximized;
    if (maximized) {
        // Keeps the hidden layout, so it comes back as it was. Passthru: without
        // it the host window paints its background over the rendering.
        ImGui::DockSpaceOverViewport(ImGui::GetID(kDockspaceName), ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_KeepAliveOnly | ImGuiDockNodeFlags_PassthruCentralNode);
    } else {
        // Menu and bars first: they shrink the work area the dockspace fills.
        draw_menu_bar();
        draw_toolbar();
        draw_status_bar();
        draw_dockspace();
        draw_file_browsers();
    }

    // Widgets will set this true if the rendering image is hovered this frame.
    state_.rendering_hovered = false;

    for (std::size_t i = 0; i < widgets_.size(); ++i) {
        DebuggerWidget& widget = *widgets_[i];
        if (maximized) {
            if (&widget == rendering_) widget.draw(state_);
            continue;
        }
        if (widget.open) widget.draw(state_);
        if (widget.open != widgets_were_open_[i]) {
            widgets_were_open_[i] = widget.open;
            ImGui::MarkIniSettingsDirty();
        }
    }

    sg_pass pass{};
    pass.action = state_.pass_action;
    pass.swapchain = sglue_swapchain();

    sg_begin_pass(&pass);
    simgui_render();
    sg_end_pass();
    sg_commit();
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
// Options: "auto_play=0|1", "tv_aspect=0|1".
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
    out->appendf("[%s][Options]\nauto_play=%d\ntv_aspect=%d\n\n", handler->TypeName, app->state_.auto_play ? 1 : 0,
                 app->state_.tv_aspect ? 1 : 0);
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

void DebuggerApp::cleanup() {
    state_.runner.reset(); // finishes a cut frame on the machine
    for (const auto& widget : widgets_)
        widget->release();
    state_.frame_texture.release();
    state_.audio_pending.clear();
    state_.audio_pending_head = 0;
    state_.mixer.reset();
    state_.machine.reset();
    saudio_shutdown();
    simgui_shutdown();
    sg_shutdown();
}

void DebuggerApp::event(const sapp_event* ev) {
    simgui_handle_event(ev);

    auto release_capture = [this]() {
        state_.mouse_captured_for_game = false;
        sapp_lock_mouse(false);
        state_.input.dx = 0;
        state_.input.dy = 0;
        state_.input.mouse_button1 = false;
        state_.input.mouse_button2 = false;
        state_.input.mouse_button12 = false;
    };

    auto set_keyboard_key = [this](sapp_keycode key_code, bool down) {
        switch (key_code) {
            case SAPP_KEYCODE_LEFT: state_.input.keyboard_left = down; break;
            case SAPP_KEYCODE_RIGHT: state_.input.keyboard_right = down; break;
            case SAPP_KEYCODE_UP: state_.input.keyboard_up = down; break;
            case SAPP_KEYCODE_DOWN: state_.input.keyboard_down = down; break;
            // Z/S: button1, X: button2, A: button1+2.
            case SAPP_KEYCODE_Z: state_.input.keyboard_key_z = down; break;
            case SAPP_KEYCODE_S: state_.input.keyboard_key_s = down; break;
            case SAPP_KEYCODE_X: state_.input.keyboard_key_x = down; break;
            case SAPP_KEYCODE_A: state_.input.keyboard_key_a = down; break;
            default: break;
        }
    };

    switch (ev->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
            set_keyboard_key(ev->key_code, true);
            if (ev->key_code == SAPP_KEYCODE_ESCAPE && state_.mouse_captured_for_game) {
                release_capture();
            } else if (ev->key_code == SAPP_KEYCODE_ESCAPE && state_.rendering_maximized) {
                set_rendering_maximized(false);
            } else if (ev->key_code == SAPP_KEYCODE_F8) {
                if (state_.mouse_captured_for_game) {
                    release_capture();
                } else if (state_.rendering_hovered) {
                    state_.mouse_captured_for_game = true;
                    // for WSLg pointer is absolute, so raw motion isn't a delta.
                    if (!std::getenv("WSL_DISTRO_NAME")) sapp_lock_mouse(true);
                } else {
                    state_.status = "Hover Rendering window, then press F8 to capture mouse";
                }
            }
            break;
        case SAPP_EVENTTYPE_FILES_DROPPED: {
            // A .cue dropped with its .bin files: take the image, not a track.
            const int count = sapp_get_num_dropped_files();
            for (int i = 0; i < count; ++i) {
                const std::string path = sapp_get_dropped_file_path(i);
                const std::string ext = lower_extension(path);
                if (i == 0 || ext == ".rom" || ext == ".cue" || ext == ".chd" || ext == ".iso") dropped_path_ = path;
                if (ext != ".bin") break;
            }
            break;
        }
        case SAPP_EVENTTYPE_KEY_UP: set_keyboard_key(ev->key_code, false); break;
        case SAPP_EVENTTYPE_MOUSE_MOVE:
            if (state_.mouse_captured_for_game) {
                state_.input.dx += static_cast<int>(ev->mouse_dx);
                state_.input.dy += static_cast<int>(ev->mouse_dy);
            }
            break;
        case SAPP_EVENTTYPE_UNFOCUSED: // sokol drops the lock here
            if (state_.mouse_captured_for_game) release_capture();
            break;
        case SAPP_EVENTTYPE_MOUSE_DOWN:
            if (state_.mouse_captured_for_game) {
                if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) state_.input.mouse_button1 = true;
                if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT) state_.input.mouse_button2 = true;
                if (ev->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) state_.input.mouse_button12 = true;
            }
            break;
        case SAPP_EVENTTYPE_MOUSE_UP:
            if (state_.mouse_captured_for_game) {
                if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) state_.input.mouse_button1 = false;
                if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT) state_.input.mouse_button2 = false;
                if (ev->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) state_.input.mouse_button12 = false;
            }
            break;
        default: break;
    }
}
