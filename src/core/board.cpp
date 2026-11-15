#include "core/board.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/cpu/musashi_core.h"
#include "core/playcdi_overlay.h"
#include "core/state_archive.h"
#include "core/util.h"
#include "m68k.h"

namespace zcdi {

namespace {
// The Board with a CPU attached; Musashi's memory callbacks take no user pointer.
Board* g_active_board = nullptr;

// Green Book disc label: LBA 16, record type 1, "CD-I ".
bool has_cdi_disc_label(Disc& disc) {
    RawSector s{};
    if (!disc.read_sector(16, s) || s.data[15] != 2) return false;
    const uint8_t* label = s.data + kSyncSize + kHeaderSize + kSubheaderSize;
    return label[0] == 1 && std::memcmp(label + 1, "CD-I ", 5) == 0;
}

using namespace address_map;

constexpr int kSdramAccessWaitCycles = 2;

// Only the even byte lane of the NVRAM window decodes.
static_assert(kNvram.size / 2 == chips::Mk48t08::kSize);
bool decode_nvram_offset(uint32_t offset, uint32_t& tk_off) {
    if (offset & 1u) return false;
    tk_off = offset >> 1;
    return true;
}

} // namespace

Board::Board()
    : plane0_(kPlane0.size, 0),
      plane1_(kPlane1.size, 0),
      extra_ram_(kExtraRam.size, 0),
      dvc_ram_(kDvcCartRam.size, 0),
      dvc_decode_ram_(kDvcDecodeRam.size, 0) {}

void Board::load_nvram(const std::span<const std::byte>& content) {
    nvram_.load({reinterpret_cast<const uint8_t*>(content.data()), content.size()});
}

void Board::load_bios(const std::span<const std::byte>& content) {
    // The circulated dumps are already big-endian.
    const auto* p = reinterpret_cast<const uint8_t*>(content.data());
    rom_.assign(p, p + content.size());
    rom_crc_ = state_crc32(rom_);
}

void Board::load_dvc(const std::span<const std::byte>& content) {
    const auto* p = reinterpret_cast<const uint8_t*>(content.data());
    dvc_rom_.assign(p, p + std::min(content.size(), std::size_t{kDvcCartRom.size}));
    dvc_rom_crc_ = state_crc32(dvc_rom_);
    dvc_.set_cartridge_present(dvc_cart_present());
}

void Board::prepare_reset_vectors() {
    if (rom_.size() < 8) return; // no BIOS loaded yet
    for (int i = 0; i < 8; ++i)
        plane0_[i] = rom_[i];
}

std::string Board::drain_uart_output() { return scc_.drain_uart_output(); }

void Board::dvc_tick_frame(double refresh_rate_hz) {
    if (dvc_.tick_field(refresh_rate_hz)) mcd212_.set_dvc_frame(nullptr, 0, 0);
}

int Board::video_begin_field(int lines) { return mcd212_.begin_field(lines); }

void Board::video_end_of_line(int line) {
    mcd212_.end_of_line(line, plane0_.data(), plane1_.data(), plane0_.size());
    dvc_.end_of_line(line, mcd212_.first_active_line(), mcd212_.active_height());
}

void Board::video_end_field() {
    if (dvc_.has_frame()) {
        mcd212_.set_dvc_frame(dvc_.frame_rgba(), dvc_.width(), dvc_.height());
    }
    mcd212_.set_dvc_attached(dvc_cart_present());
    const dvc::Dvc::DisplayWindow w = dvc_.display_window();
    mcd212_.set_dvc_window(w.scrpos_x, w.scrpos_y, w.decwin_w, w.decwin_h, w.decoff_x, w.decoff_y, w.show,
                           w.vcd_pixel_clock);
    mcd212_.end_field();
}

template <class T>
void Board::state_io(T& archive) {
    archive.begin_section(state_tag("BORD"));
    identity_io(archive);
    if (archive.failed()) return;

    ram_state_io(archive);
    archive.io_struct(static_cast<BoardSaveState&>(*this));

    archive.end_section();

    scc_.state_io(archive);
    mcd212_.state_io(archive);
    slave_.state_io(archive);
    socket_state_io(archive, Socket::kFront);
    socket_state_io(archive, Socket::kBack);
    cdic_.state_io(archive);
    dvc_.state_io(archive);
    nvram_.state_io(archive);
    if constexpr (T::kLoading) {
        sync_tray();
        // The load replaced the picture the overlay pointed into.
        mcd212_.rebind_dvc_frame(dvc_.frame_rgba(), size_t(dvc_.width()) * size_t(dvc_.height()) * 4);
    }
}
ZCDI_STATE_IO_INSTANTIATE(Board);

template <class T>
void Board::identity_io(T& archive) {
    uint32_t rom_crc = rom_crc_, dvc_rom_crc = dvc_rom_crc_;
    bool auto_play = auto_play_enabled_;
    archive.io(rom_crc);
    archive.io(dvc_rom_crc);
    archive.io(auto_play);
    archive.check(rom_crc == rom_crc_);
    archive.check(dvc_rom_crc == dvc_rom_crc_);
    archive.check(auto_play == auto_play_enabled_);
}
template void Board::identity_io<StateReader>(StateReader&);

template <class T>
void Board::nvram_state_io(T& archive) {
    nvram_.state_io(archive);
}

template <class T>
void Board::ram_state_io(T& archive) {
    archive.io_fixed(plane0_);
    archive.io_fixed(plane1_);
    archive.io_fixed(extra_ram_);
    // identity_io() has matched the cartridge already.
    if (dvc_cart_present()) archive.io_fixed(dvc_ram_);
    archive.io_fixed(dvc_decode_ram_);
}

template void Board::nvram_state_io<StateWriter>(StateWriter&);
template void Board::nvram_state_io<StateReader>(StateReader&);
template void Board::ram_state_io<StateWriter>(StateWriter&);
template void Board::ram_state_io<StateReader>(StateReader&);

void Board::set_disc(Disc* disc) {
    disc_ = disc;
    sync_tray();
    disc_mounted_ = disc != nullptr;
    auto tracks = disc ? disc->toc_tracks() : std::vector<TocTrack>{};
    const bool track1_audio = !tracks.empty() && tracks.front().is_audio;
    // Audio CD in the tray: CD-i Ready discs start with an audio track but have a CD-i label.
    audio_cd_in_tray_ = track1_audio && !has_cdi_disc_label(*disc);
    slave_.set_disc_present(disc != nullptr, track1_audio);
}

void Board::set_tray_open(bool open) {
    slave_.set_tray_open(open);
    sync_tray();
}

void Board::periph_tick(int cpu_cycles) {
    scc_.tick(cpu_cycles);
    if (front_device_) {
        // Devices send at 1200 baud.
        const int byte = front_device_->tick(cpu_cycles, slave_.front_rts_negated());
        if (byte >= 0) slave_.sci_receive(uint8_t(byte), slave_.sci_at_9600_baud());
    }
    if (back_device_) {
        // 7 data bits.
        const int byte = back_device_->tick(cpu_cycles, slave_.back_rts_negated());
        if (byte >= 0) scc_.uart_receive(uint8_t(byte & 0x7f));
    }
    cdic_.tick_cycles(cpu_cycles);
    dvc_.tick_cycles(cpu_cycles);
    nvram_.tick_cycles(cpu_cycles);
    slave_.tick_cycles(cpu_cycles);
}

void Board::slave_tick_frame() {
    const bool was_closed = slave_.tray_closed();
    slave_.tick_frame();
    if (slave_.tray_closed() != was_closed) sync_tray();
}

void Board::plug(Socket s, std::optional<DeviceId> id) {
    std::optional<PointingDevice>& device = socket(s);
    if (device && id && device->id() == *id) return;
    if (id) {
        device.emplace(*id);
    } else {
        device.reset();
    }
}

std::optional<DeviceId> Board::plugged(Socket s) const {
    const std::optional<PointingDevice>& device = socket(s);
    if (!device) return std::nullopt;
    return device->id();
}

void Board::set_socket_input(Socket s, const PointingDevice::Input& input) {
    if (std::optional<PointingDevice>& device = socket(s)) device->set_input(input);
}

// A state made with another device keeps the current one, freshly plugged.
template <class T>
void Board::socket_state_io(T& archive, Socket s) {
    std::optional<PointingDevice>& device = socket(s);
    archive.begin_section(state_tag("SOCK"));
    bool present = device.has_value();
    uint8_t id = present ? uint8_t(device->id()) : 0;
    archive.io(present);
    archive.io(id);
    if constexpr (T::kLoading) {
        if (present && device && uint8_t(device->id()) == id) {
            device->state_io(archive);
        } else {
            if (present) PointingDevice(DeviceId(id)).state_io(archive);
            if (device) device.emplace(device->id());
        }
    } else if (present) {
        device->state_io(archive);
    }
    archive.end_section();
}

AudioStreams Board::drain_audio() {
    AudioStreams out;
    out.cdic = cdic_.drain_audio();
    slave_.attenuate(out.cdic);
    out.dvc = dvc_.drain_audio();
    return out;
}

int Board::update_interrupts() {
    scc_.set_int1(mcd212_.irq1_asserted());
    scc_.set_in2(slave_.irq2_asserted());

    // SR latch: the first requester owns in4 until its request drops.
    bool cdic_req = cdic_.irq4_asserted();
    bool vmpeg_req = dvc_.irq_asserted();
    switch (in4_owner_) {
        case In4Owner::kCdic:
            if (!cdic_req) in4_owner_ = In4Owner::kIdle;
            break;
        case In4Owner::kVmpeg:
            if (!vmpeg_req) in4_owner_ = In4Owner::kIdle;
            break;
        case In4Owner::kIdle:
            // VMPEG wins a tie.
            if (cdic_req) in4_owner_ = In4Owner::kCdic;
            if (vmpeg_req) in4_owner_ = In4Owner::kVmpeg;
            break;
    }
    // An owner always has its request up: the latch drops it otherwise.
    scc_.set_in4(in4_owner_ != In4Owner::kIdle);

    return scc_.current_ipl();
}

void Board::apply_interrupts_now() {
    if (!cpu_) return; // stand-alone Board (unit tests)
    cpu_->set_irq(update_interrupts());
}

int Board::interrupt_ack(int level) {
    // Only the external in4 pin goes to its owner; an internal level-4
    // source (e.g. DMA COC at IPL 4) takes the SCC's own iack path.
    if (level == 4 && scc_.in4_asserted()) {
        return (in4_owner_ == In4Owner::kVmpeg) ? dvc_.iack_vector() : cdic_.iack_vector();
    }
    return scc_.iack(level);
}

void Board::perform_cdic_dma() {
    // Started by a DMA control write; address and count from SCC68070 DMA
    // channel 0.
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

void Board::perform_vmpeg_dma() {
    uint32_t mem_addr = scc_.dma_ch1_memory_address_counter();
    uint32_t words = scc_.dma_ch1_transfer_counter();
    uint8_t ocr = scc_.dma_ch1_operation_control();
    bool device_to_memory = (ocr & 0x80) != 0; // OCR_D

    if (!device_to_memory) {
        for (uint32_t i = 0; i < words; ++i) {
            uint32_t mem_off = mem_addr + i * 2;
            uint16_t word = (uint16_t(read8(mem_off)) << 8) | read8(mem_off + 1);
            dvc_.feed_dma_word(word);
        }
    }
    // VMPEG is wired only as a DMA sink; the counters still advance.
    scc_.advance_dma_ch1_memory_address_counter(words * 2);
    scc_.complete_dma_ch1_burst(words);
    dvc_.complete_dma(device_to_memory ? 0 : words);
}

bool Board::plain_byte(uint32_t a, uint8_t& v) const { return plain_byte(decode(a), v); }

bool Board::plain_byte(const Decoded& d, uint8_t& v) const {
    switch (d.region) {
        case Region::kPlane0: v = plane0_[d.offset]; return true;
        case Region::kPlane1: v = plane1_[d.offset]; return true;
        case Region::kBios: v = d.offset < rom_.size() ? rom_[d.offset] : 0; return true;
        case Region::kExtraRam: v = extra_ram_[d.offset]; return true;
        case Region::kNvram: {
            uint32_t tk_off = 0;
            v = decode_nvram_offset(d.offset, tk_off) ? nvram_.read(tk_off) : 0xff;
            return true;
        }
        case Region::kCdicRam: v = cdic_.read_ram_byte(d.offset); return true;
        // Non-CPU callers only without a cartridge; the CPU bus-errors first.
        case Region::kDvcCartRam: v = dvc_cart_present() ? dvc_ram_[d.offset] : 0; return true;
        case Region::kDvcCartRom: v = d.offset < dvc_rom_.size() ? dvc_rom_[d.offset] : 0; return true;
        case Region::kDvcDecodeRam: v = dvc_.mpeg_ram_enabled() ? dvc_decode_ram_[d.offset] : 0; return true;
        case Region::kPlayCdiOverlay:
            if (d.offset >= kPlayCdiOverlayCodeSize || !playcdi_overlay_active()) return false;
            v = kPlayCdiOverlayCode[d.offset];
            return true;
        default: return false;
    }
}

uint8_t Board::read8(uint32_t a) {
    const Decoded d = decode(a);
    if (uint8_t v = 0; plain_byte(d, v)) return v;
    switch (d.region) {
        case Region::kCdicRegs: return cdic_.read_reg_byte(d.offset);
        case Region::kSlave: return slave_.read_byte(a);
        case Region::kMcd212: {
            const uint8_t v = mcd212_.read_byte(d.offset);
            // A CSR2R read can clear MCD212 int1; push that to the CPU now.
            apply_interrupts_now();
            return v;
        }
        case Region::kSlaveNvramGap: return 0xff;
        case Region::kScc: return scc_.read_byte(a);
        case Region::kDvcRegs: return dvc_.read_byte(d.offset);
        default: return 0;
    }
}
uint16_t Board::read16(uint32_t a) { return static_cast<uint16_t>((read8(a) << 8) | read8(a + 1)); }
uint32_t Board::read32(uint32_t a) { return (static_cast<uint32_t>(read16(a)) << 16) | read16(a + 2); }

uint8_t Board::peek8(uint32_t a) const {
    if (uint8_t v = 0; plain_byte(a, v)) return v;
    return 0xff; // registers: reading them can have side effects
}
uint16_t Board::peek16(uint32_t a) const { return static_cast<uint16_t>((peek8(a) << 8) | peek8(a + 1)); }
uint32_t Board::peek32(uint32_t a) const { return (static_cast<uint32_t>(peek16(a)) << 16) | peek16(a + 2); }

void Board::write8(uint32_t a, uint8_t v) {
    const Decoded d = decode(a);
    switch (d.region) {
        case Region::kPlane0: plane0_[d.offset] = v; break;
        case Region::kPlane1: plane1_[d.offset] = v; break;
        case Region::kExtraRam: extra_ram_[d.offset] = v; break;
        case Region::kNvram: {
            uint32_t tk_off = 0;
            if (decode_nvram_offset(d.offset, tk_off)) nvram_.write(tk_off, v);
            break;
        }
        case Region::kCdicRam: cdic_.write_ram_byte(d.offset, v); break;
        case Region::kCdicRegs:
            cdic_.write_reg_byte(d.offset, v);
            // The low byte completes the word write.
            if (d.offset == chips::cdic::kRegDmaControl + 1) perform_cdic_dma();
            break;
        case Region::kSlave:
            slave_.write_byte(a, v);
            sync_tray(); // channel 2's 0x89/0x92
            break;
        case Region::kMcd212: mcd212_.write_byte(d.offset, v); break;
        case Region::kScc: scc_.write_byte(a, v); break;
        case Region::kDvcCartRam:
            if (dvc_cart_present()) dvc_ram_[d.offset] = v;
            break;
        case Region::kDvcDecodeRam:
            // Dropped until the firmware enables it.
            if (dvc_.mpeg_ram_enabled()) dvc_decode_ram_[d.offset] = v;
            break;
        case Region::kDvcRegs:
            dvc_.write_byte(d.offset, v);
            // The one place channel 1 starts: requests change only on these writes.
            if (dvc_.dma_active()) perform_vmpeg_dma();
            break;
        default: break; // ROMs, the overlay, the SLAVE NVRAM gap, unmapped
    }
}
void Board::write16(uint32_t a, uint16_t v) {
    write8(a, uint8_t(v >> 8));
    write8(a + 1, uint8_t(v));
}
void Board::write32(uint32_t a, uint32_t v) {
    write16(a, uint16_t(v >> 16));
    write16(a + 2, uint16_t(v));
}

bool Board::is_decoded(uint32_t a) const {
    // Musashi's double-bus-fault read; faulting here would recurse forever.
    if (a == 0x00ffff01u) return true;
    switch (decode(a).region) {
        case Region::kNone: return false;
        case Region::kDvcCartRam: return dvc_cart_present();
        case Region::kDvcDecodeRam: return dvc_.mpeg_ram_enabled();
        // The whole window, not just the overlay's bytes.
        case Region::kPlayCdiOverlay: return playcdi_overlay_active();
        // Real hardware may bus-error in the extra RAM; kept decoded until a title needs that.
        default: return true;
    }
}

void Board::set_cpu(MusashiCore* cpu) {
    cpu_ = cpu;
    if (cpu)
        g_active_board = this;
    else if (g_active_board == this)
        g_active_board = nullptr; // a newer machine's Board stays
}

bool Board::cpu_supervisor() const { return cpu_ && cpu_->supervisor(); }

int Board::sdram_wait_cycles(uint32_t a) const { return decode(a).sdram ? kSdramAccessWaitCycles : 0; }

} // namespace zcdi

namespace {
// Supervisor A31:30=10 is on-chip; everything else goes
// out on the 24-bit bus, dropping A24-A31 (software tags pointer top bytes).
uint32_t cpu_bus_address(const zcdi::Board& board, uint32_t a) {
    if ((a & 0xc0000000u) == 0x80000000u && board.cpu_supervisor()) return a;
    return a & 0x00ffffffu;
}
} // namespace

namespace zcdi {

inline const uint8_t* Board::cpu_read_ptr(uint32_t a, uint32_t size) const {
    if (a + size <= kPlane0.base + kPlane0.size) return &plane0_[a - kPlane0.base];
    if (a >= kPlane1.base && a + size <= kPlane1.base + kPlane1.size) return &plane1_[a - kPlane1.base];
    if (a >= kBios.base && a - kBios.base + size <= rom_.size()) return &rom_[a - kBios.base];
    if (dvc_cart_present() && a >= kDvcCartRam.base && a + size <= kDvcCartRam.base + kDvcCartRam.size) {
        return &dvc_ram_[a - kDvcCartRam.base];
    }
    if (a >= kDvcCartRom.base && a - kDvcCartRom.base + size <= dvc_rom_.size()) return &dvc_rom_[a - kDvcCartRom.base];
    return nullptr;
}

inline uint8_t* Board::cpu_write_ptr(uint32_t a, uint32_t size) {
    if (a + size <= kPlane0.base + kPlane0.size) return &plane0_[a - kPlane0.base];
    if (a >= kPlane1.base && a + size <= kPlane1.base + kPlane1.size) return &plane1_[a - kPlane1.base];
    if (dvc_cart_present() && a >= kDvcCartRam.base && a + size <= kDvcCartRam.base + kDvcCartRam.size) {
        return &dvc_ram_[a - kDvcCartRam.base];
    }
    return nullptr;
}

} // namespace zcdi

// m68k_pulse_bus_error_at() longjmps, so nothing after it runs.
extern "C" {
unsigned int m68k_read_memory_8(unsigned int addr) {
    zcdi::Board* board = zcdi::g_active_board;
    if (!board) return 0;
    const uint32_t a = cpu_bus_address(*board, addr);
    if (const uint8_t* p = board->cpu_read_ptr(a, 1)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        return p[0];
    }
    if (!board->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return 0;
    }
    m68k_add_wait_cycles(board->sdram_wait_cycles(a));
    return board->read8(a);
}
unsigned int m68k_read_memory_16(unsigned int addr) {
    zcdi::Board* board = zcdi::g_active_board;
    if (!board) return 0;
    const uint32_t a = cpu_bus_address(*board, addr);
    if (const uint8_t* p = board->cpu_read_ptr(a, 2)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        return (unsigned(p[0]) << 8) | p[1];
    }
    if (!board->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return 0;
    }
    m68k_add_wait_cycles(board->sdram_wait_cycles(a));
    return board->read16(a);
}
unsigned int m68k_read_memory_32(unsigned int addr) {
    zcdi::Board* board = zcdi::g_active_board;
    if (!board) return 0;
    const uint32_t a = cpu_bus_address(*board, addr);
    const uint32_t a2 = cpu_bus_address(*board, addr + 2);
    const uint8_t* hi = board->cpu_read_ptr(a, 2);
    if (const uint8_t* lo = hi ? board->cpu_read_ptr(a2, 2) : nullptr) {
        m68k_add_wait_cycles(2 * zcdi::kSdramAccessWaitCycles);
        return (unsigned(hi[0]) << 24) | (unsigned(hi[1]) << 16) | (unsigned(lo[0]) << 8) | lo[1];
    }
    // Two 16-bit bus cycles, decoded separately: a straddle faults at word 2.
    if (!board->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return 0;
    }
    if (!board->is_decoded(a2)) {
        m68k_pulse_bus_error_at(addr + 2);
        return 0;
    }
    m68k_add_wait_cycles(2 * board->sdram_wait_cycles(a));
    // Separately normalized, so 0xfffffe wraps to 0 like the 24-bit bus.
    return (uint32_t(board->read16(a)) << 16) | board->read16(a2);
}
void m68k_write_memory_8(unsigned int addr, unsigned int v) {
    zcdi::Board* board = zcdi::g_active_board;
    if (!board) return;
    const uint32_t a = cpu_bus_address(*board, addr);
    if (uint8_t* p = board->cpu_write_ptr(a, 1)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        p[0] = uint8_t(v);
        return;
    }
    if (!board->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return;
    }
    m68k_add_wait_cycles(board->sdram_wait_cycles(a));
    board->write8(a, uint8_t(v));
}
void m68k_write_memory_16(unsigned int addr, unsigned int v) {
    zcdi::Board* board = zcdi::g_active_board;
    if (!board) return;
    const uint32_t a = cpu_bus_address(*board, addr);
    if (uint8_t* p = board->cpu_write_ptr(a, 2)) {
        m68k_add_wait_cycles(zcdi::kSdramAccessWaitCycles);
        p[0] = uint8_t(v >> 8);
        p[1] = uint8_t(v);
        return;
    }
    if (!board->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return;
    }
    m68k_add_wait_cycles(board->sdram_wait_cycles(a));
    board->write16(a, uint16_t(v));
}
void m68k_write_memory_32(unsigned int addr, unsigned int v) {
    zcdi::Board* board = zcdi::g_active_board;
    if (!board) return;
    const uint32_t a = cpu_bus_address(*board, addr);
    const uint32_t a2 = cpu_bus_address(*board, addr + 2);
    uint8_t* hi = board->cpu_write_ptr(a, 2);
    if (uint8_t* lo = hi ? board->cpu_write_ptr(a2, 2) : nullptr) {
        m68k_add_wait_cycles(2 * zcdi::kSdramAccessWaitCycles);
        hi[0] = uint8_t(v >> 24);
        hi[1] = uint8_t(v >> 16);
        lo[0] = uint8_t(v >> 8);
        lo[1] = uint8_t(v);
        return;
    }
    // As in m68k_read_memory_32.
    if (!board->is_decoded(a)) {
        m68k_pulse_bus_error_at(addr);
        return;
    }
    if (!board->is_decoded(a2)) {
        m68k_pulse_bus_error_at(addr + 2);
        return;
    }
    m68k_add_wait_cycles(2 * board->sdram_wait_cycles(a));
    board->write16(a, uint16_t(v >> 16));
    board->write16(a2, uint16_t(v));
}

// Side-effect free, so disassembling can't disturb the machine.
unsigned int m68k_read_disassembler_8(unsigned int a) {
    return zcdi::g_active_board ? zcdi::g_active_board->peek8(a) : 0;
}
unsigned int m68k_read_disassembler_16(unsigned int a) {
    return zcdi::g_active_board ? zcdi::g_active_board->peek16(a) : 0;
}
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return zcdi::g_active_board ? zcdi::g_active_board->peek32(a) : 0;
}
}
