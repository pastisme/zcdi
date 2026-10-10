#include "widgets/stack_widget.h"

#include "core/machine_inspector.h"
#include "imgui/imgui.h"
#include "widgets/widget_ui.h"

#include <string>

namespace {

constexpr int kStackWords = 48;

} // namespace

// Words from SP; longs pointing into a module are likely return addresses.
void StackWidget::draw(AppState& state) {
    if (!begin_machine_window(state)) return;
    const zcdi::MachineInspector view(*state.machine);
    const uint32_t sp = view.sp();
    ImGui::Text("SP %08X", static_cast<unsigned>(sp));
    ImGui::Separator();

    if (ImGui::BeginTable("stack_words", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
        for (int i = 0; i < kStackWords; ++i) {
            const uint32_t addr = sp + static_cast<uint32_t>(i * 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%08X", static_cast<unsigned>(addr));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%04X", static_cast<unsigned>(view.peek16(addr)));
            ImGui::TableSetColumnIndex(2);
            const uint32_t value = view.peek32(addr);
            if (!state.symbols.where(value).empty()) address_cell(state, value, i);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
