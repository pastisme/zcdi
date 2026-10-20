#pragma once

#include "imgui/imgui.h"
#include "sokol/sokol_gfx.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "core/machine.h"
#include "emu_runner.h"
#include "stream_texture.h"
#include "symbols.h"

struct InputState {
    int dx = 0;
    int dy = 0;
    bool mouse_button1 = false;
    bool mouse_button2 = false;
    bool mouse_button12 = false;
    bool keyboard_key_a = false;
    bool keyboard_key_s = false;
    bool keyboard_key_x = false;
    bool keyboard_key_z = false;
    bool keyboard_left = false;
    bool keyboard_right = false;
    bool keyboard_up = false;
    bool keyboard_down = false;
    // Pressed since the last emulated frame, so a click released before it still counts.
    bool pressed_button1 = false;
    bool pressed_button2 = false;
    bool pressed_button12 = false;
};

struct LaunchConfig {
    std::string bios_path;
    std::string disc_path;
    std::string dvc_path; // empty: vmpega.rom beside the BIOS
};

// Asked for by a widget or a key, carried out by the app at the next frame.
enum class RunCommand : uint8_t { kNone, kRun, kPause, kStepInto, kStepOver, kStepOut, kStepLine, kStepFrame, kRunTo };

struct AppState {
    sg_pass_action pass_action{};
    std::unique_ptr<zcdi::Machine> machine;
    std::unique_ptr<zcdi::Mixer> mixer;
    std::unique_ptr<EmuRunner> runner;
    Symbols symbols;
    RunCommand command = RunCommand::kNone;
    uint32_t command_target = 0; // kRunTo
    // Bumped each time emulation stops for the user; widgets refresh on it.
    int stop_serial = 0;
    std::string stop_text;
    // A Disassembly "go to" from another widget.
    std::optional<uint32_t> disasm_goto;
    std::vector<uint32_t> framebuffer;
    std::vector<float> audio_pending;
    std::size_t audio_pending_head = 0;
    StreamTexture frame_texture;
    int frame_w = 0;
    int frame_h = 0;
    // Shown part of the framebuffer (top-left), drawn at 4:3 like libretro.
    int active_w = 0;
    int active_h = 0;
    // Its TV shape (Mcd212::display_aspect), used instead with tv_aspect.
    float display_aspect = 4.0f / 3.0f;
    int audio_frames_per_emu_frame = 735;
    double fallback_accum_sec = 0.0;
    InputState input{};
    std::string status;
    std::string bios_name;
    bool dvc_rom_loaded = false;
    bool rendering_hovered = false;
    // Alt+Enter: Rendering alone and fullscreen, everything else hidden.
    bool rendering_maximized = false;
    bool mouse_captured_for_game = false;
    bool running = false;
    bool audio_backend_ok = false;
    bool muted = false;
    // A strap fixed at power-on: changing it rebuilds the machine.
    zcdi::VideoStandard video_standard = zcdi::VideoStandard::kNtsc;
    zcdi::VideoStandard built_video_standard = zcdi::VideoStandard::kNtsc;
    // Saved in the ini; used from the next power-on.
    bool auto_play = false;
    // Saved in the ini.
    bool tv_aspect = false;
    // Saved in the ini.
    bool front_mouse = false;
};
