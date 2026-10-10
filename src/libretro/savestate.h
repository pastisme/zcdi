#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zcdi {

class Machine;
class Mixer;
struct InputHistory;

// The Machine's state, then the mixer queue and input history.
void save_core_state(Machine& machine, Mixer& audio, InputHistory& input, std::vector<uint8_t>& out);
// Writes straight into `out`; returns the bytes written, or 0 if it doesn't fit.
size_t save_core_state(Machine& machine, Mixer& audio, InputHistory& input, std::span<uint8_t> out);
size_t core_state_size(Machine& machine, Mixer& audio, InputHistory& input);

// All or nothing: on false nothing has changed.
bool load_core_state(Machine& machine, Mixer& audio, InputHistory& input, std::span<const uint8_t> data);

// What retro_serialize_size() reports: it may not grow during a session
// unless the frontend re-queries (FRONT_VARIABLE_SIZE).
class StateSizeReporter {
public:
    static constexpr size_t kFixedSize = size_t(16) << 20;

    explicit StateSizeReporter(bool frontend_variable_size = false) : variable_(frontend_variable_size) {}

    size_t report(size_t needed) {
        if (variable_) return needed;
        if (fixed_ == 0) fixed_ = needed > kFixedSize ? needed : kFixedSize;
        return fixed_;
    }

    bool variable() const { return variable_; }

private:
    bool variable_ = false;
    size_t fixed_ = 0;
};

} // namespace zcdi
