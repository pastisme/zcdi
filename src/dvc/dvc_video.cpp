#include "dvc/dvc_video.h"

#include <algorithm>
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

void DvcVideo::feed(const std::vector<uint8_t>& data, const std::vector<ArrivalMark>& arrivals) {
    const uint64_t base = input_offset_ + impl_->input.size();
    for (const ArrivalMark& m : arrivals)
        input_marks_.push_back({base + m.offset, m.time});
    impl_->input.insert(impl_->input.end(), data.begin(), data.end());
}

uint32_t DvcVideo::arrival(uint64_t offset) const {
    uint32_t time = 0;
    for (const InputMark& m : input_marks_) {
        if (m.offset > offset) break;
        time = m.time;
    }
    return time;
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
    discarded_pictures_ = 0; // their DTS went with Clear FIFO
    pictures_since_restart_ = 0;
    forward_fixup_pending_ = true;
    pushed_reference_buffer_ = -1;
    step_ = {};
    decoder_idle_ = true;
    input_marks_.clear();
    input_offset_ = 0;
}

void DvcVideo::decoder_off() {
    clear_fifo();
    mpeg2_reset(impl_->decoder.get(), /*full_reset=*/1);
    sequence_header_latched_ = false;
    has_frame_ = false;
}

bool DvcVideo::make_pending(const void* picture_buffer, PendingPicture& pending) {
    mpeg2dec_t* dec = impl_->decoder.get();
    const auto* fbuf = static_cast<const mpeg2_fbuf_t*>(picture_buffer);
    const mpeg2_sequence_t* seq = mpeg2_info(dec)->sequence;
    const int index = buffer_index(dec, fbuf);
    if (seq == nullptr || index < 0 || fbuf->buf[0] == nullptr) return false;
    const PictureInfo info = picture_info_[size_t(index)];

    // sw/mpeg_video/main.c push_frame(): nothing is shown before the first I picture.
    if (info.coding_type == PIC_FLAG_CODING_TYPE_I) intra_shown_ = true;
    if (!intra_shown_) return false;
    pending = {};
    if (info.temporal_ref == 0) gop_intra_shown_ = false;
    if (!gop_intra_shown_ && info.coding_type == PIC_FLAG_CODING_TYPE_I) {
        gop_intra_shown_ = true;
        pending.starts_gop = true;
        pending.starts_sequence = sequence_header_latched_;
        sequence_header_latched_ = false;
    }
    pending.width = int(seq->picture_width);
    pending.height = int(seq->picture_height);
    pending.picture_rate_code = picture_rate_code(seq->frame_period);
    pack_yuv(*fbuf, *seq, pending.yuv);
    pending.timecode = info.timecode;
    pending.video_status = uint8_t(info.temporal_ref << 2 | info.coding_type);
    return true;
}

void DvcVideo::begin_step(uint32_t done_at) {
    step_ = {};
    step_.active = true;
    step_.done_at = done_at;
}

void DvcVideo::begin_push(const void* fbuf) {
    step_.push = make_pending(fbuf, step_.picture);
    step_.discard = !step_.push;
}

namespace {
// a, unless b is later (wrapping DCLK).
uint32_t later(uint32_t a, uint32_t b) { return int32_t(b - a) > 0 ? b : a; }
} // namespace

bool DvcVideo::finish_step(std::optional<uint32_t> now) {
    if (!step_.active) return true;
    if (now && int32_t(*now - step_.done_at) < 0) return false;
    if (step_.decoded && !step_.decoded_reported) {
        step_.decoded_reported = true;
        last_decoded_timecode_ = step_.timecode;
        last_decoded_picture_rate_code_ = step_.picture_rate_code;
        picture_decoded_pending_ = true;
        ++decoder_held_pictures_;
    }
    if (step_.push) {
        if (queue_.size() >= kOutputFifoPictures) {
            step_.push_blocked = true;
            return false;
        }
        queue_.push_back(std::move(step_.picture));
        end_of_data_ = false;
    }
    if (step_.push || step_.discard) {
        decoder_held_pictures_ = std::max(0, decoder_held_pictures_ - 1);
        if (step_.discard) ++discarded_pictures_;
    }
    if (step_.restart) decoder_held_pictures_ = 0;
    if (step_.end_of_data) end_of_data_ = true;
    if (now) decoder_free_at_ = step_.push_blocked ? *now : step_.done_at;
    decoder_idle_ = false;
    step_ = {};
    return true;
}

void DvcVideo::resume_push(std::optional<uint32_t> now) {
    if (step_.push_blocked) finish_step(now);
}

bool DvcVideo::decode_frame(std::optional<uint32_t> now) {
    const size_t before = queue_.size();
    // Untimed: until one picture is queued. Timed: whatever completes by `now`.
    for (int i = 0; i < 64 && finish_step(now); ++i) {
        if (!now && queue_.size() > before) break;
        if (!parse_step(now)) break;
    }
    return queue_.size() > before;
}

