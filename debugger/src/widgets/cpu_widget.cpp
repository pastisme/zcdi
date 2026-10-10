#include "widgets/cpu_widget.h"

#include "core/cpu/musashi_core.h"
#include "imgui/imgui.h"
#include "widgets/widget_ui.h"

#include <cstdio>

namespace {

constexpr ImVec4 kChanged{1.0f, 0.75f, 0.3f, 1.0f};
constexpr const char* kEditPopup = "edit_register";

} // namespace

// Paused, a double-click edits the value.
void CpuWidget::reg(const char* name, zcdi::CpuReg id, uint32_t value, bool changed, bool editable, const char* shown) {
    char text[24]{};
    if (!shown) {
        std::snprintf(text, sizeof(text), "%-3s %08X", name, static_cast<unsigned>(value));
        shown = text;
    }
    if (changed) ImGui::PushStyleColor(ImGuiCol_Text, kChanged);
    ImGui::Selectable(shown, false, ImGuiSelectableFlags_AllowDoubleClick, ImGui::CalcTextSize(shown));
    if (changed) ImGui::PopStyleColor();
    if (editable && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        editing_ = id;
        edit_value_ = value;
        ImGui::OpenPopup(kEditPopup);
    }
}

void CpuWidget::draw(AppState& state) {
    if (!begin_machine_window(state)) return;
    const zcdi::MachineInspector view(*state.machine);
    const zcdi::MachineInspector::CpuRegisters regs = view.registers();
    if (state.stop_serial != seen_stop_) {
        seen_stop_ = state.stop_serial;
        before_ = at_stop_;
        at_stop_ = regs;
    }
    // Paused: changes since the last stop are marked, and registers are editable.
    const bool paused = !state.running;
    reg("PC", zcdi::CpuReg::Pc, regs.pc, paused && regs.pc != before_.pc, paused);
    ImGui::SameLine(0.0f, 24.0f);
    // T1 - S - - I2 I1 I0 - - - X N Z V C
    const uint16_t sr = regs.sr;
    char sr_text[40]{};
    std::snprintf(sr_text, sizeof(sr_text), "SR %04X  %c%c I%d %c%c%c%c%c", sr, (sr & 0x8000) ? 'T' : '-',
                  (sr & 0x2000) ? 'S' : 'U', (sr >> 8) & 7, (sr & 0x10) ? 'X' : '-', (sr & 0x08) ? 'N' : '-',
                  (sr & 0x04) ? 'Z' : '-', (sr & 0x02) ? 'V' : '-', (sr & 0x01) ? 'C' : '-');
    reg("SR", zcdi::CpuReg::Sr, sr, paused && regs.sr != before_.sr, paused, sr_text);
    ImGui::Separator();
    for (int i = 0; i < 8; ++i) {
        char d_name[4] = {'D', static_cast<char>('0' + i), 0, 0};
        char a_name[4] = {'A', static_cast<char>('0' + i), 0, 0};
        reg(d_name, zcdi::data_reg(unsigned(i)), regs.d[i], paused && regs.d[i] != before_.d[i], paused);
        ImGui::SameLine(0.0f, 24.0f);
        reg(a_name, zcdi::address_reg(unsigned(i)), regs.a[i], paused && regs.a[i] != before_.a[i], paused);
    }
    ImGui::Separator();
    reg("USP", zcdi::CpuReg::Usp, regs.usp, paused && regs.usp != before_.usp, paused);
    ImGui::SameLine(0.0f, 24.0f);
    reg("ISP", zcdi::CpuReg::Isp, regs.isp, paused && regs.isp != before_.isp, paused);
    if (ImGui::BeginPopup(kEditPopup)) {
        ImGui::SetNextItemWidth(hex_input_width());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter =
            ImGui::InputScalar("##value", ImGuiDataType_U32, &edit_value_, nullptr, nullptr, "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter && paused && editing_) {
            const_cast<zcdi::MusashiCore&>(view.cpu()).set_reg(*editing_, edit_value_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const std::string where = state.symbols.where(regs.pc);
    if (!where.empty()) ImGui::TextDisabled("PC in %s", where.c_str());
    ImGui::End();
}
