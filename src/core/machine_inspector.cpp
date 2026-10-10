#include "core/machine_inspector.h"

#include "core/bus.h"
#include "core/machine.h"

#include <cstdio>

namespace zcdi {

const MusashiCore& MachineInspector::cpu() const { return *machine_.cpu_; }

uint32_t MachineInspector::pc() const { return machine_.cpu_->pc(); }

uint32_t MachineInspector::sp() const { return machine_.cpu_->sp(); }

MachineInspector::CpuRegisters MachineInspector::registers() const {
    CpuRegisters regs{
        .pc = machine_.cpu_->pc(),
        .sr = machine_.cpu_->sr(),
        .usp = machine_.cpu_->usp(),
        .isp = machine_.cpu_->isp(),
    };
    for (unsigned i = 0; i < 8; ++i) {
        regs.d[i] = machine_.cpu_->d(i);
        regs.a[i] = machine_.cpu_->a(i);
    }
    return regs;
}

MachineInspector::DisassemblyLine MachineInspector::disassemble(uint32_t addr) const {
    DisassemblyLine line{};
    line.pc = addr;

    line.size = machine_.cpu_->disassemble(line.text, addr);
    if (line.size == 0) {
        std::snprintf(line.text, sizeof(line.text), "?");
        line.valid = false;
        return line;
    }

    std::size_t write_off = 0;
    for (uint32_t i = 0; i < line.size; ++i) {
        const uint8_t byte = machine_.bus_.peek8(addr + i);
        int wrote = std::snprintf(line.bytes + write_off, sizeof(line.bytes) - write_off, "%02X%s",
                                  static_cast<unsigned>(byte), (i + 1u < line.size) ? " " : "");
        if (wrote <= 0) break;
        write_off += static_cast<std::size_t>(wrote);
        if (write_off >= sizeof(line.bytes)) break;
    }

    line.valid = true;
    return line;
}

uint8_t MachineInspector::peek8(uint32_t addr) const { return machine_.bus_.peek8(addr); }

uint16_t MachineInspector::peek16(uint32_t addr) const { return machine_.bus_.peek16(addr); }

uint32_t MachineInspector::peek32(uint32_t addr) const { return machine_.bus_.peek32(addr); }

int MachineInspector::frame() const { return machine_.frame_count_; }

const mamecdi::Mcd212& MachineInspector::mcd212() const { return machine_.bus_.mcd212_; }

const mamecdi::Scc68070Periph& MachineInspector::scc68070() const { return machine_.bus_.scc_; }

const mamecdi::SlaveHle& MachineInspector::slave() const { return machine_.bus_.slave_; }

const mamecdi::Cdic& MachineInspector::cdic() const { return machine_.bus_.cdic_; }

const mamecdi::VmpegDevice& MachineInspector::vmpeg() const { return machine_.bus_.vmpeg_; }

const dvc::DvcVideo& MachineInspector::dvc_video() const { return machine_.bus_.dvc_video_; }

int MachineInspector::line() const { return machine_.line_; }

std::span<const std::byte> MachineInspector::nvram_data() const { return machine_.bus_.nvram_data(); }

std::span<const std::byte> MachineInspector::plane0_data() const { return machine_.bus_.plane0_data(); }

std::span<const std::byte> MachineInspector::plane1_data() const { return machine_.bus_.plane1_data(); }

std::span<const std::byte> MachineInspector::dvc_cart_ram_data() const {
    return machine_.bus_.dvc_cart_present() ? machine_.bus_.dvc_ram_data() : std::span<const std::byte>{};
}

} // namespace zcdi
