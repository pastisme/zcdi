#include "mk48t08/mk48t08.h"

#include <algorithm>
#include <cstddef>
#include <ctime>

#include "scc68070/scc68070.h"

#include "core/state_archive.h"
#include "core/util.h"

namespace zcdi {
namespace mamecdi {

namespace {

// MK48T08 register offsets inside the 8 KiB timekeeper.
constexpr uint32_t kTkFlags = 0x1ff0;
constexpr uint32_t kTkCentury = 0x1ff1;
constexpr uint32_t kTkControl = 0x1ff8;
constexpr uint32_t kTkSecond = 0x1ff9;
constexpr uint32_t kTkMinute = 0x1ffa;
constexpr uint32_t kTkHour = 0x1ffb;
constexpr uint32_t kTkDay = 0x1ffc;
constexpr uint32_t kTkDate = 0x1ffd;
constexpr uint32_t kTkMonth = 0x1ffe;
constexpr uint32_t kTkYear = 0x1fff;
static_assert(kTkYear == Mk48t08SaveState::kSize - 1, "the calendar fields end the image");
} // namespace

// The calendar fields are always computed live, never read from the image
// (as mk48t08b.sv), so a stale save can't freeze the clock.
std::time_t Mk48t08::current_time() const {
    return std::time_t(boot_wall_time_) + static_cast<std::time_t>(elapsed_cycles_ / kCpuClockHz);
}

std::tm Mk48t08::local_tm() const {
    const std::time_t t = current_time();
    std::tm tmv{};
    if (const std::tm* lt = std::localtime(&t)) tmv = *lt;
    return tmv;
}

uint8_t Mk48t08::read_live_calendar_field(uint32_t offset) const {
    const std::tm tmv = local_tm();

    switch (offset) {
        case kTkSecond: return to_bcd(tmv.tm_sec);
        case kTkMinute: return to_bcd(tmv.tm_min);
        case kTkHour: return to_bcd(tmv.tm_hour);
        case kTkDay: return to_bcd(tmv.tm_wday + 1); // 0..6, Sun=0
        case kTkDate: return to_bcd(tmv.tm_mday);
        case kTkMonth: return to_bcd(tmv.tm_mon + 1);
        case kTkYear: return to_bcd((tmv.tm_year + 1900) % 100);
        default: return 0xff;
    }
}

Mk48t08::Mk48t08() {
    boot_wall_time_ = std::time(nullptr);
    data_.fill(0xff);
    seed_defaults();
}

void Mk48t08::set_boot_time(std::time_t t) {
    boot_wall_time_ = t;
    elapsed_cycles_ = 0;
    seed_century();
}

uint8_t Mk48t08::read(uint32_t offset) const {
    if (offset >= kSize) return 0xff;
    if (offset >= kTkSecond) return read_live_calendar_field(offset); // seconds to year
    return data_[offset];
}

void Mk48t08::write(uint32_t offset, uint8_t value) {
    if (offset >= kSize) return;
    data_[offset] = value;
}

void Mk48t08::load(std::span<const uint8_t> content) {
    const std::size_t n = std::min(content.size(), static_cast<std::size_t>(kSize));
    std::copy_n(content.begin(), n, data_.begin());
}

void Mk48t08::tick_cycles(int cpu_cycles) { elapsed_cycles_ += static_cast<uint64_t>(cpu_cycles); }

template <class T>
void Mk48t08::state_io(T& archive) {
    archive.begin_section(state_tag("RTC "));
    archive.io_struct(static_cast<Mk48t08SaveState&>(*this));
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(Mk48t08);

void Mk48t08::seed_defaults() {
    // Century is a plain stored byte, not a live field.
    data_[kTkFlags] = 0x00;
    data_[kTkControl] = 0x00;
    seed_century();
}

void Mk48t08::seed_century() {
    const std::tm tmv = local_tm();
    data_[kTkCentury] = to_bcd((tmv.tm_year + 1900) / 100 % 100);
}

} // namespace mamecdi
} // namespace zcdi
