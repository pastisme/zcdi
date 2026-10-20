#pragma once
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/bus.h"
#include "core/cpu/musashi_core.h"
#include "core/state_archive.h"
#include "disc/disc.h"

namespace zcdi {

// Frames per second for a player of `standard`; call run_frame() at this rate.
constexpr double refresh_rate(VideoStandard standard) { return standard == VideoStandard::kNtsc ? 60.0 : 50.0; }

// Lines per field, non-interlaced (CDi_MiSTer video_timing.sv).
constexpr int field_lines(VideoStandard standard) { return standard == VideoStandard::kNtsc ? 262 : 312; }

// Not thread-safe.
class Machine {
public:
    // Optional parts; an empty span means none.
    struct InitParams {
        std::span<const std::byte> dvc{};
        std::span<const std::byte> nvram{};
        bool auto_play = false; // see Bus::set_auto_play_enabled()
    };

    // video_standard is the player's PAL/NTSC strap. Nothing can change
    // afterwards. The spans are copied, so they need only outlive this call.
    Machine(std::span<const std::byte> bios, VideoStandard video_standard);
    Machine(std::span<const std::byte> bios, VideoStandard video_standard, const InitParams& params);
    ~Machine();

    // Power-on. Keeps NVRAM, the RTC, the mounted disc, ROMs and options.
    // Call after mounting a disc to start a game.
    void cold_boot();
    // As cold_boot(), but main RAM keeps its contents (reset line).
    void reset();
    // One video field of emulation; call refresh_rate() times a second.
    void run_frame();

    // The audio produced since the last call; call once per run_frame().
    AudioStreams drain_audio();

    // Video (video_framebuffer), input (plug, set_socket_input) and NVRAM
    // (nvram_data) are on the Bus.
    Bus& bus() { return bus_; }

    // Takes ownership; replaces any mounted disc. False (no change) on null.
    bool mount_disc(std::unique_ptr<Disc> disc);
    void remove_disc();

    // The player's open/close button. The CDIC can't read while it is open;
    // swap discs then. A reset closes it.
    void set_tray_open(bool open) { bus_.set_tray_open(open); }
    bool tray_open() const { return bus_.tray_open(); }

    // Restarts the RTC at `t`, like MAME's -rtc, for reproducible runs. Kept
    // across cold_boot() and reset().
    void set_rtc_boot_time(std::time_t t) { bus_.set_rtc_boot_time(t); }

    // Only between run_frame() calls. A state must match this machine's
    // BIOS, DVC ROM, options and mounted disc; a mismatched one leaves the
    // machine as it was. A matching but damaged one cold-boots it.
    void save_state(StateWriter& w);
    bool load_state(StateReader& r);
    size_t state_size();

    double refresh_rate() const { return refresh_rate_; }

private:
    friend class MachineInspector;

    // kPowerOnBaseline: no disc check, and the disc's byteswap latch is left alone.
    enum class StateLoadContext { kSaveState, kPowerOnBaseline };

    template <class T>
    void state_io(T& archive, StateLoadContext context = StateLoadContext::kSaveState);
    template <class T>
    void header_io(T& archive, bool external, bool& byteswap, int& frame_count);
    bool state_matches(StateReader r);
    // CRC-32 of the lead-out and TOC; 0 without a disc.
    uint32_t disc_fingerprint() const;
    void restore_power_on(bool keep_ram);
    void reset_by_slave();

    Bus bus_;
    std::unique_ptr<MusashiCore> cpu_;
    std::unique_ptr<Disc> disc_;

    double refresh_rate_ = zcdi::refresh_rate(VideoStandard::kNtsc);
    int field_lines_ = zcdi::field_lines(VideoStandard::kNtsc);

    int frame_count_ = 0;
    // run_frame()'s line in progress; the field's line count between frames. Not saved.
    int line_ = 0;

    // Saved by the constructor with the save-state field lists, so a reset
    // covers exactly what a save state does.
    std::vector<uint8_t> power_on_state_;
};

} // namespace zcdi
