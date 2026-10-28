#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zcdi {
namespace mamecdi {

// Image format: raw 8192 bytes, no header (as MAME/CDi_MiSTer). Never
// touches files; the frontend persists data() and feeds load().
struct Mk48t08SaveState {
    static constexpr uint32_t kSize = 8192;
    std::array<uint8_t, kSize> data_{};
    int64_t boot_wall_time_ = 0; // sys_seconds count, read with no time zone
    uint64_t elapsed_cycles_ = 0;
};

class Mk48t08 : private Mk48t08SaveState {
public:
    using Mk48t08SaveState::kSize;

    // The clock starts at host local time.
    Mk48t08();

    // Restarts the clock at `t`, like MAME's -rtc, for reproducible runs. `t`
    // is the wall clock with no time zone: it reads the same on every host.
    void set_boot_time(std::chrono::sys_seconds t);

    uint8_t read(uint32_t offset) const;
    void write(uint32_t offset, uint8_t value);

    const uint8_t* data() const { return data_.data(); }
    std::size_t size() const { return data_.size(); }

    // Truncated to kSize; shorter content only overwrites its own prefix.
    void load(std::span<const uint8_t> content);

    // The clock advances with emulated CPU cycles, not host time.
    void tick_cycles(int cpu_cycles);

    // Saves the clock basis too, so a loaded state resumes at its saved time.
    template <class T>
    void state_io(T& archive);

private:
    void seed_defaults();
    void seed_century();
    uint8_t read_live_calendar_field(uint32_t offset) const;
    std::chrono::sys_seconds current_time() const;
};

} // namespace mamecdi
} // namespace zcdi
