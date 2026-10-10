#pragma once
typedef struct mpeg2dec_s mpeg2dec_t;

namespace zcdi {
namespace dvc {

// The whole libmpeg2 decoder. Loading needs a decoder fresh from mpeg2_init().
template <class T>
void mpeg2_state_io(T& archive, mpeg2dec_t* dec);

} // namespace dvc
} // namespace zcdi