bool DvcVideo::parse_step(std::optional<uint32_t> now) {
    mpeg2dec_t* dec = impl_->decoder.get();
    std::vector<uint8_t>& input = impl_->input;
    mpeg2_buffer(dec, input.data(), input.data() + input.size());
    auto consumed = [&] { return input.size() - size_t(mpeg2_getpos(dec)); };
    auto start_at = [&](uint64_t offset) {
        const uint32_t data = arrival(offset);
        return decoder_idle_ ? data : later(decoder_free_at_, data);
    };
    bool stepped = false;
    while (!stepped) {
        const mpeg2_state_t state = mpeg2_parse(dec);
        const mpeg2_info_t* info = mpeg2_info(dec);
        if (state == STATE_BUFFER) {
            input_offset_ += input.size();
            input.clear();
            if (program_end_waiting_) {
                program_end_waiting_ = false;
                end_of_data_ = true;
            }
            break;
        }
        const uint64_t offset = input_offset_ + consumed();
        switch (state) {
            case STATE_SEQUENCE:
            case STATE_SEQUENCE_REPEATED:
            case STATE_SEQUENCE_MODIFIED:
                sequence_header_latched_ = true;
                // The newest reference goes out (picked as in mpeg2_header_end()); the frame
                // memory stays, so B pictures after the next I still predict from it.
                if (const mpeg2_fbuf_t* held = dec->fbuf[dec->decoder.coding_type == B_TYPE];
                    decoder_held_pictures_ > 0 && pictures_since_restart_ > 0 && held != nullptr) {
                    begin_step(start_at(offset));
                    pushed_reference_buffer_ = buffer_index(dec, held);
                    begin_push(held);
                    stepped = true;
                }
                break;
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
                picture_start_offset_ = offset;
                break;
            }
            case STATE_SLICE:
            case STATE_END:
            case STATE_INVALID_END:
                begin_step(start_at(offset));
                stepped = true;
                // After a restart the buffers still hold pictures from before it.
                if (pictures_since_restart_ > 0 && info->display_fbuf != nullptr) {
                    const int index = buffer_index(dec, info->display_fbuf);
                    if (index >= 0 && index == pushed_reference_buffer_) {
                        pushed_reference_buffer_ = -1;
                    } else {
                        begin_push(info->display_fbuf);
                    }
                }
                if (state == STATE_SLICE) {
                    // The picture whose slices just ended is decoded.
                    const int decoded = buffer_index(dec, info->current_fbuf);
                    if (decoded >= 0 && info->sequence != nullptr) {
                        step_.decoded = true;
                        step_.timecode = picture_info_[size_t(decoded)].timecode;
                        step_.picture_rate_code = picture_rate_code(info->sequence->frame_period);
                    }
                    // Decoding streams in, so it ends no earlier than the data does.
                    const uint64_t bytes = offset - std::min(offset, picture_start_offset_);
                    const uint32_t ticks = kDecodeBaseTicks + uint32_t(bytes * kDecodeTicksPerKiB / 1024);
                    step_.done_at = later(start_at(picture_start_offset_) + ticks, arrival(offset));
                    if (forward_fixup_pending_) {
                        forward_fixup_pending_ = false;
                        if (dec->decoder.coding_type == I_TYPE && dec->fbuf[0] != nullptr && dec->fbuf[1] != nullptr) {
                            copy_picture(*dec->fbuf[0], *dec->fbuf[1]);
                        }
                    }
                } else {
                    // The last reference picture is out; the next sequence starts afresh.
                    step_.restart = true;
                    pictures_since_restart_ = 0;
                    forward_fixup_pending_ = true;
                    if (state == STATE_END) {
                        sequence_end_pending_ = true;
                        step_.end_of_data = true;
                    }
                }
                break;
            default: break;
        }
    }
    if (stepped) {
        const size_t used = consumed();
        input.erase(input.begin(), input.begin() + std::ptrdiff_t(used));
        input_offset_ += used;
    }
    // Keep the mark the unparsed input starts in.
    size_t drop = 0;
    while (drop + 1 < input_marks_.size() && input_marks_[drop + 1].offset <= input_offset_)
        ++drop;
    input_marks_.erase(input_marks_.begin(), input_marks_.begin() + std::ptrdiff_t(drop));
    if (stepped) finish_step(now);
    return stepped;
}

bool DvcVideo::present_next_frame() {
    if (queue_.empty()) return false;
    presented_ = std::move(queue_.front());
    queue_.pop_front();
    convert_presented();
    has_frame_ = true;
    // EOD: mpeg_video.sv !for_display_valid && pictures_in_mpeg_decoder == 0.
    if (queue_.empty() && end_of_data_ && decoder_held_pictures_ == 0 && !step_.active) last_picture_pending_ = true;
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
        if (has_frame_ && !archive.failed()) {
            convert_presented();
        } else {
            rgba_.clear();
        }
    }
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(DvcVideo);

bool DvcVideoSaveState::state_valid() const {
    auto valid = [](const PendingPicture& p) {
        return p.width >= 0 && p.height >= 0 && p.yuv.size() == yuv_size(p.width, p.height);
    };
    if (has_frame_ && !valid(presented_)) return false;
    for (const PendingPicture& p : queue_) {
        if (!valid(p)) return false;
    }
    if (step_.push && !valid(step_.picture)) return false;
    return true;
}

} // namespace dvc
} // namespace zcdi
