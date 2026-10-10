#pragma once

#include "imgui/imgui.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_imgui.h"

#include <cstdint>
#include <vector>

// An RGBA8 image ImGui can draw, refilled from the CPU.
class StreamTexture {
public:
    // Drops any previous image first.
    void create(int width, int height, const char* label) {
        release();
        sg_image_desc desc{};
        desc.width = width;
        desc.height = height;
        desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.usage.stream_update = true;
        desc.label = label;
        image_ = sg_make_image(&desc);
        sg_view_desc view_desc{};
        view_desc.texture.image = image_;
        view_ = sg_make_view(&view_desc);
        texture_ = simgui_imtextureid(view_);
    }

    // Once per frame at most: sokol streams one update a frame.
    void update(const std::vector<uint32_t>& rgba) {
        if (!valid() || rgba.empty()) return;
        sg_image_data data{};
        data.mip_levels[0].ptr = rgba.data();
        data.mip_levels[0].size = rgba.size() * sizeof(uint32_t);
        sg_update_image(image_, &data);
    }

    void release() {
        if (view_.id != SG_INVALID_ID) sg_destroy_view(view_);
        if (image_.id != SG_INVALID_ID) sg_destroy_image(image_);
        view_ = {};
        image_ = {};
        texture_ = ImTextureID_Invalid;
    }

    bool valid() const { return image_.id != SG_INVALID_ID; }
    ImTextureID id() const { return texture_; }

private:
    sg_image image_{};
    sg_view view_{};
    ImTextureID texture_ = ImTextureID_Invalid;
};

// 0x00RRGGBB to opaque RGBA8 (R in the low byte).
inline uint32_t xrgb_to_rgba(uint32_t xrgb) {
    return ((xrgb >> 16) & 0xffu) | (xrgb & 0xff00u) | ((xrgb & 0xffu) << 16) | 0xff000000u;
}

// The widest size of that width / height that fits `avail`.
inline ImVec2 fit_aspect(ImVec2 avail, float aspect = 4.0f / 3.0f) {
    float w = avail.x > 1.0f ? avail.x : 1.0f;
    float h = w / aspect;
    if (avail.y > 1.0f && h > avail.y) {
        h = avail.y;
        w = h * aspect;
    }
    return ImVec2(w, h);
}
