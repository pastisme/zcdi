#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace zcdi {

// One state_io(T& archive) field list per class drives StateWriter, StateReader
// and StateSizer. Little-endian; sections are tag + length.

// Load-time cap for unbounded buffers, against corrupt lengths.
constexpr size_t kStateMaxBuffer = size_t(16) << 20;

namespace state_detail {

template <class T>
constexpr bool kScalar = std::is_arithmetic_v<T> || std::is_enum_v<T>;

// Scalars whose memory bytes are their state bytes on this host, so arrays
// of them are copied whole. bool is excluded: any byte but 0/1 is UB.
template <class T>
constexpr bool kRawCopyable = (std::endian::native == std::endian::little) && kScalar<std::remove_cv_t<T>> &&
                              !std::is_same_v<std::remove_cv_t<T>, bool>;

template <class T>
auto to_bits(T v) {
    if constexpr (std::is_same_v<T, bool>) {
        return uint8_t(v ? 1 : 0);
    } else if constexpr (std::is_enum_v<T>) {
        return to_bits(static_cast<std::underlying_type_t<T>>(v));
    } else if constexpr (std::is_floating_point_v<T>) {
        static_assert(sizeof(T) == 8 || sizeof(T) == 4);
        if constexpr (sizeof(T) == 8)
            return std::bit_cast<uint64_t>(v);
        else
            return std::bit_cast<uint32_t>(v);
    } else {
        return static_cast<std::make_unsigned_t<T>>(v);
    }
}

template <class T, class U>
T from_bits(U bits) {
    if constexpr (std::is_same_v<T, bool>) {
        return bits != 0;
    } else if constexpr (std::is_enum_v<T>) {
        using Under = std::underlying_type_t<T>;
        return static_cast<T>(from_bits<Under>(bits));
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::bit_cast<T>(bits);
    } else {
        return static_cast<T>(bits);
    }
}

// Fields of a plain struct, without listing them: count them by aggregate
// initialisation (one braced initialiser per field), then bind them all (up to 130).
struct AnyField {
    template <class T>
    operator T() const;
};

template <class T, class... A>
consteval size_t field_count() {
    if constexpr (requires { T{{A{}}..., {AnyField{}}}; }) {
        return field_count<T, A..., AnyField>();
    } else {
        return sizeof...(A);
    }
}

#define ZCDI_U1(m) m##0
#define ZCDI_U2(m) ZCDI_U1(m), m##1
#define ZCDI_U3(m) ZCDI_U2(m), m##2
#define ZCDI_U4(m) ZCDI_U3(m), m##3
#define ZCDI_U5(m) ZCDI_U4(m), m##4
#define ZCDI_U6(m) ZCDI_U5(m), m##5
#define ZCDI_U7(m) ZCDI_U6(m), m##6
#define ZCDI_U8(m) ZCDI_U7(m), m##7
#define ZCDI_U9(m) ZCDI_U8(m), m##8
#define ZCDI_U10(m) ZCDI_U9(m), m##9
#define ZCDI_F0
#define ZCDI_F1 ZCDI_U10(a0),
#define ZCDI_F2 ZCDI_F1 ZCDI_U10(a1),
#define ZCDI_F3 ZCDI_F2 ZCDI_U10(a2),
#define ZCDI_F4 ZCDI_F3 ZCDI_U10(a3),
#define ZCDI_F5 ZCDI_F4 ZCDI_U10(a4),
#define ZCDI_F6 ZCDI_F5 ZCDI_U10(a5),
#define ZCDI_F7 ZCDI_F6 ZCDI_U10(a6),
#define ZCDI_F8 ZCDI_F7 ZCDI_U10(a7),
#define ZCDI_F9 ZCDI_F8 ZCDI_U10(a8),
#define ZCDI_F10 ZCDI_F9 ZCDI_U10(a9),
#define ZCDI_F11 ZCDI_F10 ZCDI_U10(a10),
#define ZCDI_F12 ZCDI_F11 ZCDI_U10(a11),
#define ZCDI_CASE(n, ...)          \
    else if constexpr (N == (n)) { \
        auto& [__VA_ARGS__] = s;   \
        f(__VA_ARGS__);            \
    }
#define ZCDI_TEN(t)                                \
    ZCDI_CASE(t * 10 + 1, ZCDI_F##t ZCDI_U1(a##t)) \
    ZCDI_CASE(t * 10 + 2, ZCDI_F##t ZCDI_U2(a##t)) \
    ZCDI_CASE(t * 10 + 3, ZCDI_F##t ZCDI_U3(a##t)) \
    ZCDI_CASE(t * 10 + 4, ZCDI_F##t ZCDI_U4(a##t)) \
    ZCDI_CASE(t * 10 + 5, ZCDI_F##t ZCDI_U5(a##t)) \
    ZCDI_CASE(t * 10 + 6, ZCDI_F##t ZCDI_U6(a##t)) \
    ZCDI_CASE(t * 10 + 7, ZCDI_F##t ZCDI_U7(a##t)) \
    ZCDI_CASE(t * 10 + 8, ZCDI_F##t ZCDI_U8(a##t)) \
    ZCDI_CASE(t * 10 + 9, ZCDI_F##t ZCDI_U9(a##t)) \
    ZCDI_CASE(t * 10 + 10, ZCDI_F##t ZCDI_U10(a##t))

// Calls f(field0, field1, ...) on the fields of s.
template <class S, class F>
void for_fields(S& s, F&& f) {
    constexpr size_t N = field_count<S>();
    static_assert(N <= 130, "split this struct");
    if constexpr (N == 0) return;
    ZCDI_TEN(0)
    ZCDI_TEN(1)
    ZCDI_TEN(2)
    ZCDI_TEN(3)
    ZCDI_TEN(4)
    ZCDI_TEN(5)
    ZCDI_TEN(6)
    ZCDI_TEN(7)
    ZCDI_TEN(8)
    ZCDI_TEN(9)
    ZCDI_TEN(10)
    ZCDI_TEN(11)
    ZCDI_TEN(12)
}

#undef ZCDI_U1
#undef ZCDI_U2
#undef ZCDI_U3
#undef ZCDI_U4
#undef ZCDI_U5
#undef ZCDI_U6
#undef ZCDI_U7
#undef ZCDI_U8
#undef ZCDI_U9
#undef ZCDI_U10
#undef ZCDI_F0
#undef ZCDI_F1
#undef ZCDI_F2
#undef ZCDI_F3
#undef ZCDI_F4
#undef ZCDI_F5
#undef ZCDI_F6
#undef ZCDI_F7
#undef ZCDI_F8
#undef ZCDI_F9
#undef ZCDI_F10
#undef ZCDI_F11
#undef ZCDI_F12
#undef ZCDI_CASE
#undef ZCDI_TEN

template <class T>
constexpr bool kStdArray = false;
template <class T, size_t N>
constexpr bool kStdArray<std::array<T, N>> = true;
template <class T>
constexpr bool kScalarVector = false;
template <class T>
constexpr bool kScalarVector<std::vector<T>> = kScalar<T>;

// One field of a state struct, for any of the three archives.
template <class A, class T>
void io_field(A& archive, T& v) {
    if constexpr (kScalar<T>) {
        archive.io(v);
    } else if constexpr (std::is_array_v<T> || kStdArray<T>) {
        if constexpr (kScalar<std::remove_cvref_t<decltype(v[0])>>) {
            archive.io_array(v);
        } else {
            for (auto& e : v)
                io_field(archive, e);
        }
    } else if constexpr (kScalarVector<T>) {
        archive.io_vec(v, kStateMaxBuffer);
    } else if constexpr (requires { v.resize(0); }) {
        uint32_t n = uint32_t(v.size());
        archive.io(n);
        if constexpr (A::kLoading) {
            archive.check(n <= archive.remaining());
            if (archive.failed()) return;
            v.resize(n);
        }
        for (auto& e : v)
            io_field(archive, e);
    } else {
        static_assert(std::is_aggregate_v<T>, "state fields: scalars, arrays, containers, structs");
        for_fields(v, [&](auto&... f) { (io_field(archive, f), ...); });
    }
}

} // namespace state_detail

// CRC-32 (IEEE), to fingerprint the ROMs a state was made with.
inline uint32_t state_crc32(std::span<const uint8_t> data) {
    uint32_t crc = 0xffffffffu;
    for (uint8_t b : data) {
        crc ^= b;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Four-character section tag, e.g. state_tag("CDIC").
constexpr uint32_t state_tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// The struct/array forms, shared by the three archives through io_span().
template <class Archive>
class StateArchiveBase {
public:
    // Every field of a plain struct (see for_fields).
    template <class S>
    void io_struct(S& s) {
        state_detail::io_field(self(), s);
    }

    template <class T, size_t N>
    void io_array(T (&a)[N]) {
        self().io_span(std::span<T>(a, N));
    }

    template <class T, size_t N>
    void io_array(std::array<T, N>& a) {
        self().io_span(std::span<T>(a));
    }

    template <class T>
    void io_span(std::span<T> s) {
        if constexpr (state_detail::kRawCopyable<T>) {
            self().raw(s.data(), s.size_bytes());
        } else {
            for (T& v : s)
                self().io(v);
        }
    }

private:
    Archive& self() { return static_cast<Archive&>(*this); }
};

class StateWriter : public StateArchiveBase<StateWriter> {
public:
    static constexpr bool kLoading = false;

    explicit StateWriter(std::vector<uint8_t>& out) : out_(&out) {}
    // Writes into a fixed buffer; failed() once it would overflow.
    explicit StateWriter(std::span<uint8_t> fixed) : fixed_(fixed) {}

    template <class T>
    void io(T& v) {
        static_assert(state_detail::kScalar<T>);
        auto bits = state_detail::to_bits(v);
        if constexpr (std::endian::native == std::endian::little) {
            raw(&bits, sizeof(bits));
        } else {
            for (size_t i = 0; i < sizeof(bits); ++i) {
                uint8_t b = uint8_t(uint64_t(bits) >> (8 * i));
                raw(&b, 1);
            }
        }
    }

    // Fixed-size buffer (RAM): the length is checked on load, not resized.
    template <class T>
    void io_fixed(std::vector<T>& v) {
        uint32_t n = uint32_t(v.size());
        io(n);
        io_span(std::span<T>(v));
    }

    // Length-prefixed vector, saved as io_fixed. `max` only matters on load.
    template <class T>
    void io_vec(std::vector<T>& v, size_t /*max*/) {
        io_fixed(v);
    }

    void raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        if (out_) {
            out_->insert(out_->end(), b, b + n);
        } else if (n <= fixed_.size() - pos_) {
            std::memcpy(fixed_.data() + pos_, b, n);
            pos_ += n;
        } else {
            overflow_ = true;
            pos_ = fixed_.size();
        }
    }

    size_t size() const { return out_ ? out_->size() : pos_; }

    void begin_section(uint32_t tag) {
        io(tag);
        uint32_t placeholder = 0;
        section_starts_.push_back(size());
        io(placeholder);
    }

    void end_section() {
        size_t at = section_starts_.back();
        section_starts_.pop_back();
        if (overflow_) return;
        uint8_t* base = out_ ? out_->data() : fixed_.data();
        uint32_t len = uint32_t(size() - at - 4);
        for (int i = 0; i < 4; ++i)
            base[at + i] = uint8_t(len >> (8 * i));
    }

    // Load-time range check for a field used as an index or size; a no-op
    // when saving or sizing.
    void check(bool) {}
    bool failed() const { return overflow_; }

private:
    std::vector<uint8_t>* out_ = nullptr;
    std::span<uint8_t> fixed_{};
    size_t pos_ = 0;
    bool overflow_ = false;
    std::vector<size_t> section_starts_;
};

class StateReader : public StateArchiveBase<StateReader> {
public:
    static constexpr bool kLoading = true;

    explicit StateReader(std::span<const uint8_t> in) : in_(in) {}

    template <class T>
    void io(T& v) {
        static_assert(state_detail::kScalar<T>);
        using Bits = decltype(state_detail::to_bits(v));
        if (!take(sizeof(Bits))) {
            v = T{};
            return;
        }
        if constexpr (std::endian::native == std::endian::little) {
            Bits b{};
            std::memcpy(&b, in_.data() + pos_ - sizeof(Bits), sizeof(Bits));
            v = state_detail::from_bits<T>(b);
        } else {
            uint64_t bits = 0;
            for (size_t i = 0; i < sizeof(Bits); ++i)
                bits |= uint64_t(in_[pos_ - sizeof(Bits) + i]) << (8 * i);
            v = state_detail::from_bits<T>(Bits(bits));
        }
    }

    template <class T>
    void io_vec(std::vector<T>& v, size_t max) {
        uint32_t n = 0;
        io(n);
        if (failed_ || n > max || n > remaining()) {
            fail();
            v.clear();
            return;
        }
        v.resize(n);
        io_span(std::span<T>(v));
    }

    template <class T>
    void io_fixed(std::vector<T>& v) {
        uint32_t n = 0;
        io(n);
        if (n != v.size()) {
            fail();
            return;
        }
        io_span(std::span<T>(v));
    }

    void raw(void* p, size_t n) {
        if (!take(n)) {
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, in_.data() + pos_ - n, n);
    }

    void begin_section(uint32_t tag) {
        uint32_t got = 0, len = 0;
        io(got);
        io(len);
        if (got != tag || len > remaining()) fail();
        section_ends_.push_back(failed_ ? pos_ : pos_ + len);
    }

    // Steps over a whole section without reading its fields.
    void skip_section(uint32_t tag) {
        uint32_t got = 0, len = 0;
        io(got);
        io(len);
        if (got != tag || !take(len)) fail();
    }

    void end_section() {
        if (section_ends_.empty()) {
            fail();
            return;
        }
        if (pos_ != section_ends_.back()) fail();
        section_ends_.pop_back();
    }

    void check(bool ok) {
        if (!ok) fail();
    }
    bool failed() const { return failed_; }
    void fail() { failed_ = true; }
    size_t remaining() const { return in_.size() - pos_; }

private:
    bool take(size_t n) {
        if (failed_ || n > remaining()) {
            failed_ = true;
            return false;
        }
        pos_ += n;
        return true;
    }

    std::span<const uint8_t> in_;
    size_t pos_ = 0;
    bool failed_ = false;
    std::vector<size_t> section_ends_;
};

class StateSizer : public StateArchiveBase<StateSizer> {
public:
    static constexpr bool kLoading = false;

    template <class T>
    void io(T& v) {
        size_ += sizeof(state_detail::to_bits(v));
    }

    // Hides the base's: sizes without touching the elements.
    template <class T>
    void io_span(std::span<T> s) {
        if (!s.empty()) size_ += s.size() * sizeof(state_detail::to_bits(s[0]));
    }

    template <class T>
    void io_fixed(std::vector<T>& v) {
        size_ += 4;
        io_span(std::span<T>(v));
    }

    template <class T>
    void io_vec(std::vector<T>& v, size_t) {
        io_fixed(v);
    }

    void raw(const void*, size_t n) { size_ += n; }
    void begin_section(uint32_t) { size_ += 8; }
    void end_section() {}
    void check(bool) {}
    bool failed() const { return false; }

    size_t size() const { return size_; }

private:
    size_t size_ = 0;
};

} // namespace zcdi

// Use once in the class's .cpp, after the out-of-line state_io definition.
#define ZCDI_STATE_IO_INSTANTIATE(Class)                                      \
    template void Class::state_io<::zcdi::StateWriter>(::zcdi::StateWriter&); \
    template void Class::state_io<::zcdi::StateReader>(::zcdi::StateReader&); \
    template void Class::state_io<::zcdi::StateSizer>(::zcdi::StateSizer&)
