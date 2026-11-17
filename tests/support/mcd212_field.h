#pragma once
// One whole field through Mcd212's line API, as Board drives it.
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "chips/mcd212/mcd212.h"

namespace zcdi_test {

// Returns the field's line count (one more in even interlaced fields).
inline int run_field(zcdi::chips::Mcd212& mcd, const uint8_t* plane_a, const uint8_t* plane_b,
                     std::size_t plane_size, int lines = 312) {
    const int field_lines = mcd.begin_field(lines);
    for (int line = 0; line < field_lines; ++line) mcd.end_of_line(line, plane_a, plane_b, plane_size);
    mcd.end_field();
    return field_lines;
}

// Big-endian, as plane memory holds ICA/DCA instructions.
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// Instructions from byte `at` of `plane` on.
inline void put_list(uint8_t* plane, uint32_t at, std::initializer_list<uint32_t> instructions) {
    for (uint32_t cmd : instructions) {
        put32(plane + at, cmd);
        at += 4;
    }
}

// Framebuffer pixel: `row` counts both woven fields, `x` is in double-resolution pixels.
inline uint32_t pixel(const zcdi::chips::Mcd212& mcd, int x, int row) {
    return mcd.framebuffer()[std::size_t(row) * std::size_t(mcd.fb_width()) + std::size_t(x)];
}

}  // namespace zcdi_test
