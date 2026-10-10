#include "widgets/mcd212_widget.h"

#include "core/machine_inspector.h"
#include "imgui/imgui.h"
#include "mcd212/mcd212.h"
#include "stream_texture.h"
#include "widgets/widget_ui.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>

namespace {

using zcdi::mamecdi::Mcd212SaveState;

constexpr int kDcaBudgetBytes = 64; // per line, as Mcd212::process_dca
constexpr int kMaxIcaCommands = 1920;
constexpr uint32_t kIcaStart = 0x400;
constexpr ImVec4 kCurrentLine{1.0f, 0.75f, 0.3f, 1.0f};

uint32_t read_be32(std::span<const std::byte> ram, uint32_t off) {
    if (off + 4 > ram.size()) return 0;
    return (uint32_t(ram[off]) << 24) | (uint32_t(ram[off + 1]) << 16) | (uint32_t(ram[off + 2]) << 8) |
           uint32_t(ram[off + 3]);
}

const char* register_name(uint8_t reg) {
    switch (reg) {
        case 0xc0: return "ICM";
        case 0xc1: return "TRANSPARENCY";
        case 0xc2: return "PLANE ORDER";
        case 0xc3: return "CLUT BANK";
        case 0xc4: return "TRANSP COLOR A";
        case 0xc6: return "TRANSP COLOR B";
        case 0xc7: return "MASK COLOR A";
        case 0xc9: return "MASK COLOR B";
        case 0xca: return "DYUV START A";
        case 0xcb: return "DYUV START B";
        case 0xcd: return "CURSOR POS";
        case 0xce: return "CURSOR CTRL";
        case 0xcf: return "CURSOR PATTERN";
        case 0xd8: return "BACKDROP";
        case 0xd9: return "MOSAIC A";
        case 0xda: return "MOSAIC B";
        case 0xdb: return "WEIGHT A";
        case 0xdc: return "WEIGHT B";
        default: break;
    }
    if (reg >= 0xd0 && reg <= 0xd7) return "REGION";
    return nullptr;
}

// One display control instruction, as process_ica / process_dca read it.
std::string describe(uint32_t cmd, bool ica) {
    const uint8_t op = uint8_t(cmd >> 24);
    const uint32_t arg = cmd & 0x00ffffffu;
    char text[96]{};
    if (op <= 0x0f) return "STOP";
    if (op <= 0x1f) return "NOP";
    if (op <= 0x2f) {
        std::snprintf(text, sizeof(text), ica ? "RELOAD DCP %06X" : "RELOAD DCP %06X (ignored in DCA)",
                      static_cast<unsigned>(arg & 0x3ffffcu));
    } else if (op <= 0x3f) {
        if (ica) {
            std::snprintf(text, sizeof(text), "RELOAD DCP %06X + STOP", static_cast<unsigned>(arg & 0x3ffffcu));
        } else {
            std::snprintf(text, sizeof(text), "RELOAD DCP %06X, DCA jumps to %05X",
                          static_cast<unsigned>(arg & 0x3ffffcu), static_cast<unsigned>(arg & 0x7fffcu));
        }
    } else if (op <= 0x4f) {
        if (ica) {
            std::snprintf(text, sizeof(text), "JUMP %05X", static_cast<unsigned>((cmd & 0x7ffffu) & ~1u));
        } else {
            std::snprintf(text, sizeof(text), "RELOAD VSR %06X", static_cast<unsigned>(arg & 0x3fffffu));
        }
    } else if (op <= 0x5f) {
        std::snprintf(text, sizeof(text), "RELOAD VSR %06X + STOP", static_cast<unsigned>(arg & 0x3fffffu));
    } else if (op <= 0x6f) {
        return "INTERRUPT";
    } else if (op <= 0x77) {
        return "undefined";
    } else if (op <= 0x7f) {
        std::snprintf(text, sizeof(text), "DISPLAY PARAMETERS %02X", static_cast<unsigned>(arg & 0x1fu));
    } else if (op <= 0xbf) {
        std::snprintf(text, sizeof(text), "CLUT bank+%02X = %06X", static_cast<unsigned>(op - 0x80),
                      static_cast<unsigned>(arg & 0xfcfcfcu));
    } else if (const char* name = register_name(op)) {
        std::snprintf(text, sizeof(text), "%s (%02X) = %06X", name, static_cast<unsigned>(op),
                      static_cast<unsigned>(arg));
    } else {
        std::snprintf(text, sizeof(text), "register %02X = %06X", static_cast<unsigned>(op),
                      static_cast<unsigned>(arg));
    }
    return text;
}

const char* coding_name(uint8_t icm, int path, uint8_t other) {
    if (path == 1 && icm == 0x1 && other == 0x0) return "RGB555";
    switch (icm) {
        case 0x0: return "OFF";
        case 0x1: return "CLUT8";
        case 0x3: return "CLUT7";
        case 0x4: return "CLUT7+7";
        case 0x5: return "DYUV";
        case 0xb: return "CLUT4 / RL3";
        default: return "?";
    }
}

void row(const char* name, const char* a, const char* b) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(a);
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(b);
}

