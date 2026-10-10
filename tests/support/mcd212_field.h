#pragma once
// One whole field through Mcd212's line API, as Bus drives it.
#include <cstddef>
#include <cstdint>

#include "mcd212/mcd212.h"

namespace zcdi_test {

// Returns the field's line count (one more in even interlaced fields).
inline int run_field(zcdi::mamecdi::Mcd212& mcd, const uint8_t* plane_a, const uint8_t* plane_b,
                     std::size_t plane_size, int lines = 312) {
    const int field_lines = mcd.begin_field(lines);
    for (int line = 0; line < field_lines; ++line) mcd.end_of_line(line, plane_a, plane_b, plane_size);
    mcd.end_field();
    return field_lines;
}

}  // namespace zcdi_test
