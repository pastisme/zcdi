// What the CDIC does when the disc can't deliver a sector.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "chips/cdic/cdic.h"
#include "support/cdic_rig.h"
#include "support/fake_audio_cd.h"
#include "support/fake_disc.h"

using zcdi_test::CdicRig;
using zcdi_test::FakeDisc;
using zcdi_test::write_reg16;

namespace {

// Data sectors up to `end`; reads of `failing` fail `failures` times first.
class FailingDisc : public FakeDisc {
public:
    explicit FailingDisc(int end) : FakeDisc(std::vector<uint8_t>(40, zcdi_test::kSubmodeData)), end_(end) {}

    void fail(int lba, int times) {
        failing_ = lba;
        failures_ = times;
    }
    int attempts(int lba) const { return lba == failing_ ? attempts_ : 0; }

    bool read_sector(int lba, zcdi::RawSector& out) override {
        if (lba >= end_) return false;
        if (lba == failing_) {
            ++attempts_;
            if (failures_ > 0) {
                --failures_;
                return false;
            }
        }
        return FakeDisc::read_sector(lba, out);
    }
    int lead_out_lba() const override { return end_; }

private:
    int end_;
    int failing_ = -1, failures_ = 0, attempts_ = 0;
};

std::vector<uint8_t> markers(const std::vector<zcdi_test::Delivery>& got) {
    std::vector<uint8_t> out;
    for (const zcdi_test::Delivery& d : got) out.push_back(d.marker);
    return out;
}

}  // namespace

TEST_SUITE("cdic") {

// Green Book: reading past the lead-out is an error that ends the read.
TEST_CASE("a read past the end of the disc stops the drive") {
    FailingDisc disc(5);
    CdicRig rig({});
    rig.cdic.set_disc(&disc);
    rig.start_read(3);
    CHECK(markers(rig.collect(10)) == std::vector<uint8_t>{FakeDisc::marker(3), FakeDisc::marker(4)});
    CHECK_FALSE(rig.cdic.disc_active());
}

TEST_CASE("a sector that fails to read is tried again, not skipped") {
    FailingDisc disc(40);
    disc.fail(4, 5);
    CdicRig rig({});
    rig.cdic.set_disc(&disc);
    rig.start_read(3);
    CHECK(markers(rig.collect(3)) ==
          std::vector<uint8_t>{FakeDisc::marker(3), FakeDisc::marker(4), FakeDisc::marker(5)});
    CHECK(disc.attempts(4) == 6);
    CHECK(rig.cdic.disc_active());
}

TEST_CASE("a sector that never reads stops the drive") {
    FailingDisc disc(40);
    disc.fail(4, 1000);
    CdicRig rig({});
    rig.cdic.set_disc(&disc);
    rig.start_read(3);
    CHECK(markers(rig.collect(5)) == std::vector<uint8_t>{FakeDisc::marker(3)});
    CHECK_FALSE(rig.cdic.disc_active());
    CHECK(disc.attempts(4) < 1000);
}

TEST_CASE("Stop CDDA stops a playing audio CD") {
    zcdi_test::FakeAudioCd disc({10, 40});
    zcdi::chips::Cdic cdic;
    cdic.set_disc(&disc);
    zcdi_test::set_time(cdic, 10);
    write_reg16(cdic, 0x000, zcdi::chips::cdic::kCmdPlayCdda);
    write_reg16(cdic, 0x3fe, 0xc000);
    cdic.tick_cycles(100'000);
    REQUIRE(cdic.disc_active());
    write_reg16(cdic, 0x000, zcdi::chips::cdic::kCmdStopCdda);
    write_reg16(cdic, 0x3fe, 0xc000);
    CHECK_FALSE(cdic.disc_active());
}

TEST_CASE("the control registers read back, and DMA control reads 0") {
    zcdi::chips::Cdic cdic;
    using namespace zcdi::chips::cdic;
    write_reg16(cdic, kRegInterruptVector, 0x2470);
    write_reg16(cdic, kRegDmaControl, 0x4100);
    CHECK(zcdi_test::read_reg16(cdic, kRegInterruptVector) == 0x2470);
    CHECK(cdic.iack_vector() == 0x70);
    CHECK(zcdi_test::read_reg16(cdic, kRegDmaControl) == 0);
    CHECK(cdic.dma_control() == 0x4100);
}

}  // TEST_SUITE
