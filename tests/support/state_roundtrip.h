#pragma once

#include <cstdint>
#include <vector>

#include "core/state_archive.h"

namespace zcdi_test {

template <class T>
std::vector<uint8_t> save_state(T& obj) {
    std::vector<uint8_t> out;
    zcdi::StateWriter w(out);
    obj.state_io(w);
    return out;
}

// True when every byte was consumed and nothing failed.
template <class T>
bool load_state(T& obj, const std::vector<uint8_t>& bytes) {
    zcdi::StateReader r(bytes);
    obj.state_io(r);
    return !r.failed() && r.remaining() == 0;
}

// Offset of the first byte where two states differ, or -1 when they are
// equal: a failing CHECK prints one number instead of two whole states.
inline long first_difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return long(i);
    }
    return a.size() == b.size() ? -1 : long(n);
}

template <class T>
size_t measure_state(T& obj) {
    zcdi::StateSizer s;
    obj.state_io(s);
    return s.size();
}

}  // namespace zcdi_test
