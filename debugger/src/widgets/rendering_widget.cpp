#include "widgets/rendering_widget.h"

#include "imgui/imgui.h"

void RenderingWidget::draw(AppState& state) {
    if (!state.frame_texture.valid()) return;

    if (state.rendering_maximized) {
        // Its own window: undocking "Rendering" would lose its place in the layout.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32_BLACK);
        ImGui::Begin("##rendering_maximized", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    } else {
        ImGui::Begin(title(), &open);
    }
    // The framebuffer is double horizontal resolution: show the active part at 4:3 or its TV shape.
    const int shown_w = state.active_w > 0 ? state.active_w : state.frame_w;
    const int shown_h = state.active_h > 0 ? state.active_h : state.frame_h;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size = fit_aspect(avail, state.tv_aspect ? state.display_aspect : 4.0f / 3.0f);
    const ImVec2 uv1(static_cast<float>(shown_w) / static_cast<float>(state.frame_w),
                     static_cast<float>(shown_h) / static_cast<float>(state.frame_h));
    if (size.x < avail.x) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f);
    if (state.rendering_maximized && size.y < avail.y) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - size.y) * 0.5f);
    }
    ImGui::Image(state.frame_texture.id(), size, ImVec2(0.0f, 0.0f), uv1);
    state.rendering_hovered = ImGui::IsItemHovered();
    ImGui::End();
}
