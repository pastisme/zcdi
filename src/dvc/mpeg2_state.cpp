#include "dvc/mpeg2_state.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

extern "C" {
#include "libmpeg2/config.h" // same struct layout as the library
#include "mpeg2.h"

#include "attributes.h"
#include "libmpeg2/libmpeg2/mpeg2_internal.h"
}

#include "core/state_archive.h"
#include "dvc/mpeg2_glue.h"

namespace zcdi {
namespace dvc {

namespace {

constexpr size_t kChunkBufferSize = 1194 * 1024; // decode.c BUFFER_SIZE
constexpr size_t kMaxPlaneBytes = size_t(4096) * 4096;

// A pointer into base[0..count) as its index, -1 for NULL.
template <class A, class P, class B>
void io_index(A& archive, P*& p, B* base, int count) {
    int32_t i = -1;
    if constexpr (!A::kLoading) {
        if (p != nullptr) i = int32_t(p - base);
    }
    archive.io(i);
    if constexpr (A::kLoading) {
        archive.check(i >= -1 && i < count);
        p = (i >= 0 && i < count) ? base + i : nullptr;
    }
}

template <class A, class P>
void io_fbuf(A& archive, P*& p, mpeg2dec_t* dec) {
    int32_t i = -1;
    if constexpr (!A::kLoading) {
        for (int k = 0; k < 3; ++k) {
            if (p == &dec->fbuf_alloc[k].fbuf) i = k;
        }
        if (p != nullptr && i < 0) i = 3; // not ours: fails the load
    }
    archive.io(i);
    if constexpr (A::kLoading) {
        archive.check(i >= -1 && i < 3);
        p = (i >= 0 && i < 3) ? &dec->fbuf_alloc[i].fbuf : nullptr;
    }
}

template <class A>
void io_decoder(A& archive, mpeg2_decoder_t& d) {
    archive.io(d.stride_frame);
    archive.io_array(d.f_motion.f_code);
    archive.io_array(d.b_motion.f_code);
    archive.io_array(d.DCTblock);
    for (auto& q : d.chroma_quantizer) {
        int32_t k = -1;
        for (int i = 0; i < 4; ++i) {
            if (q == d.quantizer_prescale[i]) k = i;
        }
        archive.io(k);
        if constexpr (A::kLoading) {
            archive.check(k >= -1 && k < 4);
            q = (k >= 0 && k < 4) ? d.quantizer_prescale[k] : nullptr;
        }
    }
    archive.io_struct(d.quantizer_prescale);
    archive.io(d.width);
    archive.io(d.height);
    archive.io(d.vertical_position_extension);
    archive.io(d.chroma_format);
    archive.io(d.coding_type);
    archive.io(d.intra_dc_precision);
    archive.io(d.picture_structure);
    archive.io(d.frame_pred_frame_dct);
    archive.io(d.concealment_motion_vectors);
    archive.io(d.intra_vlc_format);
    archive.io(d.top_field_first);
    int32_t scan = d.scan == mpeg2_scan_alt ? 1 : d.scan == mpeg2_scan_norm ? 0 : -1;
    archive.io(scan);
    if constexpr (A::kLoading) d.scan = scan == 1 ? mpeg2_scan_alt : scan == 0 ? mpeg2_scan_norm : nullptr;
    archive.io(d.second_field);
    archive.io(d.mpeg1);
    archive.io(d.q_scale_type);
}

template <class A>
void io_frame_buffers(A& archive, mpeg2dec_t* dec) {
    for (int i = 0; i < 3; ++i) {
        for (uint8_t*& plane : dec->fbuf_alloc[i].fbuf.buf) {
            uint32_t size = 0;
            if constexpr (!A::kLoading) size = uint32_t(i < dec->alloc_index ? zcdi_mpeg2_alloc_size(plane) : 0);
            archive.io(size);
            if constexpr (A::kLoading) {
                archive.check(size <= kMaxPlaneBytes && (size == 0 || i < dec->alloc_index));
                if (archive.failed()) return;
                plane = size != 0 ? static_cast<uint8_t*>(mpeg2_malloc(size, MPEG2_ALLOC_YUV)) : nullptr;
                archive.check(size == 0 || plane != nullptr);
                if (archive.failed()) return;
            }
            if (size != 0) archive.io_span(std::span<uint8_t>(plane, size));
        }
    }
}

// Pointers the next slice_start would set; a save can land between two slices.
void rebuild_picture_pointers(mpeg2dec_t* dec) {
    mpeg2_fbuf_t* const* f = dec->fbuf;
    if (f[0] == nullptr || f[1] == nullptr || f[2] == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        if (f[i]->buf[0] == nullptr || f[i]->buf[1] == nullptr || f[i]->buf[2] == nullptr) return;
    }
    const int b_type = dec->decoder.coding_type == B_TYPE;
    mpeg2_init_fbuf(&dec->decoder, f[0]->buf, f[b_type + 1]->buf, f[b_type]->buf);
}

} // namespace

template <class T>
void mpeg2_state_io(T& archive, mpeg2dec_t* dec) {
    if constexpr (!T::kLoading) {
        // No converter, custom buffers, tags, skip or slice region: what those drive keeps
        // mpeg2_init()'s value and isn't saved (bytes_since_tag is only read with tags).
        archive.check(dec->convert == nullptr && dec->custom_fbuf == 0 && dec->num_tags == 0 &&
                      dec->alloc_index_user == 0 && dec->first_decode_slice == 1 && dec->nb_decode_slices == 0xb0 - 1 &&
                      dec->yuv_index == 0);
    }

    io_decoder(archive, dec->decoder);

    mpeg2_info_t& info = dec->info;
    io_index(archive, info.sequence, &dec->sequence, 1);
    io_index(archive, info.gop, &dec->gop, 1);
    io_index(archive, info.current_picture, dec->pictures, 4);
    io_index(archive, info.current_picture_2nd, dec->pictures, 4);
    io_fbuf(archive, info.current_fbuf, dec);
    io_index(archive, info.display_picture, dec->pictures, 4);
    io_index(archive, info.display_picture_2nd, dec->pictures, 4);
    io_fbuf(archive, info.display_fbuf, dec);
    io_fbuf(archive, info.discard_fbuf, dec);

    archive.io(dec->shift);
    int32_t action = zcdi_mpeg2_action_id(dec);
    archive.io(action);
    if constexpr (T::kLoading) archive.check(zcdi_mpeg2_set_action(dec, action) != 0);
    int32_t state = int32_t(dec->state);
    archive.io(state);
    if constexpr (T::kLoading) dec->state = mpeg2_state_t(state);
    archive.io(dec->ext_state);

    // Offsets into the chunk buffer; NULL until the first header search.
    constexpr uint32_t kNull = 0xffffffffu;
    uint32_t chunk_start = dec->chunk_start ? uint32_t(dec->chunk_start - dec->chunk_buffer) : kNull;
    uint32_t chunk_ptr = dec->chunk_ptr ? uint32_t(dec->chunk_ptr - dec->chunk_buffer) : kNull;
    archive.io(chunk_start);
    archive.io(chunk_ptr);
    if constexpr (T::kLoading) {
        archive.check((chunk_start <= kChunkBufferSize || chunk_start == kNull) &&
                      (chunk_ptr <= kChunkBufferSize || chunk_ptr == kNull));
        if (archive.failed()) return;
        dec->chunk_start = chunk_start != kNull ? dec->chunk_buffer + chunk_start : nullptr;
        dec->chunk_ptr = chunk_ptr != kNull ? dec->chunk_buffer + chunk_ptr : nullptr;
    }
    // Only whole chunks are parsed, so nothing past chunk_ptr is read again.
    archive.io_span(std::span<uint8_t>(dec->chunk_buffer, chunk_ptr != kNull ? chunk_ptr : 0));
    archive.io(dec->code);

    archive.io(dec->first);
    archive.io(dec->alloc_index);
    if constexpr (T::kLoading) archive.check(dec->alloc_index >= 0 && dec->alloc_index <= 3);
    archive.io(dec->user_data_len);

    archive.io_struct(dec->new_sequence);
    archive.io_struct(dec->sequence);
    archive.io_struct(dec->new_gop);
    archive.io_struct(dec->gop);
    archive.io_struct(dec->new_picture);
    archive.io_struct(dec->pictures);
    io_index(archive, dec->picture, dec->pictures, 4);
    for (mpeg2_fbuf_t*& f : dec->fbuf)
        io_fbuf(archive, f, dec);
    if (archive.failed()) return;
    io_frame_buffers(archive, dec);

    archive.io(dec->display_offset_x);
    archive.io(dec->display_offset_y);
    archive.io(dec->copy_matrix);
    archive.io_array(dec->scaled);
    archive.io_struct(dec->quantizer_matrix);
    archive.io_struct(dec->new_quantizer_matrix);

    if constexpr (T::kLoading) {
        if (archive.failed()) return;
        info.user_data = nullptr;
        info.user_data_len = 0;
        dec->buf_start = dec->buf_end = nullptr;
        rebuild_picture_pointers(dec);
    }
}

template void mpeg2_state_io<StateWriter>(StateWriter&, mpeg2dec_t*);
template void mpeg2_state_io<StateReader>(StateReader&, mpeg2dec_t*);
template void mpeg2_state_io<StateSizer>(StateSizer&, mpeg2dec_t*);

} // namespace dvc
} // namespace zcdi
