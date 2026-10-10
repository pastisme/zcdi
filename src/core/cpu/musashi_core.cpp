#include "core/cpu/musashi_core.h"

#include "core/bus.h"
#include "core/cpu/musashi_state.h"
#include "core/state_archive.h"

#include "m68k.h"

namespace zcdi {

namespace {

// Musashi's callbacks take no user pointer.
MusashiCore* g_core = nullptr;

constexpr bool same_reg(CpuReg r, m68k_register_t m) { return static_cast<int>(r) == static_cast<int>(m); }
static_assert(same_reg(CpuReg::D0, M68K_REG_D0) && same_reg(CpuReg::D7, M68K_REG_D7));
static_assert(same_reg(CpuReg::A0, M68K_REG_A0) && same_reg(CpuReg::A7, M68K_REG_A7));
static_assert(same_reg(CpuReg::Pc, M68K_REG_PC) && same_reg(CpuReg::Sr, M68K_REG_SR));
static_assert(same_reg(CpuReg::Sp, M68K_REG_SP) && same_reg(CpuReg::Usp, M68K_REG_USP));
static_assert(same_reg(CpuReg::Isp, M68K_REG_ISP));
static_assert(same_reg(CpuReg::Ppc, M68K_REG_PPC) && same_reg(CpuReg::Ir, M68K_REG_IR));

} // namespace

MusashiCore::MusashiCore(Bus& bus) : bus_(&bus) {
    g_core = this;
    bus.set_cpu(this);
    // Musashi's global CPU outlives us and reset keeps D0-D7: start from
    // zero so the power-on baseline doesn't depend on earlier runs.
    zcdi_m68k_state_clear();
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_SCC68070);
    m68k_set_int_ack_callback(&MusashiCore::int_ack);

    // Known gap: CPU_TYPE_IS_010_LESS omits SCC070, so CHK/TRAPV/div0/trace
    // push a format-2 frame (not seen in practice).
}

MusashiCore::~MusashiCore() {
    bus_->set_cpu(nullptr);
    if (g_core == this) g_core = nullptr;
}

// Needs M68K_EMULATE_INT_ACK (CMakeLists.txt); without it every interrupt
// autovectors and Scc68070Periph::iack never runs.
int MusashiCore::int_ack(int level) {
    if (!g_core) return M68K_INT_ACK_AUTOVECTOR;
    const int vector = g_core->bus_->interrupt_ack(level);
    // -1: a dedicated external pin, autovectored.
    return vector < 0 ? M68K_INT_ACK_AUTOVECTOR : vector;
}

void MusashiCore::reset() { m68k_pulse_reset(); }
int MusashiCore::run(int cycles) { return m68k_execute(cycles); }
template <class T>
void MusashiCore::state_io(T& archive) {
    ZcdiM68kState st{};
    zcdi_m68k_state_get(&st);
    archive.begin_section(state_tag("CPU "));
    archive.io_struct(st);
    archive.end_section();
    if constexpr (T::kLoading) {
        if (!archive.failed()) zcdi_m68k_state_set(&st);
    }
}
ZCDI_STATE_IO_INSTANTIATE(MusashiCore);

void MusashiCore::set_irq(int level) { m68k_set_irq(level); }

uint32_t MusashiCore::reg(CpuReg r) const { return m68k_get_reg(nullptr, static_cast<m68k_register_t>(r)); }
void MusashiCore::set_reg(CpuReg r, uint32_t value) { m68k_set_reg(static_cast<m68k_register_t>(r), value); }

uint32_t MusashiCore::disassemble(char* text, uint32_t pc) const {
    return m68k_disassemble(text, pc, M68K_CPU_TYPE_SCC68070);
}

void MusashiCore::set_instruction_hook(void (*hook)(unsigned int pc)) { m68k_set_instr_hook_callback(hook); }

} // namespace zcdi
