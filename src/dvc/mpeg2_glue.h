#pragma once
// C side of the libmpeg2 save-state glue (mpeg2_glue.c).
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct mpeg2dec_s mpeg2dec_t;

// Routes all libmpeg2 allocations through zeroed, size-tagged buffers. Call before mpeg2_init().
void zcdi_mpeg2_install_allocator(void);
// Size requested for a buffer from that allocator; 0 for NULL.
size_t zcdi_mpeg2_alloc_size(const void* p);

// The pending parse action as 0-6, or -1 if unknown.
int zcdi_mpeg2_action_id(const mpeg2dec_t* dec);
// 0 if `id` isn't 0-6.
int zcdi_mpeg2_set_action(mpeg2dec_t* dec, int id);

#ifdef __cplusplus
}
#endif
