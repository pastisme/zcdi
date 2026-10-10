#include "widgets/trace_widget.h"

#include "core/machine_inspector.h"
#include "imgui/imgui.h"
#include "names.h"
#include "widgets/widget_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool is_trap(uint8_t v) { return v >= 32 && v <= 47; }

ImU32 vector_color(uint8_t v) {
    // A stable, distinct hue per vector.
    const float hue = static_cast<float>((v * 37) % 256) / 256.0f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.7f, 0.95f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
}

} // namespace

void TraceWidget::draw(AppState& state) {
    ImGui::Begin(title(), &open);
    if (!state.runner || !state.machine) {
        ImGui::End();
        return;
    }
    if (state.stop_serial != seen_stop_) {
        seen_stop_ = state.stop_serial;
        scroll_to_end_ = true;
    }
    ImGui::Checkbox("Record", &state.runner->tracing);
    ImGui::SameLine();
    ImGui::TextDisabled("costs some speed; shown while paused");
    if (state.running) {
        ImGui::TextDisabled("Pause to read the trace.");
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("trace_tabs")) {
        if (ImGui::BeginTabItem("Instructions")) {
            draw_instructions(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Exceptions")) {
            draw_exceptions(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Timeline")) {
            draw_timeline(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    scroll_to_end_ = false;
    ImGui::End();
}

// Newest last, so the stop is at the bottom.
void TraceWidget::draw_instructions(AppState& state) {
    const Ring<TraceEntry>& trace = state.runner->trace;
    const zcdi::MachineInspector view(*state.machine);
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("trace", 3, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("line");
    ImGui::TableSetupColumn("address");
    ImGui::TableSetupColumn("instruction", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(trace.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const TraceEntry& e = trace[static_cast<std::size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", e.line);
            ImGui::TableSetColumnIndex(1);
            address_cell(state, e.pc, i);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(view.disassemble(e.pc).text);
        }
    }
    if (scroll_to_end_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndTable();
}

void TraceWidget::draw_exceptions(AppState& state) {
    const Ring<ExceptionEntry>& exceptions = state.runner->exceptions;
    ImGui::Checkbox("Hide TRAPs (OS-9 calls)", &hide_traps_);
    std::vector<std::size_t> shown;
    shown.reserve(exceptions.size());
    for (std::size_t i = 0; i < exceptions.size(); ++i) {
        if (!(hide_traps_ && is_trap(exceptions[i].vector))) shown.push_back(i);
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("exceptions", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("frame:line");
    ImGui::TableSetupColumn("exception");
    ImGui::TableSetupColumn("handler");
    ImGui::TableSetupColumn("after", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const ExceptionEntry& e = exceptions[shown[static_cast<std::size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d:%d", e.frame, e.line);
            ImGui::TableSetColumnIndex(1);
            std::string name = vector_name(e.vector);
            if (e.vector == 32) {
                // `from` is the TRAP; its function word follows.
                const char* call = os9_call_name(zcdi::MachineInspector(*state.machine).peek16(e.from + 2));
                if (call) name += std::string("  ") + call;
            }
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vector_color(e.vector)), "%s", name.c_str());
            ImGui::TableSetColumnIndex(2);
            address_cell(state, e.handler, row * 2);
            ImGui::TableSetColumnIndex(3);
            address_cell(state, e.from, row * 2 + 1);
        }
    }
    if (scroll_to_end_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndTable();
}

// One row per recent field, a tick at each exception's line.
void TraceWidget::draw_timeline(AppState& state) {
    const Ring<ExceptionEntry>& exceptions = state.runner->exceptions;
    if (exceptions.size() == 0) {
        ImGui::TextDisabled("No exceptions recorded");
        return;
    }
    ImGui::Checkbox("Hide TRAPs (OS-9 calls)", &hide_traps_);
    const int last_frame = exceptions[exceptions.size() - 1].frame;
    constexpr int kFrames = 16;
    const int field_lines = std::max(1, static_cast<int>(zcdi::field_lines(state.built_video_standard)));
    const float label_w = ImGui::CalcTextSize("000000").x + 8.0f;
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x - label_w);
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const zcdi::MachineInspector view(*state.machine);
    const int first_active = view.mcd212().first_active_line();
    const int active_end = first_active + view.mcd212().active_height();
    for (int frame = last_frame - kFrames + 1; frame <= last_frame; ++frame) {
        ImGui::TextDisabled("%6d", frame);
        ImGui::SameLine(label_w);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##row", ImVec2(width, row_h - 2.0f));
        const bool hovered = ImGui::IsItemHovered();
        draw->AddRectFilled(p, ImVec2(p.x + width, p.y + row_h - 2.0f), ImGui::GetColorU32(ImGuiCol_FrameBg));
        // The active (displayed) lines, lighter.
        draw->AddRectFilled(ImVec2(p.x + width * first_active / field_lines, p.y),
                            ImVec2(p.x + width * active_end / field_lines, p.y + row_h - 2.0f),
                            ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
        std::string tip;
        for (std::size_t i = 0; i < exceptions.size(); ++i) {
            const ExceptionEntry& e = exceptions[i];
            if (e.frame != frame || (hide_traps_ && is_trap(e.vector))) continue;
            const float x = p.x + width * static_cast<float>(e.line) / field_lines;
            draw->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + row_h - 2.0f), vector_color(e.vector), 2.0f);
            if (hovered && std::abs(ImGui::GetIO().MousePos.x - x) < 4.0f) {
                char item[64]{};
                std::snprintf(item, sizeof(item), "line %d: %s\n", e.line, vector_name(e.vector).c_str());
                tip += item;
            }
        }
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::TextDisabled("lines 0-%d left to right; the lighter band is the displayed area", field_lines - 1);
}
