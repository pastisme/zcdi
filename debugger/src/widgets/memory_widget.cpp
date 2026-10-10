#include "widgets/memory_widget.h"

#include "core/machine_inspector.h"
#include "imgui/imgui.h"

MemoryWidget::MemoryWidget() : DebuggerWidget("Memory") {
    memory_editor_.OptShowDataPreview = true;
    memory_editor_.OptShowAscii = true;
    memory_editor_.OptShowHexII = false;
    memory_editor_.ReadOnly = true;
}

void MemoryWidget::draw(AppState& state) {
    if (!begin_machine_window(state)) return;

    const zcdi::MachineInspector view(*state.machine);
    // Writes go straight into the machine; only while paused.
    memory_editor_.ReadOnly = state.running;

    auto draw_span = [this](const char* empty_label, std::span<const std::byte> data) {
        if (data.empty()) {
            ImGui::TextUnformatted(empty_label);
            return;
        }
        memory_editor_.DrawContents(const_cast<std::byte*>(data.data()), data.size(), 0);
    };

    if (ImGui::BeginTabBar("memory_tabs")) {
        if (ImGui::BeginTabItem("NVRAM")) {
            draw_span("NVRAM unavailable", view.nvram_data());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Plane 0")) {
            draw_span("Plane 0 unavailable", view.plane0_data());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Plane 1")) {
            draw_span("Plane 1 unavailable", view.plane1_data());
            ImGui::EndTabItem();
        }

        if (!view.dvc_cart_ram_data().empty() && ImGui::BeginTabItem("DVC RAM")) {
            draw_span("DVC RAM unavailable", view.dvc_cart_ram_data());
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}
