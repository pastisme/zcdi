#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace zcdi {

class Machine;
class MusashiCore;
namespace mamecdi {
class Cdic;
class Mcd212;
class Scc68070Periph;
class SlaveHle;
class VmpegDevice;
} // namespace mamecdi
namespace dvc {
class DvcVideo;
}

// Read access to a machine's internals for debuggers, tracers and tests.
// What it returns is const, but backed by the live, writable machine: a
// caller that knows what it is doing may const_cast and change it.
class MachineInspector {
public:
    struct CpuRegisters {
        uint32_t d[8]{};
        uint32_t a[8]{};
        uint32_t pc = 0;
        uint16_t sr = 0;
        // Both stack banks; a[7] is the one SR.S selects.
        uint32_t usp = 0;
        uint32_t isp = 0;
    };

    struct DisassemblyLine {
        static constexpr std::size_t kBytesCapacity = 96;
        static constexpr std::size_t kTextCapacity = 256;

        uint32_t pc = 0;
        uint32_t size = 0;
        char bytes[kBytesCapacity]{};
        char text[kTextCapacity]{};
        bool valid = false;
    };

    explicit MachineInspector(const Machine& machine) : machine_(machine) {}

    // Register writes (debuggers): const_cast, between run_frame() calls.
    const MusashiCore& cpu() const;
    uint32_t pc() const;
    uint32_t sp() const;
    CpuRegisters registers() const;
    DisassemblyLine disassemble(uint32_t pc) const;
    // Memory only, no side effects; 0xff for registers.
    uint8_t peek8(uint32_t addr) const;
    uint16_t peek16(uint32_t addr) const;
    uint32_t peek32(uint32_t addr) const;

    // Machine's field counter: saved in states, restarted by cold boot/reset.
    int frame() const;
    // run_frame()'s current line; lines per field between frames.
    int line() const;

    std::span<const std::byte> nvram_data() const;
    // Main RAM, in emulated address order (big-endian words): the two
    // 512 KB video planes at 0x000000 and 0x200000, and the DVC cartridge's
    // 1 MB RAM at 0xd00000 (empty without a cartridge).
    std::span<const std::byte> plane0_data() const;
    std::span<const std::byte> plane1_data() const;
    std::span<const std::byte> dvc_cart_ram_data() const;
    // The chips, read-only.
    const mamecdi::Mcd212& mcd212() const;
    const mamecdi::Scc68070Periph& scc68070() const;
    const mamecdi::SlaveHle& slave() const;
    const mamecdi::Cdic& cdic() const;
    const mamecdi::VmpegDevice& vmpeg() const;
    const dvc::DvcVideo& dvc_video() const;

private:
    const Machine& machine_;
};

} // namespace zcdi
