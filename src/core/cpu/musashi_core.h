#pragma once
#include <cstdint>

namespace zcdi {

class Bus;

// Musashi's m68k_register_t values (static_asserted in musashi_core.cpp),
// SCC68070-relevant ones only.
enum class CpuReg : int {
    D0 = 0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7,
    A0 = 8,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    A7,
    Pc = 16,
    Sr = 17,
    Sp = 18, // the current stack, A7
    Usp = 19,
    Isp = 20,
    Ppc = 29, // the last instruction's PC
    Ir = 30,
};

constexpr CpuReg data_reg(unsigned n) { return static_cast<CpuReg>(int(CpuReg::D0) + int(n & 7)); }
constexpr CpuReg address_reg(unsigned n) { return static_cast<CpuReg>(int(CpuReg::A0) + int(n & 7)); }

// Musashi is one global CPU: the newest MusashiCore owns it, and only that
// one may run. Destroying an older one leaves the owner alone. Memory goes
// through bus.cpp's callbacks.
class MusashiCore {
public:
    explicit MusashiCore(Bus& bus);
    ~MusashiCore();
    MusashiCore(const MusashiCore&) = delete;
    MusashiCore& operator=(const MusashiCore&) = delete;

    void reset();
    int run(int cycles); // returns cycles actually executed
    void set_irq(int level);

    uint32_t reg(CpuReg r) const;
    // Only between run() calls.
    void set_reg(CpuReg r, uint32_t value);

    uint32_t pc() const { return reg(CpuReg::Pc); }
    uint32_t d(unsigned index) const { return index < 8 ? reg(data_reg(index)) : 0; }
    uint32_t a(unsigned index) const { return index < 8 ? reg(address_reg(index)) : 0; }

    uint32_t sp() const { return reg(CpuReg::Sp); }
    // SR is 16 bits; Musashi zero-extends it.
    uint16_t sr() const { return static_cast<uint16_t>(reg(CpuReg::Sr)); }
    bool supervisor() const { return (sr() & 0x2000u) != 0; }

    // Both stack banks; sp() is only the current one.
    uint32_t usp() const { return reg(CpuReg::Usp); }
    uint32_t isp() const { return reg(CpuReg::Isp); }

    // One SCC68070 instruction at `pc` into `text` (>= 256 bytes), read
    // through the side-effect-free bus path. Returns its size, 0 if invalid.
    uint32_t disassemble(char* text, uint32_t pc) const;

    // Called before each instruction; null removes it. Needs Musashi built
    // with M68K_INSTRUCTION_HOOK (the debugger's build); m68k_init() drops it.
    static void set_instruction_hook(void (*hook)(unsigned int pc));

    // Only between run() calls.
    template <class T>
    void state_io(T& archive);

private:
    static int int_ack(int level);

    Bus* bus_ = nullptr;
};

} // namespace zcdi
