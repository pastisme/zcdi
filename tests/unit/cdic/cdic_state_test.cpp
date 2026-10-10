#include "doctest.h"

#include <cstdint>
#include <vector>

#include "support/cdic_rig.h"
#include "support/fake_disc.h"
#include "support/state_roundtrip.h"

using zcdi_test::CdicRig;
using zcdi_test::Delivery;
using zcdi_test::kSubmodeAudio;
using zcdi_test::kSubmodeData;

TEST_SUITE("cdic") {

TEST_CASE("a CDIC restored mid-read delivers the same sectors") {
    const std::vector<uint8_t> submodes = {kSubmodeAudio, kSubmodeData, kSubmodeData, kSubmodeData,
                                           kSubmodeAudio, kSubmodeData, kSubmodeData, kSubmodeData,
                                           kSubmodeAudio, kSubmodeData, kSubmodeData, kSubmodeData};
    CdicRig a(submodes);
    a.start_read(0);
    REQUIRE(a.collect(3).size() == 3);
    a.cdic.tick_cycles(12345);  // mid-sector: the cycle accumulator matters

    const std::vector<uint8_t> state = zcdi_test::save_state(a.cdic);
    CHECK(zcdi_test::measure_state(a.cdic) == state.size());
    CdicRig b(submodes);
    REQUIRE(zcdi_test::load_state(b.cdic, state));
    CHECK(zcdi_test::save_state(b.cdic) == state);

    const std::vector<Delivery> da = a.collect(6);
    const std::vector<Delivery> db = b.collect(6);
    REQUIRE(da.size() == 6);
    REQUIRE(db.size() == da.size());
    for (size_t i = 0; i < da.size(); ++i) {
        CAPTURE(i);
        CHECK(db[i].dbuf == da[i].dbuf);
        CHECK(db[i].submode == da[i].submode);
        CHECK(db[i].marker == da[i].marker);
    }
    CHECK(zcdi_test::save_state(b.cdic) == zcdi_test::save_state(a.cdic));
}

TEST_CASE("a truncated CDIC state is rejected") {
    CdicRig a({kSubmodeData});
    std::vector<uint8_t> state = zcdi_test::save_state(a.cdic);
    state.pop_back();
    CdicRig b({kSubmodeData});
    CHECK(!zcdi_test::load_state(b.cdic, state));
}

}  // TEST_SUITE
