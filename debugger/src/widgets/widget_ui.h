#pragma once

#include "app_state.h"
#include "imgui/imgui.h"

#include <cstdint>
#include <cstdio>
#include <string>

// Fits an 8-digit hex input.
inline float hex_input_width() { return ImGui::CalcTextSize("00000000").x + ImGui::GetStyle().FramePadding.x * 2.0f; }

// An address that opens in Disassembly when clicked, then its symbol.
inline void address_cell(AppState& state, uint32_t addr, int id) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%06X", static_cast<unsigned>(addr));
    ImGui::PushID(id);
    if (ImGui::Selectable(text, false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(text))) state.disasm_goto = addr;
    ImGui::PopID();
    const std::string where = state.symbols.where(addr);
    if (!where.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", where.c_str());
    }
}
