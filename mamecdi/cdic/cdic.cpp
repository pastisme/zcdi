#include "cdic/cdic.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "cdic/cdic_adpcm.h"
#include "core/state_archive.h"
#include "core/util.h"
#include "disc/sector.h"
#include "scc68070/scc68070.h"

namespace zcdi {
namespace mamecdi {

using namespace cdic;

namespace {

void msf_from_lba(int lba, uint8_t& m, uint8_t& s, uint8_t& f) {
    to_bcd_msf(std::max(lba, -150) + 150, m, s, f); // nothing before 00:00:00
}

// Independent 75 Hz sector tick (cdic_clock_gen.sv sector_tick).
constexpr int kCyclesPerSectorTick = kCpuClockHz / 75; // 200000

// The backends are synchronous, so a bad sector never turns good: bound retries.
constexpr int kMaxReadFailStreak = 19;

// CRC-16/CCITT, MSB-first (same_cdi s_crc_ccitt_table).
uint16_t crc_ccitt_round(uint16_t crc, uint8_t data) {
    crc = uint16_t(crc ^ (uint16_t(data) << 8));
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

// same_cdi process_disc_sector. CRC bytes are 0xff while the CRC is computed.
void set_subq_crc(uint8_t out[kSubqSize]) {
    out[10] = 0xff;
    out[11] = 0xff;
    uint16_t crc = 0;
    for (uint32_t i = 0; i < kSubqSize; ++i)
        crc = crc_ccitt_round(crc, out[i]);
    out[10] = uint8_t(crc >> 8);
    out[11] = uint8_t(crc);
}

// Red Book Q mode 1: track and index from the TOC (index 00 before track 1's
// INDEX 01, track AA in the lead-out), time in the track (counting down before
// INDEX 01) and disc time.
void build_subq(int curr_lba, bool is_cdda, const std::vector<TocTrack>& tracks, int lead_out, uint8_t out[kSubqSize]) {
    int track = 1, start = 0;
    for (const TocTrack& t : tracks) {
        if (t.start_lba <= curr_lba || &t == &tracks.front()) {
            track = t.track_number;
            start = t.start_lba;
        }
    }
    if (curr_lba >= lead_out) {
        track = kLeadOutTrack;
        start = lead_out;
    }
    const int rel = std::abs(curr_lba - start);

    // Q_CONTROL: 0x41 for a data track, 0x01 for CD-DA (same_cdi).
    out[0] = is_cdda ? kQControlAudio : kQControlData;
    out[1] = track == kLeadOutTrack ? kLeadOutTrack : to_bcd(track);
    out[2] = curr_lba < start ? 0x00 : 0x01;
    to_bcd_msf(rel, out[3], out[4], out[5]);
    out[6] = 0x00;
    msf_from_lba(curr_lba, out[7], out[8], out[9]);
    set_subq_crc(out);
}

// same_cdi lba_from_time: MSF inside the pregap clamps to 0; frame bit 7 = even second.
int cdic_lba_from_time(uint32_t time) {
    int mins = from_bcd(uint8_t(time >> 24));
    int secs = from_bcd(uint8_t(time >> 16));
    int lba = (mins * 60 + secs) * 75;

    uint8_t bcd_frac = uint8_t(time >> 8);
    bool even_second = (bcd_frac & 0x80) != 0;
    if (!even_second) lba += from_bcd(bcd_frac & 0x7f);

    if (lba >= 150) lba -= 150;
    return lba;
}

// same_cdi get_sector_count_for_coding; 0 = reserved, poll again next tick.
int get_sector_count_for_coding(uint8_t coding) {
    int count = 2;
    switch ((coding >> 4) & 3) { // bits per sample
        case 0: count *= 2; break; // 4-bit (Level B/C): twice as many samples fit
        case 1: break; // 8-bit (Level A)
        default: return 0; // 16-bit/MPEG: unsupported
    }
    switch ((coding >> 2) & 3) { // sample rate
        case 0: break; // 37.8kHz
        case 1: count *= 2; break; // 18.9kHz: half-rate, twice the duration
        case 2: return 0; // reserved
        case 3: break; // 44.1kHz slot: not an ADPCM rate, a no-op as in same_cdi
    }
    switch (coding & 3) { // channels
        case 0: count *= 2; break; // mono: half the data cost per sample-time
        case 1: break; // stereo
        default: return 0; // reserved/MPEG
    }
    return count;
}
} // namespace

Cdic::Cdic() = default;

template <class T>
void Cdic::state_io(T& archive) {
    archive.begin_section(state_tag("CDIC"));
    archive.io_struct(static_cast<CdicSaveState&>(*this));
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(Cdic);

uint8_t Cdic::read_ram_byte(uint32_t offset) const { return offset < ram_.size() ? ram_[offset] : 0; }

void Cdic::write_ram_byte(uint32_t offset, uint8_t v) {
    // CPU writes and the CDIC DMA's memory -> CDIC leg both land here.
    if (offset < ram_.size()) ram_[offset] = v;
}

// cdic.sv:336-337 (intreq is combinational).
void Cdic::raise_irq4() {
    irq4_ = ((x_buffer_ & kBufReady) && (data_buffer_ & kDbufActive)) ||
            ((audio_buffer_ & kBufReady) && (audio_control_ & kAudctlAbufIrq));
}

void Cdic::signal_sector() {
    x_buffer_ |= kBufReady;
    data_buffer_ |= kDbufActive;
    raise_irq4();
}

uint8_t Cdic::read_reg_byte(uint32_t offset) {
    switch (offset) {
        case kRegCommand: return uint8_t(command_ >> 8);
        case kRegCommand + 1: return uint8_t(command_);
        case kRegTime: return uint8_t(time_ >> 24);
        case kRegTime + 1: return uint8_t(time_ >> 16);
        case kRegTime + 2: return uint8_t(time_ >> 8);
        case kRegTime + 3: return uint8_t(time_);
        case kRegFile: return uint8_t(file_ >> 8);
        case kRegFile + 1: return uint8_t(file_);
        case kRegChannel: return uint8_t(channel_ >> 24);
        case kRegChannel + 1: return uint8_t(channel_ >> 16);
        case kRegChannel + 2: return uint8_t(channel_ >> 8);
        case kRegChannel + 3: return uint8_t(channel_);
        case kRegAudioChannel: return uint8_t(audio_channel_ >> 8);
        case kRegAudioChannel + 1: return uint8_t(audio_channel_);
        case kRegAbuf: {
            uint8_t v = uint8_t(audio_buffer_ >> 8);
            audio_buffer_ &= ~kBufReady; // acknowledges + re-evaluates IN4
            raise_irq4();
            return v;
        }
        case kRegAbuf + 1: return uint8_t(audio_buffer_);
        case kRegXbuf: {
            uint8_t v = uint8_t(x_buffer_ >> 8);
            x_buffer_ &= ~kBufReady; // acknowledges + re-evaluates IN4
            raise_irq4();
            return v;
        }
        case kRegXbuf + 1: return uint8_t(x_buffer_);
        case kRegAudctl: return uint8_t(audio_control_ >> 8);
        case kRegAudctl + 1: {
            uint8_t v = uint8_t(audio_control_);
            audio_control_ &= ~kAudctlStoppedOnEnd;
            return v;
        }
        case kRegInterruptVector: return uint8_t(interrupt_vector_ >> 8);
        case kRegInterruptVector + 1: return uint8_t(interrupt_vector_);
        case kRegDbuf: return uint8_t(data_buffer_ >> 8);
        case kRegDbuf + 1: return uint8_t(data_buffer_);
        default: return 0; // DMA control (write-only) and unmapped offsets
    }
}

void Cdic::write_reg_byte(uint32_t offset, uint8_t v) {
    switch (offset) {
        case kRegCommand: command_ = set_hi(command_, v); return;
        case kRegCommand + 1: command_ = set_lo(command_, v); return;
        case kRegTime: time_ = set_byte(time_, 24, v); return;
        case kRegTime + 1: time_ = set_byte(time_, 16, v); return;
        case kRegTime + 2: time_ = set_byte(time_, 8, v); return;
        case kRegTime + 3: time_ = set_byte(time_, 0, v); return;
        case kRegFile: file_ = set_hi(file_, v); return;
        case kRegFile + 1: file_ = set_lo(file_, v); return;
        case kRegChannel: channel_ = set_byte(channel_, 24, v); return;
        case kRegChannel + 1: channel_ = set_byte(channel_, 16, v); return;
        case kRegChannel + 2: channel_ = set_byte(channel_, 8, v); return;
        case kRegChannel + 3: channel_ = set_byte(channel_, 0, v); return;
        case kRegAudioChannel: audio_channel_ = set_hi(audio_channel_, v); return;
        case kRegAudioChannel + 1: audio_channel_ = set_lo(audio_channel_, v); return;
        case kRegAbuf:
            audio_buffer_ = set_hi(audio_buffer_, v);
            raise_irq4();
            return;
        case kRegAbuf + 1: audio_buffer_ = set_lo(audio_buffer_, v); return;
        case kRegXbuf:
            x_buffer_ = set_hi(x_buffer_, v);
            raise_irq4();
            return;
        case kRegXbuf + 1: x_buffer_ = set_lo(x_buffer_, v); return;
        // The write completing DMA control runs Bus::perform_cdic_dma.
        case kRegDmaControl: dma_control_ = set_hi(dma_control_, v); return;
        case kRegDmaControl + 1: dma_control_ = set_lo(dma_control_, v); return;
        case kRegAudctl: audio_control_ = set_hi(audio_control_, v); return;
        case kRegAudctl + 1:
            audio_control_ = set_lo(audio_control_, v);
            handle_audctl_write(); // fires once the low byte completes the word, as DBUF below
            raise_irq4(); // kAudctlAbufIrq gates the line at once (cdic.sv intreq is combinational)
            return;
        case kRegInterruptVector: interrupt_vector_ = set_hi(interrupt_vector_, v); return;
        case kRegInterruptVector + 1: interrupt_vector_ = set_lo(interrupt_vector_, v); return;
        case kRegDbuf: data_buffer_ = set_hi(data_buffer_, v); return;
        case kRegDbuf + 1:
            data_buffer_ = set_lo(data_buffer_, v);
            handle_data_buffer_write(data_buffer_); // fires once the low byte completes the word
            raise_irq4(); // so does kDbufActive
            return;
        default: return;
    }
}

namespace {
// These start a read even mid-read. The Reset commands deliver no sector
// and interrupt a few ticks later (cdic.sv, CDi_MiSTer 8cec2d4).
bool starts_disc_read_unconditionally(uint8_t command) {
    switch (command) {
        case kCmdFetchToc:
        case kCmdPlayCdda:
        case kCmdReadMode1:
        case kCmdReadMode2:
        case kCmdSeek: return true;
        default: return false;
    }
}
} // namespace

// same_cdi cdic_w/handle_cdic_command: trigger and active are independent checks.
void Cdic::handle_data_buffer_write(uint16_t value) {
    if (value & kDbufTrigger) {
        uint8_t command = uint8_t(command_ & 0xff);
        // CD-DA mode lasts until the next command executes (cdic.sv read_cdda).
        cdda_mode_ = command == kCmdPlayCdda;
        spin_down_count_ = 0; // a new command cancels a pending interrupt
        x_buffer_ &= ~kBufReady; // and acknowledges the last one (cdic.sv:613)
        if (command == kCmdResetMode1 || command == kCmdResetMode2) {
            stop_read();
            spin_down_count_ = command == kCmdResetMode1 ? 3 : 2;
        } else if (starts_disc_read_unconditionally(command)) {
            start_read(command);
        } else if (command == kCmdStopCdda) { // cancels unconditionally
            stop_read();
            curr_lba_ = 0;
        } else if (command == kCmdUpdate) {
            // Cuts off the playing buffer (cdic.sv:651).
            if (audio_channel_ != 0 && !(audio_control_ & kAudctlRun)) abort_audio_playback();
            // Mid-read the next sector's XBUF answers it: cdapdriv takes any XBUF after Update as a sector.
            if (disc_command_ == kCmdIdle) spin_down_count_ = 2;
        }
        // anything unrecognized: untouched.
        data_buffer_ &= ~kDbufTrigger;
    }
    if (!(value & kDbufActive)) {
        stop_read();
        curr_lba_ = 0;
        // cdic.sv:793-797; kDbufBank1 was just written with `value`.
        data_target_buffer_ = false;
        audio_target_buffer_ = false;
        audio_control_ &= ~kAudctlStoppedOnEnd; // cdic.sv:815
    }
}

void Cdic::start_read(uint8_t command) {
    disc_command_ = command;
    // Fetch TOC counts lead-in sectors instead (deliver_toc_sector).
    curr_lba_ = command == kCmdFetchToc ? 0 : cdic_lba_from_time(time_);
    read_fail_streak_ = 0;
    // CDi_MiSTer hps_cd_sector_cache.sv kSeekTime (~253 ms).
    spinup_counter_ = 19;
    sector_cycle_accum_ = 0;
}

void Cdic::stop_read() {
    disc_command_ = kCmdIdle;
    spinup_counter_ = 0;
    read_fail_streak_ = 0;
}

void Cdic::tick_cycles(int cpu_cycles) {
    // Runs whether or not the disc reads (same_cdi m_audio_timer).
    audio_map_cycle_accum_ += cpu_cycles;
    while (audio_map_cycle_accum_ >= kCyclesPerSectorTick) {
        audio_map_cycle_accum_ -= kCyclesPerSectorTick;
        audio_map_tick();
    }

    spin_down_cycle_accum_ += cpu_cycles;
    while (spin_down_cycle_accum_ >= kCyclesPerSectorTick) {
        spin_down_cycle_accum_ -= kCyclesPerSectorTick;
        if (spin_down_count_ > 0 && --spin_down_count_ == 0) {
            x_buffer_ |= kBufReady;
            raise_irq4();
        }
    }

    if (disc_command_ == kCmdIdle) return;
    sector_cycle_accum_ += cpu_cycles;
    while (sector_cycle_accum_ >= kCyclesPerSectorTick) {
        sector_cycle_accum_ -= kCyclesPerSectorTick;
        if (disc_command_ == kCmdIdle)
            return; // start_read()/Stop CDDA can fire mid-sector, from process_sector() below
        if (spinup_counter_ > 0) {
            --spinup_counter_;
            continue;
        }
        if (process_sector()) ++curr_lba_; // false = transient read failure, retry same LBA next tick
    }
}

bool Cdic::is_mode2_sector_selected(const RawSector& s) const {
    if (s.sub.file != uint8_t(file_ >> 8)) return false; // FILE's high byte only (cdic.sv:453)
    uint8_t submode = s.sub.submode;
    if (submode & (kSubmodeEof | kSubmodeTrig | kSubmodeEor)) return true; // control sectors always pass
    if (!(submode & (kSubmodeData | kSubmodeAudio | kSubmodeVideo))) return false; // empty/message sector
    return s.sub.channel < 32 && ((channel_ >> s.sub.channel) & 1u) != 0;
}

bool Cdic::is_mode2_audio_selected(const RawSector& s) const {
    uint8_t submode = s.sub.submode;
    if (!(submode & kSubmodeForm) || !(submode & kSubmodeAudio)) return false;
    return s.sub.channel < 16 && ((audio_channel_ >> s.sub.channel) & 1u) != 0;
}

bool Cdic::next_data_bank() {
    // Flips per sector; the first after a reset is bank 1.
    data_target_buffer_ = !data_target_buffer_;
    data_buffer_ =
        uint16_t((data_buffer_ & ~(kDbufBank1 | kDbufAudio)) | (data_target_buffer_ ? kDbufBank1 : kDbufBank0));
    return data_target_buffer_;
}

void Cdic::pack_into_ram(const RawSector& s) {
    // same_cdi process_sector_data layout: sync dropped, synthesized Q subcode after.
    uint32_t offset = next_data_bank() ? kDataBank1 : kDataBank0;

    auto write_bytes = [&](const uint8_t* src, uint32_t n) {
        if (offset + n <= ram_.size()) std::memcpy(ram_.data() + offset, src, n);
        offset += n;
    };

    // HEADER..CODING1 (8 bytes: same_cdi's SECTOR_HEADER..SECTOR_FILE2).
    write_bytes(s.data + kSyncSize, kFile2Offset - kSyncSize);

    // The audio payload goes 0x1400 words on, in the audio toggle's bank, which
    // kDbufBank1 then reports (cdic.sv:512, cdic.sv:610).
    if (disc_command_ == kCmdReadMode2 && is_mode2_audio_selected(s)) {
        data_buffer_ =
            uint16_t((data_buffer_ & ~kDbufBank1) | kDbufAudio | (audio_target_buffer_ ? kDbufBank1 : kDbufBank0));
        offset = (audio_target_buffer_ ? kDataBank1 : kDataBank0) + (kFile2Offset - kSyncSize) + kAudioBankSkipBytes;
        audio_target_buffer_ = !audio_target_buffer_;
    }

    // FILE2..end (same_cdi's SECTOR_FILE2..SECTOR_SIZE).
    write_bytes(s.data + kFile2Offset, kRawSectorSize - kFile2Offset);

    // One byte per RAM word, zero-extended.
    uint8_t subq[kSubqSize];
    build_subq(curr_lba_, disc_command_ == kCmdPlayCdda, tracks_, disc_->lead_out_lba(), subq);
    for (uint32_t i = 0; i < kSubqSize; ++i) {
        uint8_t word[2] = {0x00, subq[i]};
        write_bytes(word, 2);
    }
}

void Cdic::handle_audctl_write() {
    if (audio_control_ & kAudctlRun) {
        if (!decoding_audio_map_) {
            decoding_audio_map_ = true;
            // ADPCM starts at kAudioMap0; CD-DA at the buffer kDbufBank1 names
            // (process_audio_map).
            decode_addr_ = 0xffff;
            // An idle player starts now, not on the next 75 Hz tick as in same_cdi
            // (audioplayer.sv decoder_start).
            if (audio_format_sectors_ == 0) {
                audio_sector_counter_ = 1;
                audio_map_cycle_accum_ = kCyclesPerSectorTick;
                adpcm_history_[0] = History{};
                adpcm_history_[1] = History{};
            }
        }
        return;
    }
    decoding_audio_map_ = false;
    if (audio_channel_ != 0) abort_audio_playback();
}

// cdic.sv audio_abort_playback: undrained samples dropped, no ABUF.
void Cdic::abort_audio_playback() {
    decoding_audio_map_ = false;
    audio_format_sectors_ = 0;
    audio_sector_counter_ = 0;
    audio_out_.resize(std::min(audio_out_mark_, audio_out_.size()));
}

void Cdic::audio_map_tick() {
    if (audio_sector_counter_ > 0) {
        --audio_sector_counter_;
        if (audio_sector_counter_ > 0) return;
    }
    if (audio_format_sectors_ != 0) {
        audio_format_sectors_ = 0;
        audio_buffer_ |= kBufReady;
        raise_irq4();
    }
    if (decoding_audio_map_) process_audio_map();
}

// kAudioMapEnd stops the player (cdic.sv decoder_disable_audiomap).
void Cdic::process_audio_map() {
    if (cdda_mode_) {
        // cdic.sv read_cdda: one raw sector per buffer, one tick long.
        if (decode_addr_ != kCddaBank0 && decode_addr_ != kCddaBank1) {
            decode_addr_ = uint16_t((data_buffer_ & kDbufBank1) ? kCddaBank1 : kCddaBank0);
        }
        const uint8_t* pcm = ram_.data() + decode_addr_;
        decode_addr_ = uint16_t(decode_addr_ == kCddaBank0 ? kCddaBank1 : kCddaBank0);
        audio_format_sectors_ = 1;
        audio_sector_counter_ = 1;
        audio_out_mark_ = audio_out_.size();
        for (uint32_t i = 0; i + 3 < kRawSectorSize; i += 4) {
            audio_out_.push_back(int16_t(uint16_t(pcm[i]) | (uint16_t(pcm[i + 1]) << 8)));
            audio_out_.push_back(int16_t(uint16_t(pcm[i + 2]) | (uint16_t(pcm[i + 3]) << 8)));
        }
        return;
    }
    if (decode_addr_ != kAudioMap0 && decode_addr_ != kAudioMap1) decode_addr_ = kAudioMap0;
    const uint32_t base = uint32_t(decode_addr_) & 0x3ffeu;
    const uint8_t coding = (base + kAudioMapCoding < ram_.size()) ? ram_[base + kAudioMapCoding] : kAudioMapEnd;
    if (coding == kAudioMapEnd) {
        decoding_audio_map_ = false;
        audio_control_ = uint16_t((audio_control_ | kAudctlStoppedOnEnd) & ~kAudctlRun);
        return;
    }
    decode_addr_ ^= kAudioMap0 ^ kAudioMap1;
    audio_format_sectors_ = get_sector_count_for_coding(coding);
    audio_sector_counter_ = audio_format_sectors_;
    audio_out_mark_ = audio_out_.size();
    if (base + kAudioMapData + kForm2DataSize <= ram_.size()) {
        std::vector<int16_t> native_37800;
        decode_xa_adpcm(ram_.data() + base + kAudioMapData, coding, native_37800, adpcm_history_);
        resample_adpcm_to_44100(native_37800, audio_out_, adpcm_resample_state_);
    }
}

// Stored raw; the ADPCM player plays it in CD-DA mode once AUDCTL starts it.
void Cdic::pack_cdda_into_ram(const RawSector& s, const uint8_t* subq) {
    const uint8_t* pcm = s.data;
    uint8_t swapped[kRawSectorSize];
    if (disc_ && disc_->cdda_big_endian()) {
        for (int i = 0; i + 1 < kRawSectorSize; i += 2) {
            swapped[i] = s.data[i + 1];
            swapped[i + 1] = s.data[i];
        }
        pcm = swapped;
    }
    // Q subcode where a data sector's goes (cdic.sv:566-575, 618-626).
    const bool data_bank = next_data_bank();
    std::memcpy(ram_.data() + (data_bank ? kCddaBank1 : kCddaBank0), pcm, kRawSectorSize);
    uint32_t q = (data_bank ? kDataBank1 : kDataBank0) + kSubqOffset;
    for (uint32_t i = 0; i < kSubqSize; ++i, q += 2) {
        ram_[q] = 0x00;
        ram_[q + 1] = subq[i];
    }
}

// Lead-in TOC, each entry 3 times, the table repeated (CDi_MiSTer sim2/table_of_contents.h).
void Cdic::deliver_toc_sector() {
    static const std::vector<TocTrack> kNoTracks{TocTrack{1, false, 0}};
    const std::vector<TocTrack>& tracks = tracks_.empty() ? kNoTracks : tracks_;
    auto control = [](const TocTrack& t) { return t.is_audio ? kQControlAudio : kQControlData; };
    bool has_data = false;
    for (const TocTrack& t : tracks)
        has_data = has_data || !t.is_audio;

    const int entries = int(tracks.size()) + 3;
    const int entry = (curr_lba_ / 3) % entries;
    uint8_t q[kSubqSize] = {};
    if (entry < int(tracks.size())) {
        q[0] = control(tracks[entry]);
        q[2] = to_bcd(tracks[entry].track_number);
        msf_from_lba(tracks[entry].start_lba, q[7], q[8], q[9]);
    } else if (entry == int(tracks.size())) {
        q[0] = control(tracks.front());
        q[2] = kTocPointA0;
        q[7] = to_bcd(tracks.front().track_number);
        q[8] = has_data ? kDiscTypeCdi : kDiscTypeCdda;
    } else if (entry == int(tracks.size()) + 1) {
        q[0] = control(tracks.back());
        q[2] = kTocPointA1;
        q[7] = to_bcd(tracks.back().track_number);
    } else {
        q[0] = control(tracks.back());
        q[2] = kTocPointA2;
        msf_from_lba(disc_ ? disc_->lead_out_lba() : 0, q[7], q[8], q[9]);
    }
    // Q_TRACK 0 (the lead-in); MIN/SEC/FRAME run through the lead-in.
    to_bcd_msf(curr_lba_, q[3], q[4], q[5], true);
    set_subq_crc(q);

    RawSector silence{};
    pack_cdda_into_ram(silence, q);
    signal_sector();
}

bool Cdic::process_sector() {
    // One lead-in sector, until the driver stops the read.
    if (disc_command_ == kCmdFetchToc) {
        deliver_toc_sector();
        return true;
    }
    // Tray open: the read waits for the disc instead of ending.
    if (!disc_) return false;
    RawSector s;
    // Play CDDA runs on into the lead-out: silence, Q track AA.
    if (disc_command_ == kCmdPlayCdda && curr_lba_ >= disc_->lead_out_lba()) {
        s = RawSector{};
    } else if (!disc_->read_sector(curr_lba_, s)) {
        // Hardware waits when starved (hps_cd_sector_cache.sv:79-80); a read past
        // the end is fatal (Green Book), and so is a failure streak.
        if (curr_lba_ >= disc_->lead_out_lba() || ++read_fail_streak_ > kMaxReadFailStreak) {
            disc_command_ = kCmdIdle;
            return true;
        }
        return false; // transient: retry the same curr_lba_ next tick
    }
    read_fail_streak_ = 0;

    // Play CDDA: no filter, every sector interrupts (cdic.sv:578-607).
    if (disc_command_ == kCmdPlayCdda) {
        uint8_t subq[kSubqSize];
        build_subq(curr_lba_, true, tracks_, disc_->lead_out_lba(), subq);
        pack_cdda_into_ram(s, subq);
        signal_sector();
        return true;
    }

    // The filter is for Read Mode2 only (same_cdi DISC_MODE2).
    bool deliver = (disc_command_ == kCmdReadMode2) ? is_mode2_sector_selected(s) : true;
    if (deliver) {
        pack_into_ram(s);
        signal_sector();
    }

    // EOF never stops a read (cdic.sv:478-482); only clearing kDbufActive does.
    return true;
}

std::vector<int16_t> Cdic::drain_audio() {
    audio_out_mark_ = 0;
    return std::exchange(audio_out_, {});
}

} // namespace mamecdi
} // namespace zcdi
