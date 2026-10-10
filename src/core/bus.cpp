#include "core/bus.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "core/cpu/musashi_core.h"
#include "core/playcdi_overlay.h"
#include "core/state_archive.h"
#include "core/util.h"
#include "m68k.h"

namespace zcdi {

namespace {
// The Bus with a CPU attached; Musashi's memory callbacks take no user pointer.
Bus* g_active_bus = nullptr;

// Green Book disc label: LBA 16, record type 1, "CD-I ".
bool has_cdi_disc_label(Disc& disc) {
    RawSector s{};
    if (!disc.read_sector(16, s) || s.data[15] != 2) return false;
    const uint8_t* label = s.data + kSyncSize + kHeaderSize + kSubheaderSize;
    return label[0] == 1 && std::memcmp(label + 1, "CD-I ", 5) == 0;
}

// MAME cdimono1_mem; sizes match MAME's region declarations.
constexpr uint32_t kPlane0Base = 0x000000, kPlane0Size = 0x080000;
constexpr uint32_t kPlane1Base = 0x200000, kPlane1Size = 0x080000;
constexpr uint32_t kNvramBase = 0x320000, kNvramSize = 0x004000;
constexpr uint32_t kBiosBase = 0x400000, kBiosSize = 0x080000;
constexpr uint32_t kCdicRamBase = 0x300000, kCdicRamSize = 0x003c00;
constexpr uint32_t kCdicRegBase = 0x303c00, kCdicRegSize = 0x000400;
constexpr uint32_t kSlaveBase = 0x310000, kSlaveSize = 0x008000;
constexpr uint32_t kMcd212Base = 0x4fffe0, kMcd212Size = 0x000020;
constexpr uint32_t kExtraBase = 0x500000, kExtraSize = 0x080000;
// MAME .noprw(): the video driver writes 0x318000 at every boot.
constexpr uint32_t kSlaveNvramGapBase = 0x318000, kSlaveNvramGapSize = 0x008000;
// Faults without a cartridge, so the OS-9 RAM probe never pools it.
constexpr uint32_t kDvcCartRamBase = 0xd00000, kDvcCartRamSize = 0x100000;
constexpr uint32_t kDvcCartRomBase = 0xe40000, kDvcCartRomSize = 0x040000;
constexpr uint32_t kDvcDecodeRamBase = 0xe80000, kDvcDecodeRamSize = 0x080000;
constexpr uint32_t kDvcRegBase = 0xe00000, kDvcRegSize = 0x040000;
// cditop.sv rom_playcdi_cs
constexpr uint32_t kPlayCdiOverlayBase = 0xf00000;

constexpr bool in_range(uint32_t a, uint32_t base, uint32_t size) { return a >= base && a < base + size; }

// `a` is in the NVRAM window; only the even byte lane decodes (MAME umask16(0xff00)).
static_assert(kNvramSize / 2 == mamecdi::Mk48t08::kSize);
bool decode_nvram_bus_addr(uint32_t a, uint32_t& tk_off) {
    const uint32_t off = a - kNvramBase;
    if (off & 1u) return false;
    tk_off = off >> 1;
    return true;
}

} // namespace

Bus::Bus()
    : plane0_(kPlane0Size, 0),
      plane1_(kPlane1Size, 0),
      extra_ram_(kExtraSize, 0),
      dvc_ram_(kDvcCartRamSize, 0),
      dvc_decode_ram_(kDvcDecodeRamSize, 0) {}

void Bus::load_nvram(const std::span<const std::byte>& content) {
    nvram_.load({reinterpret_cast<const uint8_t*>(content.data()), content.size()});
}

void Bus::load_bios(const std::span<const std::byte>& content) {
    // The circulated dumps are already big-endian.
    const auto* p = reinterpret_cast<const uint8_t*>(content.data());
    rom_.assign(p, p + content.size());
    rom_crc_ = state_crc32(rom_);
}

void Bus::load_dvc(const std::span<const std::byte>& content) {
    const auto* p = reinterpret_cast<const uint8_t*>(content.data());
    dvc_rom_.assign(p, p + std::min(content.size(), std::size_t{kDvcCartRomSize}));
    dvc_rom_crc_ = state_crc32(dvc_rom_);
}

void Bus::prepare_reset_vectors() {
    if (rom_.size() < 8) return; // no BIOS loaded yet
    for (int i = 0; i < 8; ++i)
        plane0_[i] = rom_[i];
}

std::string Bus::drain_uart_output() { return scc_.drain_uart_output(); }

void Bus::dvc_tick_frame(double refresh_rate_hz) {
    // Decoder Off also drops the shown picture so it can't flash up at the next Show.
    if (vmpeg_.consume_decoder_off()) {
        dvc_video_.decoder_off();
        mcd212_.set_dvc_frame(nullptr, 0, 0);
    }
    // Before feeding, so a cut-off picture isn't glued onto the next clip.
    if (vmpeg_.consume_clear_fifo()) {
        dvc_video_.clear_fifo();
    }
    // Demuxed by VMPEG from the channel-1 DMA and XFER writes.
    dvc_video_.feed(vmpeg_.drain_fmv_es_bytes());
    if (vmpeg_.consume_fmv_program_end()) dvc_video_.note_program_end();
    dvc_video_.decode_frame();
    if (dvc_video_.consume_picture_decoded()) {
        vmpeg_.set_video_decoder_timecode(dvc_video_.last_decoded_timecode());
        vmpeg_.set_video_frame_decoded(dvc_video_.last_decoded_width(), dvc_video_.last_decoded_height(),
                                       dvc_video_.last_decoded_picture_rate_code());
    }
    vmpeg_.set_decoder_held_pictures(dvc_video_.decoder_held_pictures());
    vmpeg_.set_decoder_input_bytes(dvc_video_.input_bytes());

    // The frame player decides presentation; skipped pictures raise no PIC.
    const uint32_t field_clocks = uint32_t(std::lround(30000000.0 / refresh_rate_hz));
    field_samples_ = int32_t(std::lround(44100.0 / refresh_rate_hz));
    const auto step = vmpeg_.frame_player_field(field_clocks, dvc_video_.queue_depth());
    const auto report_presented = [&] {
        vmpeg_.set_video_display_timecode(dvc_video_.presented_timecode());
        vmpeg_.set_video_display_status(dvc_video_.presented_video_status());
    };
    for (int i = 0; i < step.skipped && dvc_video_.present_next_frame(); ++i) {
        vmpeg_.note_picture_displayed();
        report_presented();
    }
    // PIC, GOP and SEQ belong to the display process.
    if (step.show && dvc_video_.present_next_frame()) {
        vmpeg_.note_picture_displayed();
        vmpeg_.set_video_frame_ready(dvc_video_.width(), dvc_video_.height());
        report_presented();
        if (dvc_video_.presented_starts_gop()) vmpeg_.set_video_gop_start();
        if (dvc_video_.presented_starts_sequence()) vmpeg_.set_video_sequence_start();
    }
    // No picture of its own, so checked every field.
    if (dvc_video_.consume_sequence_end()) vmpeg_.set_video_sequence_end();
    // RTL event_last_picture_starts_display: NDAT and EOD together.
    if (dvc_video_.consume_last_picture_shown()) {
        vmpeg_.set_video_buffer_underflow();
        vmpeg_.set_video_last_picture();
    }
}

int Bus::video_begin_field(int lines) { return mcd212_.begin_field(lines); }

void Bus::video_end_of_line(int line) { mcd212_.end_of_line(line, plane0_.data(), plane1_.data(), plane0_.size()); }

void Bus::video_end_field() {
    if (dvc_video_.has_frame()) {
        mcd212_.set_dvc_frame(dvc_video_.frame_rgba(), dvc_video_.width(), dvc_video_.height());
    }
    mcd212_.set_dvc_attached(dvc_cart_present());
    mamecdi::VmpegDevice::DisplayWindow w = vmpeg_.display_window();
    mcd212_.set_dvc_window(w.scrpos_x, w.scrpos_y, w.decwin_w, w.decwin_h, w.decoff_x, w.decoff_y, w.show,
                           w.vcd_pixel_clock);
    mcd212_.end_field();
    vmpeg_.notify_vsync(); // one vsync edge per field
}

template <class T>
void Bus::state_io(T& archive) {
    archive.begin_section(state_tag("BUS "));
    identity_io(archive);
    if (archive.failed()) return;

    ram_state_io(archive);
    archive.io_struct(static_cast<BusSaveState&>(*this));
    archive.check(in4_owner_ == In4Owner::kIdle || in4_owner_ == In4Owner::kCdic || in4_owner_ == In4Owner::kVmpeg);

    archive.end_section();

    scc_.state_io(archive);
    mcd212_.state_io(archive);
    slave_.state_io(archive);
    pointer2_.state_io(archive);
    cdic_.state_io(archive);
    vmpeg_.state_io(archive);
    nvram_.state_io(archive);
    dvc_video_.state_io(archive);
    dvc_audio_.state_io(archive);
    if constexpr (T::kLoading) {
        sync_tray();
        // The load replaced the picture the overlay pointed into.
        mcd212_.rebind_dvc_frame(dvc_video_.frame_rgba(), size_t(dvc_video_.width()) * size_t(dvc_video_.height()) * 4);
    }
}
ZCDI_STATE_IO_INSTANTIATE(Bus);

template <class T>
void Bus::identity_io(T& archive) {
    uint32_t rom_crc = rom_crc_, dvc_rom_crc = dvc_rom_crc_;
    bool auto_play = auto_play_enabled_;
    archive.io(rom_crc);
    archive.io(dvc_rom_crc);
    archive.io(auto_play);
    archive.check(rom_crc == rom_crc_);
    archive.check(dvc_rom_crc == dvc_rom_crc_);
    archive.check(auto_play == auto_play_enabled_);
}
template void Bus::identity_io<StateReader>(StateReader&);

template <class T>
void Bus::nvram_state_io(T& archive) {
    nvram_.state_io(archive);
}

template <class T>
void Bus::ram_state_io(T& archive) {
    archive.io_fixed(plane0_);
    archive.io_fixed(plane1_);
    archive.io_fixed(extra_ram_);
    // identity_io() has matched the cartridge already.
    if (dvc_cart_present()) archive.io_fixed(dvc_ram_);
    archive.io_fixed(dvc_decode_ram_);
}

template void Bus::nvram_state_io<StateWriter>(StateWriter&);
template void Bus::nvram_state_io<StateReader>(StateReader&);
template void Bus::ram_state_io<StateWriter>(StateWriter&);
template void Bus::ram_state_io<StateReader>(StateReader&);
template void Bus::ram_state_io<StateSizer>(StateSizer&);

void Bus::set_disc(Disc* disc) {
    disc_ = disc;
    sync_tray();
    disc_mounted_ = disc != nullptr;
    auto tracks = disc ? disc->toc_tracks() : std::vector<TocTrack>{};
    const bool track1_audio = !tracks.empty() && tracks.front().is_audio;
    // cditop.sv audio_cd_in_tray (a user switch there): CD-i Ready discs have a label.
    audio_cd_in_tray_ = track1_audio && !has_cdi_disc_label(*disc);
    slave_.set_disc_present(disc != nullptr, track1_audio);
}

void Bus::set_tray_open(bool open) {
    slave_.set_tray_open(open);
    sync_tray();
}

void Bus::periph_tick(int cpu_cycles) {
    scc_.tick(cpu_cycles);
    // 7 data bits, as scc68070.sv bypass_uart_rx.
    const int byte = pointer2_.tick(cpu_cycles, !scc_.uart_receiver_enabled());
    if (byte >= 0) scc_.uart_receive(uint8_t(byte & 0x7f));
    cdic_.tick_cycles(cpu_cycles);
    vmpeg_.tick_cycles(cpu_cycles);
    nvram_.tick_cycles(cpu_cycles);
    slave_.tick_cycles(cpu_cycles);
}

void Bus::slave_tick_frame() { slave_.tick_frame(); }

void Bus::update_pointer(int dx, int dy, bool button1, bool button2, bool button12) {
    slave_.update_pointer(dx, dy, button1, button2, button12);
}

void Bus::set_pointer(int x, int y, bool button1, bool button2, bool button12) {
    slave_.set_pointer(x, y, button1, button2, button12);
}

AudioStreams Bus::drain_audio() {
    std::vector<int16_t> cdic_pcm = cdic_.drain_audio();
    slave_.attenuate(cdic_pcm);
    // Demuxed by VMPEG from the channel-1 DMA; all of it arrived after any FMA stop.
    std::vector<uint8_t> dvc_bytes = vmpeg_.drain_fma_es_bytes();
    if (vmpeg_.consume_audio_reset()) {
        dvc_audio_.reset();
    }

    dvc_audio_.feed(dvc_bytes);
    // PS audio waits until the FMA playback timer reaches its first PTS.
    const bool hold = vmpeg_.audio_start_pending();
    std::vector<int16_t> dvc_pcm = hold ? std::vector<int16_t>{} : dvc_audio_.decode_all();
    if (!dvc_pcm.empty()) {
        if (!vmpeg_.audio_dsp_enabled()) vmpeg_.set_audio_decoding_started();
        vmpeg_.set_audio_frame_decoded(dvc_audio_.last_frame_header());
    }
    // FMA underflow (vmpeg.sv:737-745), once a DMA has fed the FMA.
    if (dvc_cart_present() && fma_dma_seen_) vmpeg_.audio_field(int32_t(dvc_pcm.size() / 2), field_samples_);
    AudioStreams out;
    out.cdic = std::move(cdic_pcm);
    // dual_ad7528.sv matrix, Q0.8 (256 = unity). The gains power up at 0 and
    // apply as each is written: muted until the driver programs them.
    const int l2l = vmpeg_.mpeg_volume_l2l(), r2r = vmpeg_.mpeg_volume_r2r();
    const int l2r = vmpeg_.mpeg_volume_l2r(), r2l = vmpeg_.mpeg_volume_r2l();
    out.dvc.assign(dvc_pcm.size() & ~std::size_t(1), 0);
    for (std::size_t i = 0; i + 1 < dvc_pcm.size(); i += 2) {
        int l = dvc_pcm[i], r = dvc_pcm[i + 1];
        out.dvc[i] = clamp16((l * l2l + r * r2l) >> 8);
        out.dvc[i + 1] = clamp16((l * l2r + r * r2r) >> 8);
    }
    return out;
}

int Bus::update_interrupts() {
    scc_.set_int1(mcd212_.irq1_asserted());
    scc_.set_in2(slave_.irq2_asserted());

    // cditop.sv SR latch: first requester owns in4 until its request drops.
    bool cdic_req = cdic_.irq4_asserted();
    bool vmpeg_req = vmpeg_.irq_asserted();
    switch (in4_owner_) {
        case In4Owner::kCdic:
            if (!cdic_req) in4_owner_ = In4Owner::kIdle;
            break;
        case In4Owner::kVmpeg:
            if (!vmpeg_req) in4_owner_ = In4Owner::kIdle;
            break;
        case In4Owner::kIdle:
            // VMPEG wins a tie: cditop.sv assigns it last.
            if (cdic_req) in4_owner_ = In4Owner::kCdic;
            if (vmpeg_req) in4_owner_ = In4Owner::kVmpeg;
            break;
    }
    // An owner always has its request up: the latch drops it otherwise.
    scc_.set_in4(in4_owner_ != In4Owner::kIdle);

    return scc_.current_ipl();
}

void Bus::apply_interrupts_now() {
    if (!cpu_) return; // stand-alone Bus (unit tests)
    cpu_->set_irq(update_interrupts());
}

int Bus::interrupt_ack(int level) {
    // Only the external in4 pin goes to its owner; an internal level-4
    // source (e.g. DMA COC at IPL 4) takes the SCC's own iack path.
    if (level == 4 && scc_.in4_asserted()) {
        return (in4_owner_ == In4Owner::kVmpeg) ? vmpeg_.iack_vector() : cdic_.iack_vector();
    }
    return scc_.iack(level);
}

void Bus::perform_cdic_dma() {
    // same_cdi cdicdic_device::regs_w (0x3ff8); address and count from
    // SCC68070 DMA channel 0.
    uint32_t cdic_word_index = (cdic_.dma_control() & 0x3fffu) >> 1;
    uint32_t mem_addr = scc_.dma_memory_address_counter();
    uint32_t words = scc_.dma_transfer_counter();
    bool device_to_memory = (scc_.dma_operation_control() & 0x80) != 0; // OCR_D

    for (uint32_t i = 0; i < words; ++i) {
        uint32_t cdic_off = (cdic_word_index + i) * 2;
        uint32_t mem_off = mem_addr + i * 2;
        if (device_to_memory) {
            write8(mem_off, cdic_.read_ram_byte(cdic_off));
            write8(mem_off + 1, cdic_.read_ram_byte(cdic_off + 1));
        } else {
            cdic_.write_ram_byte(cdic_off, read8(mem_off));
            cdic_.write_ram_byte(cdic_off + 1, read8(mem_off + 1));
        }
    }
    scc_.advance_dma_memory_address_counter(words * 2);
}

void Bus::perform_vmpeg_dma() {
    uint32_t mem_addr = scc_.dma_ch1_memory_address_counter();
    uint32_t words = scc_.dma_ch1_transfer_counter();
    uint8_t ocr = scc_.dma_ch1_operation_control();
    bool device_to_memory = (ocr & 0x80) != 0; // OCR_D

    if (!device_to_memory) {
        bool for_fma = vmpeg_.dma_for_fma(); // read before complete_dma() clears it below
        for (uint32_t i = 0; i < words; ++i) {
            uint32_t mem_off = mem_addr + i * 2;
            uint16_t word = (uint16_t(read8(mem_off)) << 8) | read8(mem_off + 1);
            vmpeg_.feed_dma_word(word);
        }
        if (words > 0 && for_fma) fma_dma_seen_ = true;
    }
    // cditop.sv wires VMPEG only as a DMA sink; the counters still advance.
    scc_.advance_dma_ch1_memory_address_counter(words * 2);
    scc_.complete_dma_ch1_burst(words);
    vmpeg_.complete_dma();
}

bool Bus::plain_byte(uint32_t a, uint8_t& v) const {
    if (in_range(a, kPlane0Base, kPlane0Size)) {
        v = plane0_[a - kPlane0Base];
        return true;
    }
    if (in_range(a, kPlane1Base, kPlane1Size)) {
        v = plane1_[a - kPlane1Base];
        return true;
    }
    if (in_range(a, kBiosBase, kBiosSize)) {
        v = a - kBiosBase < rom_.size() ? rom_[a - kBiosBase] : 0;
        return true;
    }
    if (in_range(a, kExtraBase, kExtraSize)) {
        v = extra_ram_[a - kExtraBase];
        return true;
    }
    if (in_range(a, kNvramBase, kNvramSize)) {
        uint32_t tk_off = 0;
        v = decode_nvram_bus_addr(a, tk_off) ? nvram_.read(tk_off) : 0xff;
        return true;
    }
    if (in_range(a, kCdicRamBase, kCdicRamSize)) {
        v = cdic_.read_ram_byte(a - kCdicRamBase);
        return true;
    }
    if (in_range(a, kDvcCartRamBase, kDvcCartRamSize)) {
        // Non-CPU callers only without a cartridge; the CPU bus-errors first.
        v = dvc_cart_present() ? dvc_ram_[a - kDvcCartRamBase] : 0;
        return true;
    }
    if (in_range(a, kDvcCartRomBase, kDvcCartRomSize)) {
        const uint32_t off = a - kDvcCartRomBase;
        v = off < dvc_rom_.size() ? dvc_rom_[off] : 0;
        return true;
    }
    if (in_range(a, kDvcDecodeRamBase, kDvcDecodeRamSize)) {
        v = vmpeg_.mpeg_ram_enabled() ? dvc_decode_ram_[a - kDvcDecodeRamBase] : 0;
        return true;
    }
    if (in_range(a, kPlayCdiOverlayBase, kPlayCdiOverlayCodeSize) && playcdi_overlay_active()) {
        v = kPlayCdiOverlayCode[a - kPlayCdiOverlayBase];
        return true;
    }
    return false;
}

uint8_t Bus::read8(uint32_t a) {
    if (uint8_t v = 0; plain_byte(a, v)) return v;
    if (in_range(a, kCdicRegBase, kCdicRegSize)) return cdic_.read_reg_byte(a - kCdicRegBase);
    if (in_range(a, kSlaveBase, kSlaveSize)) return slave_.read_byte(a);
    if (in_range(a, kMcd212Base, kMcd212Size)) {
        uint8_t v = mcd212_.read_byte(a - kMcd212Base);
        // A CSR2R read can clear MCD212 int1; push that to the CPU now.
        apply_interrupts_now();
        return v;
    }
    if (in_range(a, kSlaveNvramGapBase, kSlaveNvramGapSize)) return 0xff;
    if (a >= 0x80000000u) return scc_.read_byte(a);
    if (in_range(a, kDvcRegBase, kDvcRegSize)) return vmpeg_.read_byte(a - kDvcRegBase);
    return 0;
}
uint16_t Bus::read16(uint32_t a) { return static_cast<uint16_t>((read8(a) << 8) | read8(a + 1)); }
uint32_t Bus::read32(uint32_t a) { return (static_cast<uint32_t>(read16(a)) << 16) | read16(a + 2); }

uint8_t Bus::peek8(uint32_t a) const {
    if (uint8_t v = 0; plain_byte(a, v)) return v;
    return 0xff; // registers: reading them can have side effects
}
uint16_t Bus::peek16(uint32_t a) const { return static_cast<uint16_t>((peek8(a) << 8) | peek8(a + 1)); }
uint32_t Bus::peek32(uint32_t a) const { return (static_cast<uint32_t>(peek16(a)) << 16) | peek16(a + 2); }

void Bus::write8(uint32_t a, uint8_t v) {
    if (in_range(a, kPlane0Base, kPlane0Size)) {
        plane0_[a - kPlane0Base] = v;
        return;
    }
    if (in_range(a, kPlane1Base, kPlane1Size)) {
        plane1_[a - kPlane1Base] = v;
        return;
    }
    if (in_range(a, kExtraBase, kExtraSize)) {
        extra_ram_[a - kExtraBase] = v;
        return;
    }
    if (in_range(a, kNvramBase, kNvramSize)) {
        uint32_t tk_off = 0;
        if (decode_nvram_bus_addr(a, tk_off)) nvram_.write(tk_off, v);
        return;
    }
    if (in_range(a, kCdicRamBase, kCdicRamSize)) {
        cdic_.write_ram_byte(a - kCdicRamBase, v);
        return;
    }
    if (in_range(a, kCdicRegBase, kCdicRegSize)) {
        cdic_.write_reg_byte(a - kCdicRegBase, v);
        // The DMA control low byte (0x303ff9) completes the word write.
        if (a - kCdicRegBase == 0x3f9) perform_cdic_dma();
        return;
    }
    if (in_range(a, kSlaveBase, kSlaveSize)) {
        slave_.write_byte(a, v);
        sync_tray(); // channel 2's 0x89/0x92
        return;
    }
    if (in_range(a, kMcd212Base, kMcd212Size)) {
        mcd212_.write_byte(a - kMcd212Base, v);
        return;
    }
    if (in_range(a, kSlaveNvramGapBase, kSlaveNvramGapSize)) return; // unmapped, dropped
    if (a >= 0x80000000u) {
        scc_.write_byte(a, v);
        return;
    }
    if (in_range(a, kDvcCartRamBase, kDvcCartRamSize)) {
        if (dvc_cart_present()) dvc_ram_[a - kDvcCartRamBase] = v;
        return;
    }
    if (in_range(a, kDvcDecodeRamBase, kDvcDecodeRamSize)) {
        // Dropped until the firmware enables it (vmpeg.sv:742-747).
        if (vmpeg_.mpeg_ram_enabled()) dvc_decode_ram_[a - kDvcDecodeRamBase] = v;
        return;
    }
    if (in_range(a, kDvcRegBase, kDvcRegSize)) {
        vmpeg_.write_byte(a - kDvcRegBase, v);
        // The one place channel 1 starts: requests change only on these writes.
        if (vmpeg_.dma_active()) perform_vmpeg_dma();
        return;
    }
}
void Bus::write16(uint32_t a, uint16_t v) {
    write8(a, uint8_t(v >> 8));
    write8(a + 1, uint8_t(v));
}
void Bus::write32(uint32_t a, uint32_t v) {
    write16(a, uint16_t(v >> 16));
    write16(a + 2, uint16_t(v));
}

bool Bus::is_decoded(uint32_t a) const {
    // Musashi's double-bus-fault read; faulting here would recurse forever.
    if (a == 0x00ffff01u) return true;
    if (in_range(a, kPlane0Base, kPlane0Size)) return true;
    if (in_range(a, kPlane1Base, kPlane1Size)) return true;
    if (in_range(a, kBiosBase, kBiosSize)) return true;
    // The RTL bus-errors this too; kept decoded until a title needs that.
    if (in_range(a, kExtraBase, kExtraSize)) return true;
    if (in_range(a, kNvramBase, kNvramSize)) return true;
    if (in_range(a, kCdicRamBase, kCdicRamSize)) return true;
    if (in_range(a, kCdicRegBase, kCdicRegSize)) return true;
    if (in_range(a, kSlaveBase, kSlaveSize)) return true;
    if (in_range(a, kMcd212Base, kMcd212Size)) return true;
    if (in_range(a, kSlaveNvramGapBase, kSlaveNvramGapSize)) return true;
    if (a >= 0x80000000u) return true;
    if (in_range(a, kDvcCartRamBase, kDvcCartRamSize)) return dvc_cart_present();
    if (in_range(a, kDvcCartRomBase, kDvcCartRomSize)) return true;
    // cditop.sv bus_err_ram_area5
    if (in_range(a, kDvcDecodeRamBase, kDvcDecodeRamSize)) return vmpeg_.mpeg_ram_enabled();
    if (in_range(a, kDvcRegBase, kDvcRegSize)) return true;
    // bus_err_ram_area3/4: the whole window, not just the overlay's bytes.
    if (in_range(a, kPlayCdiOverlayBase, 0x10000) && playcdi_overlay_active()) return true;
    return false;
}

namespace {
// CDi_MiSTer [zcdi-arbit] average, arbitration included; std::round isn't
// constexpr in C++20.
constexpr double kMeasuredSdramCyclesPerAccess = 3.65;
constexpr int kSdramAccessWaitCycles = static_cast<int>(kMeasuredSdramCyclesPerAccess + 0.5);
} // namespace

void Bus::set_cpu(MusashiCore* cpu) {
    cpu_ = cpu;
    if (cpu)
        g_active_bus = this;
    else if (g_active_bus == this)
        g_active_bus = nullptr; // a newer machine's Bus stays
}

bool Bus::cpu_supervisor() const { return cpu_ && cpu_->supervisor(); }

int Bus::sdram_wait_cycles(uint32_t a) const {
    if (in_range(a, kPlane0Base, kPlane0Size)) return kSdramAccessWaitCycles;
    if (in_range(a, kPlane1Base, kPlane1Size)) return kSdramAccessWaitCycles;
    if (in_range(a, kBiosBase, kBiosSize)) return kSdramAccessWaitCycles;
    if (in_range(a, kDvcCartRamBase, kDvcCartRamSize)) return kSdramAccessWaitCycles;
    if (in_range(a, kDvcCartRomBase, kDvcCartRomSize)) return kSdramAccessWaitCycles;
    if (in_range(a, kDvcDecodeRamBase, kDvcDecodeRamSize)) return kSdramAccessWaitCycles;
    return 0;
}

} // namespace zcdi

