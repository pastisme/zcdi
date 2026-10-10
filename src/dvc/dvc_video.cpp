#include "dvc/dvc_video.h"

#include <cstddef>
#include <cstring>

extern "C" {
#include "libmpeg2/config.h" // same struct layout as the library
#include "mpeg2.h"

#include "attributes.h"
#include "libmpeg2/libmpeg2/mpeg2_internal.h"
}

#include "core/state_archive.h"
#include "core/util.h"
#include "dvc/mpeg2_glue.h"
#include "dvc/mpeg2_state.h"

namespace zcdi {
namespace dvc {

namespace {
size_t chroma_width(int width) { return size_t(width + 1) / 2; }
size_t chroma_height(int height) { return size_t(height + 1) / 2; }
size_t yuv_size(int width, int height) {
    return size_t(width) * size_t(height) + 2 * chroma_width(width) * chroma_height(height);
}

// Packs a decoded picture's visible area as Y, Cb, Cr planes without padding.
void pack_yuv(const mpeg2_fbuf_t& fbuf, const mpeg2_sequence_t& seq, std::vector<uint8_t>& out) {
    const int width = int(seq.picture_width);
    const int height = int(seq.picture_height);
    out.resize(yuv_size(width, height));
    uint8_t* dst = out.data();
    for (int y = 0; y < height; ++y, dst += width) {
        std::memcpy(dst, fbuf.buf[0] + size_t(y) * seq.width, size_t(width));
    }
    for (int plane = 1; plane < 3; ++plane) {
        for (size_t y = 0; y < chroma_height(height); ++y, dst += chroma_width(width)) {
            std::memcpy(dst, fbuf.buf[plane] + y * seq.chroma_width, chroma_width(width));
        }
    }
}

// Studio levels (black 16) like the MCD212 planes, Green Book V.4.4.2.2.
void yuv_to_studio_rgba(const uint8_t* yuv, int width, int height, uint8_t* dest) {
    const size_t cw = chroma_width(width);
    const uint8_t* cb_plane = yuv + size_t(width) * size_t(height);
    const uint8_t* cr_plane = cb_plane + cw * chroma_height(height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* luma = yuv + size_t(y) * size_t(width);
        const uint8_t* cb_row = cb_plane + size_t(y >> 1) * cw;
        const uint8_t* cr_row = cr_plane + size_t(y >> 1) * cw;
        uint8_t* out = dest + size_t(y) * size_t(width) * 4;
        // Each chroma sample covers two pixels.
        for (int x = 0; x < width; x += 2) {
            const int cb = cb_row[x >> 1] - 128;
            const int cr = cr_row[x >> 1] - 128;
            // 1.402, 0.344136, 0.714136, 1.772 in 1/65536 units.
            const int r = (cr * 91881) >> 16;
            const int g = (cb * 22554 + cr * 46802) >> 16;
            const int b = (cb * 116130) >> 16;
            for (int i = x; i < x + 2 && i < width; ++i) {
                const int l = luma[i];
                out[4 * i + 0] = clamp_u8(l + r);
                out[4 * i + 1] = clamp_u8(l - g);
                out[4 * i + 2] = clamp_u8(l + b);
                out[4 * i + 3] = 0xff;
            }
        }
    }
}

// The sequence header's picture_rate code, from libmpeg2's frame period (27 MHz ticks).
int picture_rate_code(unsigned frame_period) {
    static constexpr unsigned kPeriods[] = {0, 1126125, 1125000, 1080000, 900900, 900000, 540000, 450450, 450000};
    for (int code = 1; code < 9; ++code) {
        if (kPeriods[code] == frame_period) return code;
    }
    return 0;
}

// s << 22 | pic << 16 | h << 6 | min, as the FMV time code registers hold it.
uint32_t pack_timecode(const mpeg2_gop_t& gop) {
    return uint32_t(gop.seconds & 63) << 22 | uint32_t(gop.pictures & 63) << 16 | uint32_t(gop.hours & 31) << 6 |
           uint32_t(gop.minutes & 63);
}

int buffer_index(const mpeg2dec_t* dec, const mpeg2_fbuf_t* fbuf) {
    for (int i = 0; i < 3; ++i) {
        if (fbuf == &dec->fbuf_alloc[i].fbuf) return i;
    }
    return -1;
}

void copy_picture(const mpeg2_fbuf_t& from, mpeg2_fbuf_t& to) {
    for (int i = 0; i < 3; ++i) {
        const size_t n = zcdi_mpeg2_alloc_size(from.buf[i]);
        if (n != 0 && n == zcdi_mpeg2_alloc_size(to.buf[i])) std::memcpy(to.buf[i], from.buf[i], n);
    }
}

struct Mpeg2Closer {
    void operator()(mpeg2dec_t* dec) const { mpeg2_close(dec); }
};

std::unique_ptr<mpeg2dec_t, Mpeg2Closer> make_decoder() {
    static const bool kAllocatorInstalled = (zcdi_mpeg2_install_allocator(), true);
    (void)kAllocatorInstalled;
    return std::unique_ptr<mpeg2dec_t, Mpeg2Closer>(mpeg2_init());
}
} // namespace

struct DvcVideo::Impl {
    std::unique_ptr<mpeg2dec_t, Mpeg2Closer> decoder = make_decoder();
    std::vector<uint8_t> input; // not yet parsed by libmpeg2
};

DvcVideo::DvcVideo() : impl_(std::make_unique<Impl>()) {}

DvcVideo::~DvcVideo() = default;

void DvcVideo::feed(const std::vector<uint8_t>& data) {
    impl_->input.insert(impl_->input.end(), data.begin(), data.end());
}

size_t DvcVideo::input_bytes() const { return impl_->input.size(); }

void DvcVideo::clear_fifo() {
    impl_->input.clear();
    mpeg2_reset(impl_->decoder.get(), /*full_reset=*/0); // keeps the sequence header
    // mpeg_video.sv readyframes reset; the picture on screen stays.
    queue_.clear();
    decoder_held_pictures_ = 0;
    end_of_data_ = false;
    last_picture_pending_ = false;
    program_end_waiting_ = false;
    pictures_since_restart_ = 0;
    forward_fixup_pending_ = true;
}

void DvcVideo::decoder_off() {
    clear_fifo();
    mpeg2_reset(impl_->decoder.get(), /*full_reset=*/1);
    sequence_header_latched_ = false;
    has_frame_ = false;
}

bool DvcVideo::output_picture(const void* picture_buffer) {
    mpeg2dec_t* dec = impl_->decoder.get();
    const auto* fbuf = static_cast<const mpeg2_fbuf_t*>(picture_buffer);
    const mpeg2_sequence_t* seq = mpeg2_info(dec)->sequence;
    const int index = buffer_index(dec, fbuf);
    if (seq == nullptr || index < 0 || fbuf->buf[0] == nullptr) return false;
    const PictureInfo info = picture_info_[size_t(index)];

    // sw/mpeg_video/main.c push_frame(): nothing is shown before the first I picture.
    if (info.coding_type == PIC_FLAG_CODING_TYPE_I) intra_shown_ = true;
    if (!intra_shown_) return false;
    PendingPicture pending;
    if (info.temporal_ref == 0) gop_intra_shown_ = false;
    if (!gop_intra_shown_ && info.coding_type == PIC_FLAG_CODING_TYPE_I) {
        gop_intra_shown_ = true;
        pending.starts_gop = true;
        pending.starts_sequence = sequence_header_latched_;
        sequence_header_latched_ = false;
    }
    pending.width = int(seq->picture_width);
    pending.height = int(seq->picture_height);
    pack_yuv(*fbuf, *seq, pending.yuv);
    pending.timecode = info.timecode;
    pending.video_status = uint8_t(info.temporal_ref << 2 | info.coding_type);
    end_of_data_ = false;
    queue_.push_back(std::move(pending));
    return true;
}

bool DvcVideo::decode_frame() {
    if (queue_.size() >= kMaxDecodedPictures) return false;
    mpeg2dec_t* dec = impl_->decoder.get();
    std::vector<uint8_t>& input = impl_->input;
    mpeg2_buffer(dec, input.data(), input.data() + input.size());
    bool queued = false;
    while (!queued) {
        const mpeg2_state_t state = mpeg2_parse(dec);
        const mpeg2_info_t* info = mpeg2_info(dec);
        if (state == STATE_BUFFER) {
            input.clear();
            if (program_end_waiting_) {
                program_end_waiting_ = false;
                end_of_data_ = true;
            }
            return false;
        }
        switch (state) {
            case STATE_SEQUENCE:
            case STATE_SEQUENCE_REPEATED:
            case STATE_SEQUENCE_MODIFIED: sequence_header_latched_ = true; break;
            case STATE_GOP:
                if (info->gop != nullptr) gop_timecode_ = pack_timecode(*info->gop);
                break;
            case STATE_PICTURE: {
                const int index = buffer_index(dec, info->current_fbuf);
                if (index >= 0 && info->current_picture != nullptr) {
                    picture_info_[size_t(index)] = {gop_timecode_, uint16_t(info->current_picture->temporal_reference),
                                                    uint8_t(info->current_picture->flags & PIC_MASK_CODING_TYPE)};
                }
                ++pictures_since_restart_;
                break;
            }
            case STATE_SLICE:
            case STATE_END:
            case STATE_INVALID_END:
                // After a restart the buffers still hold pictures from before it.
                if (pictures_since_restart_ > 0 && info->display_fbuf != nullptr) {
                    queued = output_picture(info->display_fbuf);
                }
                if (state == STATE_SLICE) {
                    // The picture whose slices just ended is decoded.
                    const int decoded = buffer_index(dec, info->current_fbuf);
                    if (decoded >= 0 && info->sequence != nullptr) {
                        last_decoded_timecode_ = picture_info_[size_t(decoded)].timecode;
                        last_decoded_width_ = int(info->sequence->picture_width);
                        last_decoded_height_ = int(info->sequence->picture_height);
                        last_decoded_picture_rate_code_ = picture_rate_code(info->sequence->frame_period);
                        picture_decoded_pending_ = true;
                    }
                    const bool reference = dec->decoder.coding_type != B_TYPE;
                    if (reference) decoder_held_pictures_ = 1;
                    if (forward_fixup_pending_) {
                        forward_fixup_pending_ = false;
                        if (dec->decoder.coding_type == I_TYPE && dec->fbuf[0] != nullptr && dec->fbuf[1] != nullptr) {
                            copy_picture(*dec->fbuf[0], *dec->fbuf[1]);
                        }
                    }
                } else {
                    // The last reference picture is out; the next sequence starts afresh.
                    decoder_held_pictures_ = 0;
                    pictures_since_restart_ = 0;
                    forward_fixup_pending_ = true;
                    if (state == STATE_END) {
                        sequence_end_pending_ = true;
                        end_of_data_ = true;
                    }
                }
                break;
            default: break;
        }
    }
    const size_t unparsed = size_t(mpeg2_getpos(dec));
    input.erase(input.begin(), input.end() - std::ptrdiff_t(unparsed));
    return true;
}

bool DvcVideo::present_next_frame() {
    if (queue_.empty()) return false;
    presented_ = std::move(queue_.front());
    queue_.pop_front();
    convert_presented();
    has_frame_ = true;
    // EOD: mpeg_video.sv !for_display_valid && pictures_in_mpeg_decoder == 0.
    if (queue_.empty() && end_of_data_ && decoder_held_pictures_ == 0) last_picture_pending_ = true;
    return true;
}

void DvcVideo::convert_presented() {
    rgba_.resize(size_t(presented_.width) * size_t(presented_.height) * 4);
    yuv_to_studio_rgba(presented_.yuv.data(), presented_.width, presented_.height, rgba_.data());
}

template <class T>
void DvcVideo::state_io(T& archive) {
    archive.begin_section(state_tag("DVCV"));
    if constexpr (T::kLoading) impl_ = std::make_unique<Impl>();
    archive.check(impl_->decoder != nullptr);
    if (archive.failed()) return;
    archive.io_vec(impl_->input, kStateMaxBuffer);
    mpeg2_state_io(archive, impl_->decoder.get());
    archive.io_struct(static_cast<DvcVideoSaveState&>(*this));
    if constexpr (T::kLoading) {
        archive.check(loaded());
        if (has_frame_ && !archive.failed()) {
            convert_presented();
        } else {
            rgba_.clear();
        }
    }
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(DvcVideo);

bool DvcVideo::loaded() const {
    auto valid = [](const PendingPicture& p) {
        return p.width >= 0 && p.height >= 0 && p.yuv.size() == yuv_size(p.width, p.height);
    };
    if (has_frame_ && !valid(presented_)) return false;
    for (const PendingPicture& p : queue_) {
        if (!valid(p)) return false;
    }
    return true;
}

} // namespace dvc
} // namespace zcdi