void row_hex(const char* name, unsigned a, unsigned b, int digits) {
    char ta[16]{};
    char tb[16]{};
    std::snprintf(ta, sizeof(ta), "%0*X", digits, a);
    std::snprintf(tb, sizeof(tb), "%0*X", digits, b);
    row(name, ta, tb);
}

std::string dcr_flags(uint16_t dcr) {
    // DE CF FD SM CM | IC DC
    std::string s;
    if (dcr & 0x8000) s += "DE ";
    if (dcr & 0x4000) s += "CF ";
    if (dcr & 0x2000) s += "FD ";
    if (dcr & 0x1000) s += "SM ";
    if (dcr & 0x0800) s += "CM ";
    if (dcr & 0x0200) s += "IC ";
    if (dcr & 0x0100) s += "DC ";
    return s.empty() ? "-" : s;
}

void draw_registers(const Mcd212SaveState& r, const zcdi::mamecdi::Mcd212& chip) {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("paths", 3, flags)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Path A (plane 0)");
        ImGui::TableSetupColumn("Path B (plane 1)");
        ImGui::TableHeadersRow();
        row_hex("CSR W", r.csrw_[0], r.csrw_[1], 4);
        row_hex("CSR R", r.csr1r_, r.csr2r_, 2);
        row_hex("DCR", r.dcr_[0], r.dcr_[1], 4);
        row("  flags", dcr_flags(r.dcr_[0]).c_str(), dcr_flags(r.dcr_[1]).c_str());
        row_hex("VSR", r.vsr_[0], r.vsr_[1], 4);
        row_hex("DDR", r.ddr_[0], r.ddr_[1], 4);
        row_hex("DCP", ((r.ddr_[0] & 0x3fu) << 16) | r.dcp_[0], ((r.ddr_[1] & 0x3fu) << 16) | r.dcp_[1], 6);
        row_hex("DCA", r.dca_[0], r.dca_[1], 6);
        row("Coding", coding_name(r.icm_[0], 0, r.icm_[1]), coding_name(r.icm_[1], 1, r.icm_[0]));
        row_hex("Transparency", r.transparency_control_ & 0xfu, (r.transparency_control_ >> 8) & 0xfu, 1);
        row_hex("Transp color", r.transparent_color_[0], r.transparent_color_[1], 6);
        row_hex("Mask color", r.mask_color_[0], r.mask_color_[1], 6);
        row_hex("CLUT bank", r.clut_bank_[0], r.clut_bank_[1], 1);
        row_hex("DYUV start", r.dyuv_abs_start_[0], r.dyuv_abs_start_[1], 6);
        row_hex("Mosaic/hold", r.pixel_hold_[0], r.pixel_hold_[1], 6);
        row_hex("Weight", r.weight_factor_[0], r.weight_factor_[1], 2);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::Text("ICM %s%s%s  plane order %s  backdrop %X  IRQ1 %s", r.icm_nr_ ? "NR " : "", r.icm_ev_ ? "EV " : "",
                r.icm_cs_ ? "CS " : "", (r.plane_order_ & 1) ? "B over A" : "A over B",
                static_cast<unsigned>(r.backdrop_color_), r.irq1_ ? "on" : "off");
    ImGui::Text("Cursor pos %06X ctrl %06X", static_cast<unsigned>(r.cursor_position_),
                static_cast<unsigned>(r.cursor_control_));
    ImGui::Text("Active %dx%d from field line %d", chip.active_width(), chip.active_height(), chip.first_active_line());
    if (ImGui::TreeNode("Region control")) {
        for (int i = 0; i < 8; ++i)
            ImGui::Text("%d: %06X", i, static_cast<unsigned>(r.region_control_[i]));
        ImGui::TreePop();
    }
}

