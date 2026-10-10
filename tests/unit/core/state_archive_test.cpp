#include "doctest.h"

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "core/state_archive.h"
#include "support/state_roundtrip.h"

using zcdi::StateReader;
using zcdi::StateSizer;
using zcdi::StateWriter;
using zcdi::state_tag;

namespace {

enum class Mode : uint8_t { kA = 1, kB = 7 };

struct Sample {
    bool b = false;
    int8_t i8 = 0;
    uint16_t u16 = 0;
    int32_t i32 = 0;
    uint64_t u64 = 0;
    double d = 0;
    Mode mode = Mode::kA;
    uint16_t arr[3] = {};
    std::array<uint8_t, 4> bytes = {};
    std::vector<int16_t> vec;
    std::vector<uint8_t> ram = std::vector<uint8_t>(8);

    template <class T>
    void state_io(T& archive) {
        archive.begin_section(state_tag("SMPL"));
        archive.io(b);
        archive.io(i8);
        archive.io(u16);
        archive.io(i32);
        archive.io(u64);
        archive.io(d);
        archive.io(mode);
        archive.io_array(arr);
        archive.io_array(bytes);
        archive.io_vec(vec, 16);
        archive.io_fixed(ram);
        archive.end_section();
    }
};

Sample filled() {
    Sample s{
        .b = true,
        .i8 = -5,
        .u16 = 0xbeef,
        .i32 = -123456,
        .u64 = 0x0123456789abcdefull,
        .d = -0.1,
        .mode = Mode::kB,
        .arr = {1, 0xffff, 3},
        .bytes = {9, 8, 7, 6},
        .vec = {-1, 2, -3},
    };
    for (size_t i = 0; i < s.ram.size(); ++i) s.ram[i] = uint8_t(0x10 + i);
    return s;
}

std::vector<uint8_t> save(Sample s) { return zcdi_test::save_state(s); }

struct Point {
    int16_t x = 0, y = 0;
};

// Every field kind io_struct() handles.
struct Reflected {
    uint8_t in[3] = {};
    bool grid[2][2] = {};
    Point point;
    std::array<uint16_t, 2> pair{};
    std::vector<int32_t> samples;
    std::deque<Point> queue;
    Mode mode = Mode::kA;
    double d = 0;
};

Reflected reflected_filled() {
    Reflected r;
    r.in[2] = 9;
    r.grid[1][0] = true;
    r.point = {-2, 3};
    r.pair = {0x1234, 7};
    r.samples = {-1, 5};
    r.queue = {{1, 2}, {3, 4}};
    r.mode = Mode::kB;
    r.d = 2.5;
    return r;
}

}  // namespace

