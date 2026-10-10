#pragma once

#include <functional>
#include <type_traits>
#include <utility>

namespace zcdi_test::private_access_detail {
template <class T, class C>
using member_ptr = T C::*;

template <class M>
struct member_owner;
template <class T, class C>
struct member_owner<T C::*> {
    using type = C;
};
}  // namespace zcdi_test::private_access_detail

namespace {
template <class Tag, typename Tag::type Ptr>
struct ZcdiPrivateMemberSteal {
    friend typename Tag::type zcdi_private_member(Tag) { return Ptr; }
};
}  // namespace

// At file scope: ZCDI_PRIVATE_MEMBER(Tag, Class, member, Type), then
// priv<Tag>(obj) is the field, or priv<Tag>(obj, args...) calls the method.
// Explicit instantiations skip access checks ([temp.spec]/6).
#define ZCDI_PRIVATE_MEMBER(Tag, Class, Member, ...)                                        \
    namespace {                                                                             \
    struct Tag {                                                                            \
        using type = ::zcdi_test::private_access_detail::member_ptr<__VA_ARGS__, Class>;    \
        friend type zcdi_private_member(Tag);                                               \
    };                                                                                      \
    }                                                                                       \
    template struct ZcdiPrivateMemberSteal<Tag, &Class::Member>

namespace zcdi_test {

template <class Tag, class Obj, class... Args>
decltype(auto) priv(Obj& obj, Args&&... args) {
    auto member = zcdi_private_member(Tag{});
    using Owner = typename private_access_detail::member_owner<decltype(member)>::type;
    using Ref = std::conditional_t<std::is_const_v<Obj>, const Owner&, Owner&>;
    // A C-style cast also reaches a private base (the class's saved-state structs).
    return std::invoke(member, (Ref)obj, std::forward<Args>(args)...);
}

}  // namespace zcdi_test
