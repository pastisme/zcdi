#include "app.h"

#include "core/pointing_device.h"

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

// The PAL BIOS of the libretro core (libretro_core.cpp kRegionProfiles).
constexpr const char* kPalBios = "cdi220b.rom";

// The BIOS picks the player model, as in the libretro core.
zcdi::VideoStandard video_standard_for(const std::string& bios_path) {
    return std::filesystem::path(bios_path).filename() == kPalBios ? zcdi::VideoStandard::kPal
                                                                   : zcdi::VideoStandard::kNtsc;
}

// The DVC cartridge ROM, looked for beside the BIOS (as the libretro core does).
constexpr const char* kDvcRom = "vmpega.rom";

bool is_dvc_rom(const std::string& path) {
    std::string name = std::filesystem::path(path).filename().string();
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name == kDvcRom;
}

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

    zcdi::Machine& machine = *state_.machine;
    machine.plug(zcdi::Socket::kFront, state_.front_mouse ? zcdi::DeviceId::kRelative : zcdi::DeviceId::kManeuvering);
    const bool button12 = state_.input.pressed_button12 || state_.input.mouse_button12 || state_.input.keyboard_key_a;
    machine.set_socket_input(zcdi::Socket::kFront,
                             {
                                 .left = state_.input.keyboard_left,
                                 .right = state_.input.keyboard_right,
                                 .up = state_.input.keyboard_up,
                                 .down = state_.input.keyboard_down,
                                 .mouse_dx = state_.input.dx,
                                 .mouse_dy = state_.input.dy,
                                 .button1 = button12 || state_.input.pressed_button1 || state_.input.mouse_button1 ||
                                            state_.input.keyboard_key_z || state_.input.keyboard_key_s,
                                 .button2 = button12 || state_.input.pressed_button2 || state_.input.mouse_button2 ||
                                            state_.input.keyboard_key_x,
                             });
    state_.input.dx = 0;
    state_.input.dy = 0;
    state_.input.pressed_button1 = false;
    state_.input.pressed_button2 = false;
    state_.input.pressed_button12 = false;

    state_.machine->run_frame();
    queue_audio_frame();
    copy_framebuffer();

    std::string uart = state_.machine->drain_uart_output();
    if (!uart.empty()) {
        std::fwrite(uart.data(), 1, uart.size(), stdout);
        std::fflush(stdout);
    }
}

// Mid-frame, the lines drawn so far over the previous frame's.
void DebuggerApp::copy_framebuffer() {
    if (!state_.machine) return;
    const zcdi::Machine::VideoFrame frame = state_.machine->video_frame();
    state_.active_w = frame.width;
    state_.active_h = frame.height;
    state_.display_aspect = static_cast<float>(frame.display_aspect);
    const uint32_t* src = frame.pixels;
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
    if (state_.runner->mid_frame()) {
        std::snprintf(text, sizeof(text), "%s at %06X, line %d", what, static_cast<unsigned>(pc), view.line());
        copy_framebuffer();
    } else {
        std::snprintf(text, sizeof(text), "%s at frame end, PC %06X", what, static_cast<unsigned>(pc));
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
    if (cold && state_.auto_play != state_.built_auto_play) {
        start_machine(); // Auto Play is fixed when the machine is built
        if (state_.machine) state_.runner->attach(state_.machine.get());
        return;
    }
    if (cold) {
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
    state_.built_auto_play = state_.auto_play;
    state_.machine->cold_boot();

    if (!config_.disc_path.empty() &&
        !state_.machine->mount_disc(zcdi::open_disc(std::make_unique<zcdi::DiscContext>(config_.disc_path)))) {
        state_.status = "Failed to mount disc: " + config_.disc_path;
    } else {
        state_.status = "Running";
    }

    int w = state_.machine->video_frame().pitch;
    int h = state_.machine->video_frame().buffer_height;
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
    start_machine();
    if (config_.bios_path.empty()) browse(*bios_browser_, "");
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
    start_machine();
}

void DebuggerApp::open_bios(const std::string& path) {
    config_.bios_path = path;
    state_.video_standard = video_standard_for(path);
    recent_bios_ = path;
    ImGui::MarkIniSettingsDirty();
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