TEST_SUITE("core") {

TEST_CASE("io_struct stores every field in declaration order, as io() calls would") {
    Reflected r = reflected_filled();
    std::vector<uint8_t> bytes;
    StateWriter w(bytes);
    w.io_struct(r);

    std::vector<uint8_t> manual;
    StateWriter m(manual);
    m.io_array(r.in);
    for (auto& row : r.grid) m.io_array(row);
    m.io(r.point.x);
    m.io(r.point.y);
    m.io_array(r.pair);
    m.io_vec(r.samples, 16);
    uint32_t n = uint32_t(r.queue.size());
    m.io(n);
    for (Point& p : r.queue) {
        m.io(p.x);
        m.io(p.y);
    }
    m.io(r.mode);
    m.io(r.d);
    CHECK(bytes == manual);

    StateSizer sz;
    sz.io_struct(r);
    CHECK(sz.size() == bytes.size());

    Reflected back;
    StateReader rd(bytes);
    rd.io_struct(back);
    REQUIRE(!rd.failed());
    CHECK(rd.remaining() == 0);
    CHECK(back.in[2] == 9);
    CHECK(back.grid[1][0]);
    CHECK(back.point.y == 3);
    CHECK(back.pair[0] == 0x1234);
    CHECK(back.samples == r.samples);
    REQUIRE(back.queue.size() == 2);
    CHECK(back.queue[1].x == 3);
    CHECK(back.mode == Mode::kB);
    CHECK(back.d == 2.5);
}

TEST_CASE("io_struct rejects a container count larger than the data left") {
    Reflected r = reflected_filled();
    std::vector<uint8_t> bytes;
    StateWriter w(bytes);
    w.io_struct(r);
    // in(3) grid(4) point(4) pair(4) samples(4 + 8), then the queue count.
    bytes[3 + 4 + 4 + 4 + 12] = 0xff;
    Reflected back;
    StateReader rd(bytes);
    rd.io_struct(back);
    CHECK(rd.failed());
}

TEST_CASE("a state round-trips every field type") {
    std::vector<uint8_t> bytes = save(filled());
    Sample r;
    StateReader rd(bytes);
    r.state_io(rd);
    REQUIRE(!rd.failed());
    CHECK(rd.remaining() == 0);
    Sample e = filled();
    CHECK(r.b == e.b);
    CHECK(r.i8 == e.i8);
    CHECK(r.u16 == e.u16);
    CHECK(r.i32 == e.i32);
    CHECK(r.u64 == e.u64);
    CHECK(r.d == e.d);
    CHECK(r.mode == e.mode);
    CHECK(r.arr[1] == 0xffff);
    CHECK(r.bytes == e.bytes);
    CHECK(r.vec == e.vec);
    CHECK(r.ram == e.ram);
}

TEST_CASE("the sizer counts exactly what the writer writes") {
    Sample s = filled();
    StateSizer sz;
    s.state_io(sz);
    CHECK(sz.size() == save(s).size());
}

TEST_CASE("the format is little-endian") {
    uint32_t v = 0x11223344;
    std::vector<uint8_t> out;
    StateWriter w(out);
    w.io(v);
    CHECK(out == std::vector<uint8_t>{0x44, 0x33, 0x22, 0x11});
}

TEST_CASE("arrays are stored as little-endian elements, whatever the copy path") {
    uint32_t u32[2] = {0x11223344, 0xa0b0c0d0};
    int16_t i16[2] = {-2, 0x0102};
    float f[1] = {1.0f};
    Mode modes[2] = {Mode::kB, Mode::kA};
    std::vector<uint32_t> ram = {0xdeadbeef};
    std::vector<uint8_t> out;
    StateWriter w(out);
    w.io_array(u32);
    w.io_array(i16);
    w.io_array(f);
    w.io_array(modes);
    w.io_fixed(ram);
    CHECK(out == std::vector<uint8_t>{0x44, 0x33, 0x22, 0x11, 0xd0, 0xc0, 0xb0, 0xa0,
                                      0xfe, 0xff, 0x02, 0x01,
                                      0x00, 0x00, 0x80, 0x3f,
                                      7, 1,
                                      1, 0, 0, 0, 0xef, 0xbe, 0xad, 0xde});

    uint32_t u32_back[2] = {};
    int16_t i16_back[2] = {};
    float f_back[1] = {};
    Mode modes_back[2] = {};
    std::vector<uint32_t> ram_back(1);
    StateReader rd(out);
    rd.io_array(u32_back);
    rd.io_array(i16_back);
    rd.io_array(f_back);
    rd.io_array(modes_back);
    rd.io_fixed(ram_back);
    REQUIRE(!rd.failed());
    CHECK(u32_back[1] == 0xa0b0c0d0);
    CHECK(i16_back[0] == -2);
    CHECK(f_back[0] == 1.0f);
    CHECK(modes_back[0] == Mode::kB);
    CHECK(ram_back == ram);
}

TEST_CASE("a bool array byte other than 0 or 1 loads as true") {
    std::vector<uint8_t> bytes = {2, 0};
    bool flags[2] = {false, true};
    StateReader rd(bytes);
    rd.io_array(flags);
    REQUIRE(!rd.failed());
    CHECK(flags[0]);
    CHECK(!flags[1]);
}

TEST_CASE("a fixed buffer gets the same bytes as a growing one") {
    std::vector<uint8_t> grown = save(filled());
    std::vector<uint8_t> fixed(grown.size() + 3, 0xaa);
    Sample s = filled();
    StateWriter w(std::span<uint8_t>(fixed.data(), grown.size()));
    s.state_io(w);
    REQUIRE(!w.failed());
    CHECK(w.size() == grown.size());
    CHECK(std::vector<uint8_t>(fixed.begin(), fixed.begin() + grown.size()) == grown);
    CHECK(fixed[grown.size()] == 0xaa);
}

TEST_CASE("a fixed buffer too small fails without writing past it") {
    size_t need = save(filled()).size();
    for (size_t n : {size_t(0), size_t(5), need - 1}) {
        CAPTURE(n);
        std::vector<uint8_t> buf(need, 0xaa);
        Sample s = filled();
        StateWriter w(std::span<uint8_t>(buf.data(), n));
        s.state_io(w);
        CHECK(w.failed());
        for (size_t i = n; i < need; ++i) CHECK(buf[i] == 0xaa);
    }
}

TEST_CASE("every truncation of a state fails without reading past the end") {
    std::vector<uint8_t> bytes = save(filled());
    for (size_t n = 0; n < bytes.size(); ++n) {
        CAPTURE(n);
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + n);
        Sample r;
        StateReader rd(cut);
        r.state_io(rd);
        CHECK(rd.failed());
    }
}

TEST_CASE("a wrong section tag fails the load") {
    std::vector<uint8_t> bytes = save(filled());
    bytes[0] ^= 1;
    Sample r;
    StateReader rd(bytes);
    r.state_io(rd);
    CHECK(rd.failed());
}

TEST_CASE("a section longer than its fields fails the load") {
    std::vector<uint8_t> bytes = save(filled());
    bytes.push_back(0);
    bytes[4] += 1;  // section length, low byte
    Sample r;
    StateReader rd(bytes);
    r.state_io(rd);
    CHECK(rd.failed());
}

TEST_CASE("a vector longer than its maximum fails the load") {
    Sample s = filled();
    s.vec.assign(17, 0);
    std::vector<uint8_t> bytes = save(s);
    Sample r;
    StateReader rd(bytes);
    r.state_io(rd);
    CHECK(rd.failed());
}

TEST_CASE("a fixed buffer of another size fails the load") {
    Sample s = filled();
    s.ram.resize(9);
    std::vector<uint8_t> bytes = save(s);
    Sample r;
    StateReader rd(bytes);
    r.state_io(rd);
    CHECK(rd.failed());
    CHECK(r.ram.size() == 8);
}

}  // TEST_SUITE
