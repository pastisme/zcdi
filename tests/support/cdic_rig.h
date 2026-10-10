#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "cdic/cdic.h"
#include "support/fake_disc.h"

namespace zcdi_test {

inline void write_reg16(zcdi::mamecdi::Cdic& cdic, uint32_t off, uint16_t v) {
    cdic.write_reg_byte(off, uint8_t(v >> 8));
    cdic.write_reg_byte(off + 1, uint8_t(v));
}

inline uint16_t read_reg16(zcdi::mamecdi::Cdic& cdic, uint32_t off) {
    const uint8_t hi = cdic.read_reg_byte(off);
    return uint16_t(hi << 8 | cdic.read_reg_byte(off + 1));
}

// An LBA's disc time, as the time register takes it (BCD MM:SS, FF).
inline void set_time(zcdi::mamecdi::Cdic& cdic, int lba) {
    const MsfBcd t = msf_bcd(lba);
    write_reg16(cdic, 0x002, uint16_t(t.m << 8 | t.s));
    write_reg16(cdic, 0x004, uint16_t(t.f << 8));
}

struct Delivery {
    uint16_t dbuf;
    uint8_t submode;  // read back from the bank DBUF points at
    uint8_t marker;
};

// Reads file 1 / channel 0 the way the BIOS CD driver programs it.
struct CdicRig {
    FakeDisc disc;
    zcdi::mamecdi::Cdic cdic;

    explicit CdicRig(std::vector<uint8_t> submodes) : disc(std::move(submodes)) {
        cdic.set_disc(&disc);
        write_reg16(cdic, 0x006, 0x0100);  // file 1
        write_reg16(cdic, 0x008, 0x0000);
        write_reg16(cdic, 0x00a, 0x0001);  // channel 0
        write_reg16(cdic, 0x00c, 0x0001);  // audio channel 0
    }

    // Read Mode 2 from `lba`, then start.
    void start_read(int lba) {
        set_time(cdic, lba);
        write_reg16(cdic, 0x000, 0x002a);  // Read Mode 2
        write_reg16(cdic, 0x3fe, 0xc000);
    }

    // DBUF with bit14 clear: stops the read and resets both toggles.
    void stop() { write_reg16(cdic, 0x3fe, 0x0000); }

    std::vector<Delivery> collect(size_t count) {
        std::vector<Delivery> out;
        for (int step = 0; step < 4000 && out.size() < count; ++step) {
            cdic.tick_cycles(10000);
            if (!(cdic.read_reg_byte(0x3f6) & 0x80)) continue;
            const uint16_t dbuf = uint16_t(cdic.read_reg_byte(0x3fe) << 8 | cdic.read_reg_byte(0x3ff));
            const uint32_t bank = (dbuf & 1) ? 0xa00 : 0;
            // FILE2 subheader copy: after the header (data) or at 0x2808 (audio).
            const uint32_t file2 = (dbuf & 4) ? 0x2808 + bank : 8 + bank;
            out.push_back({.dbuf = dbuf,
                           .submode = cdic.read_ram_byte(file2 + 2),
                           .marker = cdic.read_ram_byte(file2 + 4)});
        }
        return out;
    }
};

}  // namespace zcdi_test
