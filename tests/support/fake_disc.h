#pragma once

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "core/util.h"
#include "disc/disc.h"
#include "disc/sector.h"

namespace zcdi_test {

struct MsfBcd {
    uint8_t m, s, f;
};

// An LBA's disc time in BCD.
inline MsfBcd msf_bcd(int lba) {
    int m = 0, s = 0, f = 0;
    zcdi::lba_to_msf(lba, m, s, f);
    return {zcdi::to_bcd(m), zcdi::to_bcd(s), zcdi::to_bcd(f)};
}

// Sync, the LBA's own MSF, mode 2.
inline void write_mode2_header(int lba, zcdi::RawSector& out) {
    out.data[0] = 0x00;
    std::memset(out.data + 1, 0xff, 10);
    out.data[11] = 0x00;
    const MsfBcd t = msf_bcd(lba);
    out.data[12] = t.m;
    out.data[13] = t.s;
    out.data[14] = t.f;
    out.data[15] = 2;
}

// Real-time file submodes.
constexpr uint8_t kSubmodeData = 0x48;          // real-time | data
constexpr uint8_t kSubmodeAudio = 0x64;         // real-time | form 2 | audio
constexpr uint8_t kSubmodeAudioTrigger = 0x74;  // ... | trigger

// Valid Mode 2 sectors at their own MSF; payload byte 0 is marker(lba).
class FakeDisc : public zcdi::Disc {
public:
    explicit FakeDisc(std::vector<uint8_t> submodes) : Disc(nullptr), submodes_(std::move(submodes)) {}

    bool read_sector(int lba, zcdi::RawSector& out) override {
        out = {};
        const uint8_t submode = lba >= 0 && lba < int(submodes_.size()) ? submodes_[lba] : 0x00;
        write_mode2_header(lba, out);
        const uint8_t sub[4] = {1, 0, submode, 0};
        std::memcpy(out.data + 16, sub, 4);
        std::memcpy(out.data + 20, sub, 4);
        out.data[24] = marker(lba);
        out.sub = {sub[0], sub[1], sub[2], sub[3]};
        return true;
    }
    int lead_out_lba() const override { return 1000; }

    static uint8_t marker(int lba) { return uint8_t(0xa0 + lba); }

private:
    std::vector<uint8_t> submodes_;
};

}  // namespace zcdi_test