// Where the field's picture starts: the RELOAD VSR + STOP the ICA ends on.
bool ica_start_vsr(std::span<const std::byte> ram, uint32_t& vsr) {
    uint32_t off = kIcaStart;
    for (int i = 0; i < kMaxIcaCommands && off + 4 <= ram.size(); ++i) {
        const uint32_t cmd = read_be32(ram, off);
        const uint8_t op = uint8_t(cmd >> 24);
        if (op >= 0x50 && op <= 0x5f) {
            vsr = cmd & 0x3fffffu;
            return true;
        }
        if (op <= 0x0f || (op >= 0x30 && op <= 0x3f)) return false;
        off = (op >= 0x40 && op <= 0x4f) ? ((cmd & 0x7ffffu) & ~1u) : off + 4;
    }
    return false;
}

void draw_ica(std::span<const std::byte> ram) {
    uint32_t off = kIcaStart;
    for (int i = 0; i < kMaxIcaCommands && off + 4 <= ram.size(); ++i) {
        const uint32_t cmd = read_be32(ram, off);
        const uint8_t op = uint8_t(cmd >> 24);
        ImGui::Text("%05X  %08X  %s", static_cast<unsigned>(off), static_cast<unsigned>(cmd),
                    describe(cmd, true).c_str());
        if (op <= 0x0f || (op >= 0x30 && op <= 0x3f) || (op >= 0x50 && op <= 0x5f)) break;
        off = (op >= 0x40 && op <= 0x4f) ? ((cmd & 0x7ffffu) & ~1u) : off + 4;
    }
}

// The field's DCA lists from DCP, as process_dca walks them; RAM as it is now.
void draw_dca(std::span<const std::byte> ram, uint32_t dcp, int lines, int current, bool hide_idle) {
    if (ram.empty()) return;
    const uint32_t mask = static_cast<uint32_t>(ram.size()) - 1;
    uint32_t dca = dcp;
    for (int y = 0; y < lines; ++y) {
        uint32_t off = dca & mask;
        uint32_t next = off + kDcaBudgetBytes;
        std::string body;
        bool idle = true;
        for (int used = 0; used < kDcaBudgetBytes; used += 4) {
            const uint32_t cmd = read_be32(ram, off);
            const uint8_t op = uint8_t(cmd >> 24);
            char line[128]{};
            std::snprintf(line, sizeof(line), "  %05X  %08X  %s\n", static_cast<unsigned>(off),
                          static_cast<unsigned>(cmd), describe(cmd, false).c_str());
            body += line;
            off += 4;
            if (op <= 0x0f) break;
            idle = idle && op <= 0x1f;
            if (op >= 0x30 && op <= 0x3f) {
                next = cmd & 0x7fffcu;
                break;
            }
            if (op >= 0x50 && op <= 0x5f) break;
        }
        dca = next;
        if (idle && hide_idle && y != current) continue;
        if (y == current) {
            ImGui::TextColored(kCurrentLine, "line %d (current)", y);
        } else {
            ImGui::TextDisabled("line %d", y);
        }
        ImGui::TextUnformatted(body.c_str());
    }
}

void draw_clut(const Mcd212SaveState& r) {
    const float size = ImGui::GetFrameHeight();
    for (int i = 0; i < 256; ++i) {
        if (i % 64 == 0)
            ImGui::TextDisabled("Bank %d%s%s", i / 64, r.clut_bank_[0] == i / 64 ? "  (A)" : "",
                                r.clut_bank_[1] == i / 64 ? "  (B)" : "");
        const uint32_t c = r.clut_[i];
        const ImVec4 color(float((c >> 16) & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f, float(c & 0xff) / 255.0f,
                           1.0f);
        char id[16]{};
        std::snprintf(id, sizeof(id), "%02X  %06X", i, static_cast<unsigned>(c));
        ImGui::ColorButton(id, color, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker, ImVec2(size, size));
        if (i % 16 != 15) ImGui::SameLine(0.0f, 2.0f);
    }
}

} // namespace

