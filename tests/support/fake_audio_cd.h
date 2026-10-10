#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "disc/disc.h"
#include "support/fake_disc.h"

namespace zcdi_test {

// Red Book disc of sine tones, track k at 441*k Hz (100/k samples a period),
// left and right in opposite phase. CD-i Ready: a Mode 2 program with its disc
// label at LBA 16 sits before track 1's INDEX 01, as on The Apprentice.
class FakeAudioCd : public zcdi::Disc {
public:
    static constexpr int kSamplesPerSector = 588;
    static constexpr int kCdiReadyPregap = 150;

    struct Options {
        bool big_endian = false;  // CHD stores CD-DA big-endian
        bool cdi_ready = false;
    };

    explicit FakeAudioCd(std::vector<int> track_sectors) : FakeAudioCd(std::move(track_sectors), Options{}) {}
    FakeAudioCd(std::vector<int> track_sectors, Options options) : Disc(nullptr), options_(options) {
        int lba = options.cdi_ready ? kCdiReadyPregap : 0;
        for (size_t i = 0; i < track_sectors.size(); ++i) {
            toc_.push_back({.track_number = int(i) + 1, .is_audio = true, .start_lba = lba});
            lba += track_sectors[i];
        }
        lead_out_ = lba;
    }

    // Sample n of the track (n counts from its INDEX 01), left or right.
    static int16_t sample(int track, int n, bool right) {
        const double v = 8000.0 * std::sin(2.0 * 3.14159265358979323846 * 441.0 * track * n / 44100.0);
        return int16_t(std::lround(right ? -v : v));
    }

    bool read_sector(int lba, zcdi::RawSector& out) override {
        out = {};
        if (lba < 0 || lba >= lead_out_) return false;
        if (lba < toc_.front().start_lba) {
            write_program_sector(lba, out);
            return true;
        }
        const zcdi::TocTrack* track = &toc_.front();
        for (const zcdi::TocTrack& t : toc_) {
            if (t.start_lba <= lba) track = &t;
        }
        const int first = (lba - track->start_lba) * kSamplesPerSector;
        for (int i = 0; i < kSamplesPerSector; ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                const uint16_t v = uint16_t(sample(track->track_number, first + i, ch == 1));
                uint8_t* p = out.data + (i * 2 + ch) * 2;
                p[options_.big_endian ? 1 : 0] = uint8_t(v);
                p[options_.big_endian ? 0 : 1] = uint8_t(v >> 8);
            }
        }
        return true;
    }
    int lead_out_lba() const override { return lead_out_; }
    bool cdda_big_endian() const override { return options_.big_endian; }
    std::vector<zcdi::TocTrack> toc_tracks() const override { return toc_; }

private:
    // Mode 2 Form 1 at its own MSF; LBA 16 is the Green Book disc label.
    static void write_program_sector(int lba, zcdi::RawSector& out) {
        write_mode2_header(lba, out);
        out.data[18] = out.data[22] = 0x08;  // data
        if (lba == 16) std::memcpy(out.data + 24, "\x01" "CD-I \x01", 7);
        out.sub = {0, 0, 0x08, 0};
    }

    Options options_;
    std::vector<zcdi::TocTrack> toc_;
    int lead_out_ = 0;
};

}  // namespace zcdi_test