namespace {
// rtl/scc68070.sv: supervisor A31:30=10 is on-chip; everything else goes
// out on the 24-bit bus, dropping A24-A31 (software tags pointer top bytes).
uint32_t cpu_bus_address(const zcdi::Bus& bus, uint32_t a) {
    if ((a & 0xc0000000u) == 0x80000000u && bus.cpu_supervisor()) return a;
    return a & 0x00ffffffu;
}
} // namespace

namespace zcdi {

inline const uint8_t* Bus::cpu_read_ptr(uint32_t a, uint32_t size) const {
    if (a + size <= kPlane0Base + kPlane0Size) return &plane0_[a - kPlane0Base];
    if (a >= kPlane1Base && a + size <= kPlane1Base + kPlane1Size) return &plane1_[a - kPlane1Base];
    if (a >= kBiosBase && a - kBiosBase + size <= rom_.size()) return &rom_[a - kBiosBase];
    if (dvc_cart_present() && a >= kDvcCartRamBase && a + size <= kDvcCartRamBase + kDvcCartRamSize) {
        return &dvc_ram_[a - kDvcCartRamBase];
    }
    if (a >= kDvcCartRomBase && a - kDvcCartRomBase + size <= dvc_rom_.size()) return &dvc_rom_[a - kDvcCartRomBase];
    return nullptr;
}

inline uint8_t* Bus::cpu_write_ptr(uint32_t a, uint32_t size) {
    if (a + size <= kPlane0Base + kPlane0Size) return &plane0_[a - kPlane0Base];
    if (a >= kPlane1Base && a + size <= kPlane1Base + kPlane1Size) return &plane1_[a - kPlane1Base];
    if (dvc_cart_present() && a >= kDvcCartRamBase && a + size <= kDvcCartRamBase + kDvcCartRamSize) {
        return &dvc_ram_[a - kDvcCartRamBase];
    }
    return nullptr;
}

} // namespace zcdi

