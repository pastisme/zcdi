#include "widgets/disassembly_widget.h"

#include "core/machine_inspector.h"
#include "imgui/imgui.h"
#include "names.h"
#include "widgets/widget_ui.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr ImU32 kPcRow = IM_COL32(110, 95, 20, 160);
constexpr ImU32 kCursorRow = IM_COL32(60, 80, 120, 120);
constexpr ImU32 kBreakpointOn = IM_COL32(220, 50, 50, 255);
constexpr ImU32 kBreakpointOff = IM_COL32(140, 90, 90, 255);

// Musashi's line, but an OS-9 TRAP includes its function word.
zcdi::MachineInspector::DisassemblyLine decode(const zcdi::MachineInspector& view, uint32_t addr) {
    zcdi::MachineInspector::DisassemblyLine line = view.disassemble(addr);
    const uint16_t op = view.peek16(addr);
    if (!line.valid || !is_trap_opcode(op)) return line;
    const uint16_t code = view.peek16(addr + 2);
    const char* call = (op & 0xf) == 0 ? os9_call_name(code) : nullptr;
    line.size = 4;
    std::snprintf(line.bytes, sizeof(line.bytes), "%02X %02X %02X %02X", op >> 8, op & 0xff, code >> 8, code & 0xff);
    if (call) {
        std::snprintf(line.text, sizeof(line.text), "os9     %s", call);
    } else {
        std::snprintf(line.text, sizeof(line.text), "trap    #%d, $%04X", op & 0xf, code);
    }
    return line;
}

void toggle_breakpoint(AppState& state, uint32_t addr) {
    auto& bps = state.runner->breakpoints;
    if (bps.erase(addr) == 0) bps[addr] = true;
}

} // namespace

uint32_t DisassemblyWidget::back(const zcdi::MachineInspector& view, uint32_t addr, int lines) {
    for (uint32_t span = 64; span >= 2; span -= 2) {
        if (span > addr) continue;
        std::vector<uint32_t> starts;
        uint32_t a = addr - span;
        while (a < addr) {
            const zcdi::MachineInspector::DisassemblyLine line = decode(view, a);
            if (!line.valid) break;
            starts.push_back(a);
            a += line.size;
        }
        if (a != addr || starts.empty()) continue;
        const std::size_t n = static_cast<std::size_t>(lines);
        return starts.size() >= n ? starts[starts.size() - n] : starts.front();
    }
    const uint32_t step = 2u * static_cast<uint32_t>(lines);
    return addr >= step ? addr - step : 0;
}

void DisassemblyWidget::draw(AppState& state) {
    ImGui::Begin(title(), &open);
    if (!state.machine || !state.runner) {
        ImGui::TextUnformatted("No machine");
        ImGui::End();
        return;
    }
    const zcdi::MachineInspector view(*state.machine);
    const uint32_t pc = view.pc();

    ImGui::Checkbox("Follow PC", &follow_pc_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(hex_input_width());
    if (ImGui::InputTextWithHint("##goto", "go to", goto_text_, sizeof(goto_text_),
                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
        state.disasm_goto = static_cast<uint32_t>(std::strtoul(goto_text_, nullptr, 16));
    }
    ImGui::SameLine();
    if (ImGui::Button("PC")) {
        follow_pc_ = true;
        cursor_ = pc;
    }
    if (state.disasm_goto) {
        anchor_ = cursor_ = *state.disasm_goto & ~1u;
        follow_pc_ = false;
        state.disasm_goto.reset();
    }

    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const int rows = static_cast<int>(ImGui::GetContentRegionAvail().y / row_h);
    if (rows <= 0) {
        ImGui::End();
        return;
    }

    // The addresses on screen; entry headers take a row each.
    auto layout = [&](uint32_t from, std::vector<uint32_t>& out) {
        out.clear();
        uint32_t a = from;
        int used = 0;
        while (used < rows) {
            if (state.symbols.entry(a) && used > 0) ++used;
            if (used >= rows) break;
            out.push_back(a);
            ++used;
            const zcdi::MachineInspector::DisassemblyLine line = decode(view, a);
            a += line.size ? line.size : 2;
        }
    };
    std::vector<uint32_t> shown;
    layout(anchor_, shown);
    if (follow_pc_) {
        bool visible = false;
        for (std::size_t i = 0; i < shown.size(); ++i) {
            if (shown[i] == pc && i + 3 < shown.size()) visible = true;
        }
        if (!visible) {
            anchor_ = back(view, pc, rows > 8 ? 4 : 1);
            layout(anchor_, shown);
        }
    }

    // Scrolling moves the listing by instructions and stops following.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel < 0.0f && shown.size() > 3) {
            anchor_ = shown[3];
            follow_pc_ = false;
        } else if (wheel > 0.0f) {
            anchor_ = back(view, anchor_, 3);
            follow_pc_ = false;
        }
    }
    if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        toggle_breakpoint(state, cursor_);
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoHostExtendY;
    if (ImGui::BeginTable("listing", 6, flags)) {
        ImGui::TableSetupColumn("bp", ImGuiTableColumnFlags_WidthFixed, row_h);
        ImGui::TableSetupColumn("addr");
        ImGui::TableSetupColumn("where");
        ImGui::TableSetupColumn("bytes");
        ImGui::TableSetupColumn("code");
        ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthStretch);
        for (const uint32_t addr : shown) {
            if (const std::string* entry = state.symbols.entry(addr)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("; %s", entry->c_str());
            }
            const zcdi::MachineInspector::DisassemblyLine line = decode(view, addr);
            ImGui::PushID(static_cast<int>(addr));
            ImGui::TableNextRow();
            if (addr == pc) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, kPcRow);
            } else if (addr == cursor_) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, kCursorRow);
            }

            ImGui::TableSetColumnIndex(1);
            char addr_text[16]{};
            std::snprintf(addr_text, sizeof(addr_text), "%06X", static_cast<unsigned>(addr & 0x00ffffffu));
            if (ImGui::Selectable(addr_text, false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                cursor_ = addr;
            }
            if (ImGui::BeginPopupContextItem("line")) {
                cursor_ = addr;
                if (ImGui::MenuItem("Run to here", nullptr, false, !state.running)) {
                    state.command = RunCommand::kRunTo;
                    state.command_target = addr;
                }
                if (ImGui::MenuItem("Toggle breakpoint", "F9")) toggle_breakpoint(state, addr);
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(0);
            const auto bp = state.runner->breakpoints.find(addr);
            if (bp != state.runner->breakpoints.end()) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float r = row_h * 0.3f;
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + row_h * 0.5f, p.y + row_h * 0.45f), r,
                                                            bp->second ? kBreakpointOn : kBreakpointOff);
            }
            if (ImGui::InvisibleButton("bp", ImVec2(row_h, ImGui::GetTextLineHeight()))) toggle_breakpoint(state, addr);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", state.symbols.where(addr).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", line.bytes);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(line.text);
            ImGui::TableSetColumnIndex(5);
            if (const std::string* note = state.symbols.note(addr)) {
                // The first of the joined notes; all of them on hover.
                const std::size_t cut = note->find(" / ");
                ImGui::TextDisabled("; %s", note->substr(0, cut).c_str());
                if (ImGui::IsItemHovered() && cut != std::string::npos) {
                    std::string all = *note;
                    for (std::size_t at = all.find(" / "); at != std::string::npos; at = all.find(" / ", at)) {
                        all.replace(at, 3, "\n");
                    }
                    ImGui::SetTooltip("%s", all.c_str());
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
