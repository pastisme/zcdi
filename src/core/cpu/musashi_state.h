#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The m68ki_cpu fields an SCC68070 can change; the rest are fixed by
 * m68k_set_cpu_type() or only reachable on 020+ (FPU, PMMU, cache, virq). */
#define ZCDI_M68K_STATE_FIELDS(ARRAY, SCALAR) \
    ARRAY(dar, 16)                            \
    ARRAY(dar_save, 16)                       \
    ARRAY(sp, 7)                              \
    SCALAR(ppc)                               \
    SCALAR(pc)                                \
    SCALAR(vbr)                               \
    SCALAR(sfc)                               \
    SCALAR(dfc)                               \
    SCALAR(ir)                                \
    SCALAR(t1_flag)                           \
    SCALAR(t0_flag)                           \
    SCALAR(s_flag)                            \
    SCALAR(m_flag)                            \
    SCALAR(x_flag)                            \
    SCALAR(n_flag)                            \
    SCALAR(not_z_flag)                        \
    SCALAR(v_flag)                            \
    SCALAR(c_flag)                            \
    SCALAR(int_mask)                          \
    SCALAR(int_level)                         \
    SCALAR(stopped)                           \
    SCALAR(instr_mode)                        \
    SCALAR(run_mode)                          \
    SCALAR(reset_cycles)                      \
    SCALAR(nmi_pending)

#define ZCDI_M68K_STATE_ARRAY(name, n) uint32_t name[n];
#define ZCDI_M68K_STATE_SCALAR(name) uint32_t name;
typedef struct ZcdiM68kState {
    ZCDI_M68K_STATE_FIELDS(ZCDI_M68K_STATE_ARRAY, ZCDI_M68K_STATE_SCALAR)
} ZcdiM68kState;
#undef ZCDI_M68K_STATE_ARRAY
#undef ZCDI_M68K_STATE_SCALAR

/* Zeroes the whole CPU, pointers included: m68k_init() and
 * m68k_set_cpu_type() must follow. */
void zcdi_m68k_state_clear(void);
void zcdi_m68k_state_get(ZcdiM68kState* out);
void zcdi_m68k_state_set(const ZcdiM68kState* in);

#ifdef __cplusplus
}
#endif
