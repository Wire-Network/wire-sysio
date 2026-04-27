#pragma once

#include <fc/io/json_stream.hpp>
#include <fc/variant.hpp>

namespace fc {

/**
 *  Opt-in trait for types whose JSON form is the result of T::to_string() and
 *  whose JSON parse form is T::from_string(std::string_view).  Specializing
 *  this to std::true_type for a type makes all three serializers available
 *  without hand-rolled overloads:
 *
 *    fc::to_variant(t, v)        -> v = t.to_string()
 *    fc::from_variant(v, t)      -> t = T::from_string(v.get_string())
 *    fc::to_json_stream(t, w)    -> w.value_string(t.to_string())
 *
 *  Use the FC_SERIALIZE_AS_STRING macro for the one-line opt-in.
 *
 *  Requirements on the type:
 *    - std::string T::to_string() const            (or convertible-to-string return)
 *    - static T T::from_string(std::string_view)
 *
 *  Types whose to_variant is the FC_REFLECT'd struct shape do NOT specialize
 *  this; they fall through to the generic reflector dispatch.  Types whose
 *  shape is something else (number, struct, ISO date, etc.) need their own
 *  hand-rolled overload.
 */
template<typename T>
struct serialize_as_string : std::false_type {};

template<typename T>
   requires serialize_as_string<T>::value
inline void to_variant(const T& v, fc::variant& vo) {
   vo = v.to_string();
}

template<typename T>
   requires serialize_as_string<T>::value
inline void from_variant(const fc::variant& v, T& out) {
   out = T::from_string(v.get_string());
}

template<typename T>
   requires serialize_as_string<T>::value
inline void to_json_stream(const T& v, fc::json_writer& w) {
   w.value_string(v.to_string());
}

} // namespace fc

#define FC_SERIALIZE_AS_STRING(TYPE) \
   namespace fc { template<> struct serialize_as_string<TYPE> : std::true_type {}; }

// Class-template variant of FC_SERIALIZE_AS_STRING.  Wrap each argument in parens
// so internal commas (template parameter lists, template arguments) survive the
// preprocessor's argument splitting.
//   FC_SERIALIZE_AS_STRING_TEMPLATE((typename T, typename U), (my_type<T, U>))
#define FC_SERIALIZE_AS_STRING_TEMPLATE(TPL_PARAMS, TYPE) \
   namespace fc { template<FC_SERIALIZE_AS_STRING_UNPAREN_ TPL_PARAMS> \
                  struct serialize_as_string<FC_SERIALIZE_AS_STRING_UNPAREN_ TYPE> : std::true_type {}; }

#define FC_SERIALIZE_AS_STRING_UNPAREN_(...) __VA_ARGS__
