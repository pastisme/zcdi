#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cdic/cdic.h"
#include "core/pointing_device.h"
#include "disc/disc.h"
#include "dvc/dvc_audio.h"
#include "dvc/dvc_video.h"
#include "mcd212/mcd212.h"
#include "mk48t08/mk48t08.h"
#include "scc68070/scc68070.h"
#include "slave/slave.h"
#include "vmpeg/vmpeg.h"

namespace zcdi {

class MusashiCore;

// One drain's worth of decoded audio per source, interleaved stereo S16.
struct AudioStreams {
    std::vector<int16_t> cdic;
    std::vector<int16_t> dvc;
};

// RAM goes through ram_state_io().
struct BusSaveState {
    // cditop.sv's irq_in4owner SR latch.
    enum class In4Owner { kIdle, kCdic, kVmpeg } in4_owner_ = In4Owner::kIdle;
    bool last_reset_by_slave_ = false;

    // FMA underflow is only reported once a DMA has fed the FMA.
    bool fma_dma_seen_ = false;
};

class Bus : private BusSaveState {
public:
    Bus();

    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    void write8(uint32_t addr, uint8_t v);
    void write16(uint32_t addr, uint16_t v);
    void write32(uint32_t addr, uint32_t v);

    // Memory without side effects (no register reads); 0xff for registers.
    uint8_t peek8(uint32_t addr) const;
    uint16_t peek16(uint32_t addr) const;
    uint32_t peek32(uint32_t addr) const;

    // A cdimono1 BIOS dump (cdi200.rom, cdi220b.rom), loaded as-is.
    void load_bios(const std::span<const std::byte>& content);

    // vmpega.rom, up to 256 KB. Without it there is no DVC cartridge.
    void load_dvc(const std::span<const std::byte>& content);
    std::span<const std::byte> dvc_ram_data() const { return std::as_bytes(std::span(dvc_ram_)); }

    // Verbatim, truncated to Mk48t08::kSize; persistence is the frontend's job.
    void load_nvram(const std::span<const std::byte>& content);
    std::span<const std::byte> nvram_data() const { return std::as_bytes(std::span(nvram_.data(), nvram_.size())); }
    void set_rtc_boot_time(std::time_t t) { nvram_.set_boot_time(t); }

    std::span<const std::byte> plane0_data() const { return std::as_bytes(std::span(plane0_)); }
    std::span<const std::byte> plane1_data() const { return std::as_bytes(std::span(plane1_)); }
    // Copies the BIOS's SSP/PC to RAM 0; call before the CPU's reset fetch.
    void prepare_reset_vectors();

    // CD-RTOS boot console bytes since the last call; draining clears them.
    std::string drain_uart_output();

    // One field, as Mcd212::begin_field / end_of_line / end_field.
    int video_begin_field(int lines);
    void video_end_of_line(int line);
    void video_end_field();
    const uint32_t* video_framebuffer() const { return mcd212_.framebuffer(); }
    // The shown part starts top-left; its row pitch stays video_width().
    int video_active_width() const { return mcd212_.active_width(); }
    int video_active_height() const { return mcd212_.output_height(); }
    double video_display_aspect() const { return mcd212_.display_aspect(); }
    int video_width() const { return mcd212_.fb_width(); }
    int video_height() const { return mcd212_.fb_height(); }

    // Non-owning; Machine keeps the Disc alive. nullptr unmounts.
    void set_disc(Disc* disc);

    // The disc stays set while the tray is open, but the CDIC can't read it.
    void set_tray_open(bool open);
    bool tray_open() const { return slave_.tray_open(); }

    // Fails the load if the BIOS, DVC ROM or options differ; the disc is
    // Machine's to check.
    template <class T>
    void state_io(T& archive);
    // The BIOS, DVC ROM and options part, which state_io() starts with.
    template <class T>
    void identity_io(T& archive);

    // The subsets Machine keeps across cold boot/reset. Written in place, so
    // nvram_data()'s pointer stays valid.
    template <class T>
    void nvram_state_io(T& archive);
    template <class T>
    void ram_state_io(T& archive);

    // Set once at construction.
    void set_auto_play_enabled(bool enabled) { auto_play_enabled_ = enabled; }

    // Set once at construction.
    void set_video_standard(VideoStandard standard) {
        slave_.set_video_standard(standard);
        vmpeg_.set_video_standard(standard);
    }

    bool consume_slave_reset_request() { return slave_.consume_reset_request(); }

    // While set, the playcdi overlay stays off (RTL last_reset_by_slave).
    void note_reset_by_slave() { last_reset_by_slave_ = true; }

