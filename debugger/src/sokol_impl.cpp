#define SOKOL_IMPL
#define SOKOL_ARGS_IMPL
#define SOKOL_IMGUI_IMPL

#if defined(__APPLE__)
#define SOKOL_METAL
#elif defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE
#endif

#include "imgui/imgui.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_args.h"
#include "sokol/sokol_audio.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_imgui.h"
#include "sokol/sokol_log.h"
