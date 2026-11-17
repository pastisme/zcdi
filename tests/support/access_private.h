#pragma once
// Private members for tests, declared once per member at file scope in a
// tests/support/*_private.h header:
//   ACCESS_PRIVATE_FIELD(Class, Type, member)  then access_private::member(obj)
//   ACCESS_PRIVATE_FUN(Class, Type(Args), fn)  then call_private::fn(obj, args...)
// Methods take a non-const object.
#include "access_private/access_private.hpp"

namespace zcdi_test {

// The public fields of a class's private saved-state base. A C-style cast is
// the one cast allowed to reach a private base.
template <class Base, class Derived>
Base& saved_state(Derived& obj) {
    return (Base&)obj;
}

}  // namespace zcdi_test
