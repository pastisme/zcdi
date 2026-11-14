#include "app.h"

#include "sokol/sokol_app.h"
#include "sokol/sokol_imgui.h"

#include <cstdlib>
#include <string>

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
        state_.input.pressed_button1 = false;
        state_.input.pressed_button2 = false;
        state_.input.pressed_button12 = false;
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
        if (!down) return;
        if (key_code == SAPP_KEYCODE_Z || key_code == SAPP_KEYCODE_S) state_.input.pressed_button1 = true;
        if (key_code == SAPP_KEYCODE_X) state_.input.pressed_button2 = true;
        if (key_code == SAPP_KEYCODE_A) state_.input.pressed_button12 = true;
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
                if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT)
                    state_.input.mouse_button1 = state_.input.pressed_button1 = true;
                if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
                    state_.input.mouse_button2 = state_.input.pressed_button2 = true;
                if (ev->mouse_button == SAPP_MOUSEBUTTON_MIDDLE)
                    state_.input.mouse_button12 = state_.input.pressed_button12 = true;
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