    bool playcdi_overlay_active() const {
        return auto_play_enabled_ && disc_mounted_ && !audio_cd_in_tray_ && !last_reset_by_slave_;
    }

    // False means the CPU bus-errors at `a`; ROMs count as decoded for writes.
    bool is_decoded(uint32_t a) const;

    // Direct pointers for plain memory, or nullptr to take the full path.
    inline const uint8_t* cpu_read_ptr(uint32_t a, uint32_t size) const;
    inline uint8_t* cpu_write_ptr(uint32_t a, uint32_t size);

    // SR.S of the attached CPU; false without one.
    bool cpu_supervisor() const;

    void periph_tick(int cpu_cycles);

    bool cdic_disc_active() const { return cdic_.disc_active(); }

    void slave_tick_frame();

    // Call every field before video_end_field().
    void dvc_tick_frame(double refresh_rate_hz);

    // Audio since the last drain, one stream per source; Mixer sums them.
    AudioStreams drain_audio();

    // Front "INPUT" on the SLAVE's SCI, back "INPUT 2" on the SCC68070 UART.
    enum class Socket : uint8_t { kFront, kBack };
    // Not saved; the same type keeps the plugged device.
    void plug(Socket socket, std::optional<DeviceId> id);
    std::optional<DeviceId> plugged(Socket socket) const;
    // Once per frame, before run_frame().
    void set_socket_input(Socket socket, const PointingDevice::Input& input);

    // Returns the CPU interrupt level; Machine polls it after every CPU chunk.
    int update_interrupts();

    void set_cpu(MusashiCore* cpu);

    int interrupt_ack(int level);

private:
    friend class MachineInspector;

    // Memory without side effects (RAM, ROM, NVRAM, CDIC RAM, overlay);
    // false for registers and unmapped addresses.
    bool plain_byte(uint32_t a, uint8_t& v) const;

    void perform_cdic_dma();
    void perform_vmpeg_dma();

    bool auto_play_enabled_ = false;
    bool disc_mounted_ = false;
    bool audio_cd_in_tray_ = false;

    // Handed to the CDIC only while the tray is closed: sync_tray() after
    // anything that can move the tray.
    Disc* disc_ = nullptr;
    void sync_tray() { cdic_.set_disc(slave_.tray_closed() ? disc_ : nullptr); }

    std::vector<uint8_t> plane0_; // 0x000000-0x07ffff: video plane 0 DRAM
    std::vector<uint8_t> plane1_; // 0x200000-0x27ffff: video plane 1 DRAM
    std::vector<uint8_t> extra_ram_; // 0x500000-0x57ffff: additional system RAM
    std::vector<uint8_t> dvc_ram_; // 0xd00000-0xdfffff: DVC cartridge RAM (cartridge only)
    std::vector<uint8_t> dvc_rom_; // 0xe40000-0xe7ffff: DVC cartridge ROM (vmpega.rom)
    bool dvc_cart_present() const { return !dvc_rom_.empty(); }
    // 0xe80000-0xefffff, hidden until vmpeg_.mpeg_ram_enabled().
    std::vector<uint8_t> dvc_decode_ram_;
    mamecdi::Mk48t08 nvram_; // 0x320000-0x323fff: MK48T08 timekeeper
    std::vector<uint8_t> rom_; // 0x400000-0x47ffff: BIOS ROM
    // Save-state fingerprints.
    uint32_t rom_crc_ = 0;
    uint32_t dvc_rom_crc_ = 0;
    int32_t field_samples_ = 882; // 44.1 kHz samples per field, set by dvc_tick_frame

    void apply_interrupts_now();

    MusashiCore* cpu_ = nullptr;

    mamecdi::Mcd212 mcd212_; // 0x4fffe0-0x4fffff
    mamecdi::Scc68070Periph scc_; // 0x80000000+
    mamecdi::SlaveHle slave_; // 0x310000-0x317fff
    std::optional<PointingDevice> front_device_;
    std::optional<PointingDevice> back_device_;
    std::optional<PointingDevice>& socket(Socket s) { return s == Socket::kFront ? front_device_ : back_device_; }
    const std::optional<PointingDevice>& socket(Socket s) const {
        return s == Socket::kFront ? front_device_ : back_device_;
    }
    template <class T>
    void socket_state_io(T& archive, Socket s);
    mamecdi::Cdic cdic_; // 0x300000-0x303fff
    mamecdi::VmpegDevice vmpeg_; // 0xe00000-0xe3ffff, FMA/FMV registers
    dvc::DvcVideo dvc_video_;
    dvc::DvcAudio dvc_audio_;
};

} // namespace zcdi
