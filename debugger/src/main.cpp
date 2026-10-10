#include "sokol/sokol_app.h"
#include "sokol/sokol_args.h"
#include "sokol/sokol_log.h"

#include <memory>
#include <string>

#include "app.h"

namespace {
std::unique_ptr<DebuggerApp> g_app;

void init() {
    if (g_app) g_app->init();
}

void frame() {
    if (g_app) g_app->frame();
}

void cleanup() {
    if (g_app) {
        g_app->cleanup();
        g_app.reset();
    }
    sargs_shutdown();
}

void event(const sapp_event* ev) {
    if (g_app) g_app->event(ev);
}

} // namespace

sapp_desc sokol_main(int argc, char* argv[]) {
    sargs_desc args_desc{};
    args_desc.argc = argc;
    args_desc.argv = argv;
    sargs_setup(&args_desc);

    // A missing key reads as "".
    g_app = std::make_unique<DebuggerApp>();
    g_app->set_launch_config(sargs_value("bios"), sargs_value("disc"));

    sapp_desc desc{};
    desc.gl.major_version = 3;
    desc.gl.minor_version = 3;
    desc.init_cb = init;
    desc.frame_cb = frame;
    desc.cleanup_cb = cleanup;
    desc.event_cb = event;
    desc.window_title = DebuggerApp::kBaseWindowTitle;
    desc.width = 1440;
    desc.height = 960;
    desc.depth_format = SAPP_PIXELFORMAT_NONE;
    desc.enable_dragndrop = true;
    desc.max_dropped_files = 8; // a .cue and its .bin files
    desc.icon.sokol_default = true;
    desc.logger.func = slog_func;
    return desc;
}