// m68k_pulse_bus_error_at() longjmps, so nothing after it runs.
extern "C" {
unsigned int m68k_read_memory_8(unsigned int addr) {
    zcdi::Bus* bus = zcdi::g_active_bus;
    if (!bus) return 0;
    const uint32_t a = cpu_bus_address(*bus, addr);
    if (const uint8_t* p = bus->cpu_read_ptr(a, 1)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        return p[0];
    }
    if (!bus->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return 0;
    }
    m68k_add_wait_cycles(bus->sdram_wait_cycles(a));
    return bus->read8(a);
}
unsigned int m68k_read_memory_16(unsigned int addr) {
    zcdi::Bus* bus = zcdi::g_active_bus;
    if (!bus) return 0;
    const uint32_t a = cpu_bus_address(*bus, addr);
    if (const uint8_t* p = bus->cpu_read_ptr(a, 2)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        return (unsigned(p[0]) << 8) | p[1];
    }
    if (!bus->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return 0;
    }
    m68k_add_wait_cycles(bus->sdram_wait_cycles(a));
    return bus->read16(a);
}
unsigned int m68k_read_memory_32(unsigned int addr) {
    zcdi::Bus* bus = zcdi::g_active_bus;
    if (!bus) return 0;
    const uint32_t a = cpu_bus_address(*bus, addr);
    const uint32_t a2 = cpu_bus_address(*bus, addr + 2);
    const uint8_t* hi = bus->cpu_read_ptr(a, 2);
    if (const uint8_t* lo = hi ? bus->cpu_read_ptr(a2, 2) : nullptr) {
        m68k_add_wait_cycles(2 * zcdi::kSdramAccessWaitCycles);
        return (unsigned(hi[0]) << 24) | (unsigned(hi[1]) << 16) | (unsigned(lo[0]) << 8) | lo[1];
    }
    // Two 16-bit bus cycles, decoded separately: a straddle faults at word 2.
    if (!bus->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return 0;
    }
    if (!bus->is_decoded(a2)) {
        m68k_pulse_bus_error_at(addr + 2);
        return 0;
    }
    m68k_add_wait_cycles(2 * bus->sdram_wait_cycles(a));
    // Separately normalized, so 0xfffffe wraps to 0 like the 24-bit bus.
    return (uint32_t(bus->read16(a)) << 16) | bus->read16(a2);
}
void m68k_write_memory_8(unsigned int addr, unsigned int v) {
    zcdi::Bus* bus = zcdi::g_active_bus;
    if (!bus) return;
    const uint32_t a = cpu_bus_address(*bus, addr);
    if (uint8_t* p = bus->cpu_write_ptr(a, 1)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        p[0] = uint8_t(v);
        return;
    }
    if (!bus->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return;
    }
    m68k_add_wait_cycles(bus->sdram_wait_cycles(a));
    bus->write8(a, uint8_t(v));
}
void m68k_write_memory_16(unsigned int addr, unsigned int v) {
    zcdi::Bus* bus = zcdi::g_active_bus;
    if (!bus) return;
    const uint32_t a = cpu_bus_address(*bus, addr);
    if (uint8_t* p = bus->cpu_write_ptr(a, 2)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        p[0] = uint8_t(v >> 8);
        p[1] = uint8_t(v);
        return;
    }
    if (!bus->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return;
    }
    m68k_add_wait_cycles(bus->sdram_wait_cycles(a));
    bus->write16(a, uint16_t(v));
}
void m68k_write_memory_32(unsigned int addr, unsigned int v) {
    zcdi::Bus* bus = zcdi::g_active_bus;
    if (!bus) return;
    const uint32_t a = cpu_bus_address(*bus, addr);
    const uint32_t a2 = cpu_bus_address(*bus, addr + 2);
    uint8_t* hi = bus->cpu_write_ptr(a, 2);
    if (uint8_t* lo = hi ? bus->cpu_write_ptr(a2, 2) : nullptr) {
        m68k_add_wait_cycles(2 * zcdi::kSdramAccessWaitCycles);
        hi[0] = uint8_t(v >> 24);
        hi[1] = uint8_t(v >> 16);
        lo[0] = uint8_t(v >> 8);
        lo[1] = uint8_t(v);
        return;
    }
    // As in m68k_read_memory_32.
    if (!bus->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return;
    }
    if (!bus->is_decoded(a2)) {
        m68k_pulse_bus_error_at(addr + 2);
        return;
    }
    m68k_add_wait_cycles(2 * bus->sdram_wait_cycles(a));
    bus->write16(a, uint16_t(v >> 16));
    bus->write16(a2, uint16_t(v));
}

// Side-effect free, so disassembling can't disturb the machine.
unsigned int m68k_read_disassembler_8(unsigned int a) { return zcdi::g_active_bus ? zcdi::g_active_bus->peek8(a) : 0; }
unsigned int m68k_read_disassembler_16(unsigned int a) {
    return zcdi::g_active_bus ? zcdi::g_active_bus->peek16(a) : 0;
}
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return zcdi::g_active_bus ? zcdi::g_active_bus->peek32(a) : 0;
}
}