void Mcd212Widget::draw(AppState& state) {
    if (!begin_machine_window(state)) return;
    const zcdi::MachineInspector view(*state.machine);
    const zcdi::mamecdi::Mcd212& chip = view.mcd212();
    const Mcd212SaveState& r = chip.state();
    // Mid-frame, the active line run_frame() is on.
    const int current = state.runner && state.runner->mid_frame() ? view.line() - chip.first_active_line() : -1;

    if (ImGui::BeginTabBar("mcd212_tabs")) {
        if (ImGui::BeginTabItem("Registers")) {
            draw_registers(r, chip);
            ImGui::EndTabItem();
        }
        for (int path = 0; path < 2; ++path) {
            const std::span<const std::byte> ram = path == 0 ? view.plane0_data() : view.plane1_data();
            const uint16_t dcr = r.dcr_[path];
            const uint32_t dcp = ((uint32_t(r.ddr_[path]) & 0x3fu) << 16) | r.dcp_[path];
            if (ImGui::BeginTabItem(path == 0 ? "ICA A" : "ICA B")) {
                if (!(dcr & 0x0200)) ImGui::TextDisabled("DCR.IC off: the ICA doesn't run");
                ImGui::BeginChild("ica");
                draw_ica(ram);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(path == 0 ? "DCA A" : "DCA B")) {
                if (!(dcr & 0x0100)) ImGui::TextDisabled("DCR.DC off: the DCA doesn't run");
                ImGui::Checkbox("Hide lines with no work", &hide_idle_lines_);
                ImGui::SameLine();
                ImGui::TextDisabled("from DCP %06X", static_cast<unsigned>(dcp));
                ImGui::BeginChild("dca");
                draw_dca(ram, dcp, chip.active_height(), current, hide_idle_lines_);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("CLUT")) {
            draw_clut(r);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Planes")) {
            draw_planes(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void Mcd212Widget::release() { texture_.release(); }

// The plane through Mcd212::preview_plane.
void Mcd212Widget::draw_planes(AppState& state) {
    const zcdi::MachineInspector view(*state.machine);
    const zcdi::mamecdi::Mcd212& chip = view.mcd212();
    const Mcd212SaveState& r = chip.state();
    constexpr int kWidth = Mcd212SaveState::kFbWidth;
    constexpr int kHeight = Mcd212SaveState::kFbHeight;

    ImGui::RadioButton("Plane A", &plane_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Plane B", &plane_, 1);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", coding_name(r.icm_[plane_], plane_, r.icm_[plane_ ^ 1]));
    const std::span<const std::byte> ram = plane_ == 0 ? view.plane0_data() : view.plane1_data();
    ImGui::Checkbox("Start at the ICA's VSR", &start_from_ica_);
    uint32_t ica_vsr = 0;
    const bool have_ica_vsr = ica_start_vsr(ram, ica_vsr);
    if (start_from_ica_) start_ = have_ica_vsr ? ica_vsr : 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(hex_input_width());
    ImGui::BeginDisabled(start_from_ica_);
    ImGui::InputScalar("start", ImGuiDataType_U32, &start_, nullptr, nullptr, "%06X",
                       ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::EndDisabled();
    if (start_from_ica_ && !have_ica_vsr) ImGui::TextDisabled("The ICA loads no VSR: starting at 0");

    if (!texture_.valid()) texture_.create(kWidth, kHeight, "zcdi-plane-preview");
    const int lines = chip.active_height();
    pixels_.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
    rgba_.resize(pixels_.size());
    if (!ram.empty() && lines > 0) {
        chip.preview_plane(plane_, start_, lines, reinterpret_cast<const uint8_t*>(ram.data()), ram.size(),
                           pixels_.data());
    }
    for (std::size_t i = 0; i < pixels_.size(); ++i)
        rgba_[i] = xrgb_to_rgba(pixels_[i]);
    texture_.update(rgba_);

    // The active area at 4:3, as the Rendering window shows the screen.
    const ImVec2 uv1(static_cast<float>(chip.active_width()) / kWidth,
                     static_cast<float>(std::max(lines, 1)) / kHeight);
    ImGui::Image(texture_.id(), fit_aspect(ImGui::GetContentRegionAvail()), ImVec2(0.0f, 0.0f), uv1);
}
