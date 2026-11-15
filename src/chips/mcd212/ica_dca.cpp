#include "chips/mcd212/mcd212.h"

namespace zcdi {
namespace chips {

using namespace mcd212;

namespace {
uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// ICA's plain RELOAD VSR only moves the walker. The low 19 bits are a word
// index, so the byte offset is the masked value with bit 0 cleared.
uint32_t reload_vsr_jump_target(uint32_t cmd) { return (cmd & 0x0007ffffu) & ~1u; }

// RELOAD DISPLAY PARAMETERS: bits 0-3 to
// DDR bits 8-11 (file type), bit 4 to DCR bit 11.
void apply_display_parameters(uint16_t& ddr_reg, uint16_t& dcr_reg, uint8_t value) {
    ddr_reg = uint16_t((ddr_reg & 0xf0ffu) | (uint16_t(value & 0x0fu) << 8));
    dcr_reg = uint16_t((dcr_reg & 0xf7ffu) | (uint16_t(value & 0x10u) << 7));
}
} // namespace

void Mcd212::process_ica(int path, const uint8_t* plane_ram, std::size_t plane_size) {
    // ICA starts at byte 0x400, or 0x404 in even interlaced fields.
    uint32_t byte_off = (dcr_[0] & kDcr1Sm) && even_field_ ? kIcaStartEvenField : kIcaStart;
    for (int i = 0; i < kIcaMaxCommands; ++i) {
        if (byte_off + 4 > plane_size) break;
        uint32_t cmd = read_be32(plane_ram + byte_off);
        uint8_t op = uint8_t(cmd >> 24);
        uint32_t arg = cmd & 0x00ffffffu;

        if (op < kCmdNop) break; // STOP
        if (op < kCmdReloadDcp) {
            byte_off += 4;
            continue;
        } // NOP
        if (op < kCmdReloadDcpStop) {
            set_dcp_address(path, arg & 0x3ffffcu);
            byte_off += 4;
            continue;
        }
        if (op < kCmdReloadVsr) {
            set_dcp_address(path, arg & 0x3ffffcu);
            break;
        }
        if (op < kCmdReloadVsrStop) {
            byte_off = reload_vsr_jump_target(cmd);
            continue;
        } // jump only, no register touched
        if (op < kCmdInterrupt) {
            set_vsr_address(path, arg);
            break;
        } // sets the real register
        if (op < kCmdUndefined) {
            // Both paths' flags live in CSR2R.
            csr2r_ |= (path == 0) ? kCsr2rIt1 : kCsr2rIt2;
            if (!(csrw_[path] & kCsrWDi)) irq1_ = true;
            byte_off += 4;
            continue;
        }
        if (op >= kCmdReloadDisplayParams && op < kCmdSetRegister) {
            apply_display_parameters(ddr_[path], dcr_[path], uint8_t(arg & 0x1fu));
            byte_off += 4;
            continue;
        }
        if (op >= kCmdSetRegister) {
            set_register(path, op, arg);
            byte_off += 4;
            continue;
        }
        byte_off += 4; // kCmdUndefined
    }
}

void Mcd212::process_dca(int path, const uint8_t* plane_ram, std::size_t plane_size) {
    // DCP can hold 22 bits; masked into the installed VRAM.
    uint32_t byte_off = dca_[path] & (uint32_t(plane_size) - 1);
    int consumed = 0;
    bool processing = true;

    while (processing && consumed < kDcaBudgetBytes) {
        if (byte_off + 4 > plane_size) break;
        uint32_t cmd = read_be32(plane_ram + byte_off);
        uint8_t op = uint8_t(cmd >> 24);
        uint32_t arg = cmd & 0x00ffffffu;
        byte_off += 4;
        consumed += 4; // STOP counts too

        if (op < kCmdNop) {
            processing = false;
            continue;
        } // STOP
        if (op < kCmdReloadDcp) continue;
        if (op < kCmdReloadDcpStop) continue; // meaningless mid-DCA, ignored
        if (op < kCmdReloadVsr) {
            // Jumps DCA at once, skipping the budget advance; narrower mask
            // than DCP's.
            set_dcp_address(path, arg & 0x3ffffcu);
            dca_[path] = arg & 0x0007fffcu;
            return;
        }
        // In DCA both RELOAD VSR forms set VSR.
        if (op < kCmdReloadVsrStop) {
            set_vsr_address(path, arg);
            continue;
        }
        if (op < kCmdInterrupt) {
            set_vsr_address(path, arg);
            processing = false;
            continue;
        }
        if (op < kCmdUndefined) {
            csr2r_ |= (path == 0) ? kCsr2rIt1 : kCsr2rIt2;
            if (!(csrw_[path] & kCsrWDi)) irq1_ = true;
            continue;
        }
        if (op >= kCmdSetRegister) {
            set_register(path, op, arg);
            continue;
        }
        if (op >= kCmdReloadDisplayParams) {
            apply_display_parameters(ddr_[path], dcr_[path], uint8_t(arg & 0x1fu));
            continue;
        }
        // kCmdUndefined: no-op
    }

    // The whole 64-byte budget is consumed wherever STOP fell.
    byte_off += (kDcaBudgetBytes - consumed);
    dca_[path] = byte_off;
}

} // namespace chips
} // namespace zcdi
