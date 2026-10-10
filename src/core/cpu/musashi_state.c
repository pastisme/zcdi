#include "core/cpu/musashi_state.h"

#include <string.h>

// m68kcpu.h is Musashi's internal header; its warnings aren't ours.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "m68kcpu.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void zcdi_m68k_state_clear(void) { memset(&m68ki_cpu, 0, sizeof m68ki_cpu); }

#define COPY_ARRAY(name, n) memcpy(DST.name, SRC.name, sizeof DST.name);
#define COPY_SCALAR(name) DST.name = SRC.name;

void zcdi_m68k_state_get(ZcdiM68kState* out) {
#define DST (*out)
#define SRC m68ki_cpu
    ZCDI_M68K_STATE_FIELDS(COPY_ARRAY, COPY_SCALAR)
#undef DST
#undef SRC
}

void zcdi_m68k_state_set(const ZcdiM68kState* in) {
#define DST m68ki_cpu
#define SRC (*in)
    ZCDI_M68K_STATE_FIELDS(COPY_ARRAY, COPY_SCALAR)
#undef DST
#undef SRC
}
