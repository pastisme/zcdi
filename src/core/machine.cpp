#include "core/machine.h"
#include "core/cpu/musashi_core.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace zcdi {

namespace {

// CPU cycles between peripheral ticks and interrupt updates. A whole frame
// at once made the timer interrupt late and CD-RTOS's boot stall.
constexpr int kPeriphTickChunk = 1000;

// While the CDIC streams: the timer0 tick, so a CDIC IRQ4 lands promptly.
constexpr int kFineTickChunk = 96;

int cycles_per_frame_for(double refresh_rate) { return static_cast<int>(mamecdi::kCpuClockHz / refresh_rate); }

} // namespace

Machine::Machine(std::span<const std::byte> bios, VideoStandard video_standard)
    : Machine(bios, video_standard, InitParams{}) {}

Machine::Machine(std::span<const std::byte> bios, VideoStandard video_standard, const InitParams& params)
    : refresh_rate_(zcdi::refresh_rate(video_standard)),
      field_lines_(zcdi::field_lines(video_standard)),
      line_(field_lines_) {
    cpu_ = std::make_unique<MusashiCore>(bus_);
    bus_.load_bios(bios);
    if (!params.dvc.empty()) bus_.load_dvc(params.dvc);
    if (!params.nvram.empty()) bus_.load_nvram(params.nvram);
    bus_.set_auto_play_enabled(params.auto_play);
    bus_.set_video_standard(video_standard);
    StateWriter w(power_on_state_);
    state_io(w);
}

Machine::~Machine() = default;

void Machine::cold_boot() { restore_power_on(/*keep_ram=*/false); }

void Machine::reset() { restore_power_on(/*keep_ram=*/true); }

void Machine::reset_by_slave() {
    const int frame = frame_count_;
    reset();
    frame_count_ = frame;
    bus_.note_reset_by_slave();
}

void Machine::restore_power_on(bool keep_ram) {
    std::vector<uint8_t> kept;
    StateWriter keep(kept);
    bus_.nvram_state_io(keep);
    if (keep_ram) bus_.ram_state_io(keep);

    StateReader baseline(power_on_state_);
    state_io(baseline, StateLoadContext::kPowerOnBaseline);
    StateReader restore(kept);
    bus_.nvram_state_io(restore);
    if (keep_ram) bus_.ram_state_io(restore);
    if (baseline.failed() || restore.failed()) {
        // A field list reads back differently than it writes.
        std::fprintf(stderr, "[zcdi] internal error: power-on state didn't reload\n");
    }

    // Before the reset pulse: the CPU fetches SSP/PC from these vectors.
    bus_.prepare_reset_vectors();
    cpu_->reset();
}

bool Machine::mount_disc(std::unique_ptr<Disc> disc) {
    if (!disc) return false;
    disc_ = std::move(disc);
    bus_.set_disc(disc_.get());
    return true;
}

void Machine::remove_disc() {
    bus_.set_disc(nullptr);
    disc_.reset();
}

namespace {
constexpr uint32_t kStateFormatVersion = 1;

void append_le32(std::vector<uint8_t>& out, int32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(uint32_t(v) >> (8 * i)));
}
} // namespace

uint32_t Machine::disc_fingerprint() const {
    if (!disc_) return 0;
    std::vector<uint8_t> bytes;
    append_le32(bytes, disc_->lead_out_lba());
    for (const TocTrack& t : disc_->toc_tracks()) {
        append_le32(bytes, t.track_number);
        append_le32(bytes, t.is_audio ? 1 : 0);
        append_le32(bytes, t.start_lba);
    }
    return state_crc32(bytes);
}

// Version, disc identity and frame count: everything before the CPU section.
template <class T>
void Machine::header_io(T& archive, bool external, bool& byteswap, int& frame_count) {
    uint32_t version = kStateFormatVersion;
    archive.io(version);
    archive.check(version == kStateFormatVersion);

    // The disc isn't saved, only identified: its TOC must match.
    const uint32_t live_disc_id = disc_fingerprint();
    uint32_t disc_id = live_disc_id;
    archive.io(disc_id);
    if (external) archive.check(disc_id == live_disc_id);
    byteswap = disc_ && disc_->byteswap_detected();
    archive.io(byteswap);
    archive.io(frame_count);
}

