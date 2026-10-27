#pragma once
#include <array>
#include <cstdint>
#include <cstring>

#include "core/util.h"

namespace zcdi {

enum : int {
    kRawSectorSize = 2352,
    kSyncSize = 12,
    kHeaderSize = 4, // min, sec, frac, mode
    kSubheaderSize = 8, // subheader stored twice (2x4)
    kForm1DataSize = 2048, // Mode 2 Form 1 user data
    kForm2DataSize = 2324, // Mode 2 Form 2 user data
};

// Q subcode (Red Book): 12 bytes, CRC last (same_cdi SUBCODE_Q_CONTROL..SUBCODE_Q_CRC1).
constexpr uint32_t kSubqSize = 12;
// CONTROL/ADR byte: ADR 1 (position), CONTROL 4 for a data track.
enum QControl : uint8_t {
    kQControlAudio = 0x01,
    kQControlData = 0x41,
};
constexpr uint8_t kLeadOutTrack = 0xaa;

// Lead-in TOC points.
enum TocPoint : uint8_t {
    kTocPointA0 = 0xa0, // first track; PSEC is the disc type
    kTocPointA1 = 0xa1, // last track
    kTocPointA2 = 0xa2, // lead-out start
};
enum TocDiscType : uint8_t {
    kDiscTypeCdda = 0x00,
    kDiscTypeCdi = 0x10,
};

// XA subheader SUBMODE bits (Green Book; same_cdi SUBMODE_*).
enum Submode : uint8_t {
    kSubmodeEor = 0x01,
    kSubmodeVideo = 0x02,
    kSubmodeAudio = 0x04,
    kSubmodeData = 0x08,
    kSubmodeTrig = 0x10,
    kSubmodeForm = 0x20, // Form 2
    kSubmodeRt = 0x40,
    kSubmodeEof = 0x80,
};

// XA subheader (first of the two identical copies).
struct Subheader {
    uint8_t file = 0;
    uint8_t channel = 0;
    uint8_t submode = 0;
    uint8_t coding = 0;
};

// LBA -> MSF with the standard 150-sector (2s) pregap.
inline void lba_to_msf(int lba, int& m, int& s, int& f) {
    int t = lba + 150;
    f = t % 75;
    t /= 75;
    s = t % 60;
    t /= 60;
    m = t;
}

// Compares the whole sync field: same_cdi/MAME check only two bytes, which a
// CD-DA sector starting with the sample 255 matches too.
inline bool sector_looks_byteswapped(const uint8_t* data) {
    static constexpr uint8_t kSwappedSync[12] = {0xff, 0x00, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0x00, 0xff};
    return std::memcmp(data, kSwappedSync, sizeof kSwappedSync) == 0;
}
inline void byteswap_sector(uint8_t* data, int size) {
    for (int i = 0; i + 1 < size; i += 2) {
        uint8_t t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
    }
}

// ECMA-130 Annex B scrambler: 15-bit LFSR x^15 + x + 1 seeded with 1, low bit
// first, over everything past the sync field (which stays 0 here).
consteval std::array<uint8_t, kRawSectorSize> make_sector_scramble() {
    std::array<uint8_t, kRawSectorSize> table{};
    uint16_t lfsr = 1;
    for (int i = kSyncSize; i < kRawSectorSize; ++i) {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; ++bit) {
            byte |= uint8_t((lfsr & 1) << bit);
            const uint16_t feedback = (lfsr ^ (lfsr >> 1)) & 1;
            lfsr = uint16_t((lfsr >> 1) | (feedback << 14));
        }
        table[i] = byte;
    }
    return table;
}
inline constexpr std::array<uint8_t, kRawSectorSize> kSectorScramble = make_sector_scramble();

// Scrambling is an XOR, so this also scrambles.
inline void descramble_sector(uint8_t* data) {
    for (int i = kSyncSize; i < kRawSectorSize; ++i)
        data[i] ^= kSectorScramble[i];
}

// The header MSF matches lba, the mode is 1 or 2 and both subheader copies
// agree; a still-scrambled sector fails this.
inline bool sector_looks_valid(const uint8_t* data, int lba) {
    uint8_t m = 0, s = 0, f = 0;
    to_bcd_msf(lba + 150, m, s, f); // the header's MSF counts the 2 s pregap
    if (data[kSyncSize + 0] != m || data[kSyncSize + 1] != s || data[kSyncSize + 2] != f) return false;
    const uint8_t mode = data[kSyncSize + 3];
    if (mode != 1 && mode != 2) return false;
    const uint8_t* sub = data + kSyncSize + kHeaderSize;
    return std::memcmp(sub, sub + 4, 4) == 0;
}

} // namespace zcdi
