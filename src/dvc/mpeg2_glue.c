// libmpeg2's decode.c and header.c, built here so save states can name the
// parse actions those files keep static. No library source is changed.
#include "decode.c"
#include "header.c"

#include "dvc/mpeg2_glue.h"

// Every libmpeg2 buffer: zeroed, 64-byte aligned, with its size recorded for save states.
typedef struct {
    void* base;
    size_t size;
} zcdi_alloc_header;

static void* zcdi_mpeg2_alloc(unsigned size, mpeg2_alloc_t reason) {
    (void)reason;
    char* base = (char*)calloc(1, size + 64 + sizeof(zcdi_alloc_header));
    if (base == NULL) return NULL;
    char* p = base + sizeof(zcdi_alloc_header);
    p += (64 - ((uintptr_t)p & 63)) & 63;
    ((zcdi_alloc_header*)p)[-1].base = base;
    ((zcdi_alloc_header*)p)[-1].size = size;
    return p;
}

static int zcdi_mpeg2_free(void* p) {
    if (p != NULL) free(((zcdi_alloc_header*)p)[-1].base);
    return 1;
}

void zcdi_mpeg2_install_allocator(void) { mpeg2_malloc_hooks(zcdi_mpeg2_alloc, zcdi_mpeg2_free); }

size_t zcdi_mpeg2_alloc_size(const void* p) { return p != NULL ? ((const zcdi_alloc_header*)p)[-1].size : 0; }

// A save state stores the index of dec->action here.
static mpeg2_state_t (*const zcdi_mpeg2_actions[])(mpeg2dec_t*) = {
    NULL,       mpeg2_seek_header,  mpeg2_parse_header, mpeg2_header_slice_start,
    seek_chunk, invalid_end_action, seek_sequence,
};
static const int zcdi_mpeg2_action_count = (int)(sizeof(zcdi_mpeg2_actions) / sizeof(zcdi_mpeg2_actions[0]));

int zcdi_mpeg2_action_id(const mpeg2dec_t* dec) {
    for (int id = 0; id < zcdi_mpeg2_action_count; ++id) {
        if (dec->action == zcdi_mpeg2_actions[id]) return id;
    }
    return -1;
}

int zcdi_mpeg2_set_action(mpeg2dec_t* dec, int id) {
    if (id < 0 || id >= zcdi_mpeg2_action_count) return 0;
    dec->action = zcdi_mpeg2_actions[id];
    return 1;
}
