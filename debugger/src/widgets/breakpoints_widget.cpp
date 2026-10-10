#include "widgets/breakpoints_widget.h"

#include "imgui/imgui.h"
#include "names.h"
#include "widgets/widget_ui.h"

#include <cstdlib>
#include <optional>
#include <string>

namespace {

// Exceptions worth stopping on; the IRQs and TRAPs fire all the time.
constexpr uint8_t kBreakableVectors[] = {2, 3, 4, 5, 6, 7, 8, 10, 11, 15, 24};

} // namespace

void BreakpointsWidget::draw(AppState& state) {
    ImGui::Begin(title(), &open);
    if (!state.runner) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(hex_input_width());
    const bool entered = ImGui::InputText("##add", add_text_, sizeof(add_text_),
                                          ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || entered) && add_text_[0] != '\0') {
        state.runner->breakpoints[static_cast<uint32_t>(std::strtoul(add_text_, nullptr, 16))] = true;
        add_text_[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) state.runner->breakpoints.clear();

    std::optional<uint32_t> remove;
    if (ImGui::BeginTable("breakpoints", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        for (auto& [addr, enabled] : state.runner->breakpoints) {
            ImGui::PushID(static_cast<int>(addr));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("##on", &enabled);
            ImGui::TableSetColumnIndex(1);
            address_cell(state, addr, 0);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("x")) remove = addr;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove) state.runner->breakpoints.erase(*remove);

    // TRAP #0 calls, stopped on before the kernel runs them.
    if (ImGui::CollapsingHeader("OS-9 calls", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F$SRqCMem  ").x + ImGui::GetFrameHeight());
        const char* picked = os9_call_name(static_cast<uint16_t>(os9_pick_));
        if (ImGui::BeginCombo("##os9", picked ? picked : "?")) {
            for (uint16_t code = 0; code <= 0x8f; ++code) {
                const char* name = os9_call_name(code);
                if (name && ImGui::Selectable(name, code == os9_pick_)) os9_pick_ = code;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add call")) state.runner->os9_breaks.insert(static_cast<uint16_t>(os9_pick_));
        std::optional<uint16_t> drop;
        for (const uint16_t code : state.runner->os9_breaks) {
            ImGui::PushID(code);
            const char* name = os9_call_name(code);
            ImGui::Text("%s ($%02X)", name ? name : "?", code);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) drop = code;
            ImGui::PopID();
        }
        if (drop) state.runner->os9_breaks.erase(*drop);
    }

    // Caught at the handler's first instruction.
    if (ImGui::CollapsingHeader("Exceptions", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const uint8_t vector : kBreakableVectors) {
            bool on = state.runner->vector_breaks.count(vector) != 0;
            if (ImGui::Checkbox(vector_name(vector).c_str(), &on)) {
                if (on) {
                    state.runner->vector_breaks.insert(vector);
                } else {
                    state.runner->vector_breaks.erase(vector);
                }
            }
        }
    }
    ImGui::End();
}