template <class T>
void Machine::state_io(T& archive, StateLoadContext context) {
    const bool external = context == StateLoadContext::kSaveState;
    archive.begin_section(state_tag("ZCST"));
    bool byteswap = false;
    header_io(archive, external, byteswap, frame_count_);
    if (archive.failed()) return;

    // Inside ZCST, so a reader can skip the whole machine (libretro/savestate.cpp).
    cpu_->state_io(archive);
    bus_.state_io(archive);
    archive.end_section();
    if constexpr (T::kLoading) {
        if (archive.failed()) return;
        if (disc_ && external) disc_->set_byteswap_detected(byteswap);
    }
}
template void Machine::state_io<StateWriter>(StateWriter&, StateLoadContext);
template void Machine::state_io<StateReader>(StateReader&, StateLoadContext);
template void Machine::state_io<StateSizer>(StateSizer&, StateLoadContext);

void Machine::save_state(StateWriter& w) { state_io(w); }

size_t Machine::state_size() {
    StateSizer s;
    state_io(s);
    return s.size();
}

bool Machine::state_matches(StateReader r) {
    r.begin_section(state_tag("ZCST"));
    bool byteswap = false;
    int frame_count = 0;
    header_io(r, /*external=*/true, byteswap, frame_count);
    r.skip_section(state_tag("CPU "));
    r.begin_section(state_tag("BUS "));
    bus_.identity_io(r);
    return !r.failed();
}

bool Machine::load_state(StateReader& r) {
    if (!state_matches(r)) return false;
    // Loading writes as it goes, so a damaged state is undone from a snapshot.
    // The buffers keep their capacity across loads.
    undo_state_.clear();
    StateWriter snapshot(undo_state_);
    save_state(snapshot);
    undo_nvram_.clear();
    StateWriter nvram(undo_nvram_);
    bus_.nvram_state_io(nvram);

    state_io(r);
    if (!r.failed()) return true;
    StateReader undo(undo_state_);
    state_io(undo);
    if (!undo.failed()) return false;
    // A field list reads back differently than it writes: cold boot, saves kept.
    std::fprintf(stderr, "[zcdi] internal error: state before the load didn't reload\n");
    restore_power_on(/*keep_ram=*/false);
    StateReader keep(undo_nvram_);
    bus_.nvram_state_io(keep);
    return false;
}

AudioStreams Machine::drain_audio() { return bus_.drain_audio(); }

void Machine::run_frame() {
    ++frame_count_;

    bus_.slave_tick_frame(); // once per frame

    // Line by line, so the MCD212's line work and interrupts land in time.
    const int64_t field_cycles = cycles_per_frame_for(refresh_rate_);
    int64_t done = 0;
    // Interlaced even fields have a line more, in the same cycles.
    const int lines = bus_.video_begin_field(field_lines_);
    for (line_ = 0; line_ < lines; ++line_) {
        const int64_t line_end = field_cycles * (line_ + 1) / lines;
        while (done < line_end) {
            int tick_chunk = bus_.cdic_disc_active() ? kFineTickChunk : kPeriphTickChunk;
            int chunk = int(std::min<int64_t>(line_end - done, tick_chunk));
            // Can overshoot `chunk` (the last instruction completes); the
            // overshoot is charged to the next line.
            int consumed = cpu_->run(chunk);
            bus_.periph_tick(consumed);
            if (bus_.consume_slave_reset_request()) reset_by_slave();
            cpu_->set_irq(bus_.update_interrupts());
            done += consumed;
        }
        bus_.video_end_of_line(line_);
        cpu_->set_irq(bus_.update_interrupts());
    }

    bus_.dvc_tick_frame(refresh_rate_);
    bus_.video_end_field();
}

} // namespace zcdi
