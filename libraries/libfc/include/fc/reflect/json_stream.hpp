#pragma once

#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace fc {

/**
 *  to_json_stream is the streaming counterpart to to_variant: it emits the JSON form of a
 *  reflected C++ value directly into a json_writer without building an intermediate
 *  fc::variant tree.  The primary template dispatches through fc::reflector in the same
 *  way fc::to_variant does, so any type already marked with FC_REFLECT(...) gets a JSON
 *  serializer for free.
 *
 *  Overloads for primitives and standard containers are provided below.  Domain-specific
 *  types (sha256, public_key_type, variant, time_point, ...) can add their own
 *  to_json_stream overload next to their existing to_variant overload; until that happens
 *  callers can fall back to raw_value(fc::json::to_string(variant(v))) for individual
 *  fields at measured hot spots.
 */
template<typename T>
void to_json_stream(const T& o, json_writer& w);

// -- Scalar overloads ---------------------------------------------------------------------

inline void to_json_stream(bool b, json_writer& w)                 { w.value_bool(b); }
inline void to_json_stream(char c, json_writer& w)                 { w.value_int8(static_cast<int8_t>(c)); }
inline void to_json_stream(int8_t n, json_writer& w)               { w.value_int8(n); }
inline void to_json_stream(uint8_t n, json_writer& w)              { w.value_uint8(n); }
inline void to_json_stream(int16_t n, json_writer& w)              { w.value_int16(n); }
inline void to_json_stream(uint16_t n, json_writer& w)             { w.value_uint16(n); }
inline void to_json_stream(int32_t n, json_writer& w)              { w.value_int32(n); }
inline void to_json_stream(uint32_t n, json_writer& w)             { w.value_uint32(n); }
inline void to_json_stream(int64_t n, json_writer& w)              { w.value_int64(n); }
inline void to_json_stream(uint64_t n, json_writer& w)             { w.value_uint64(n); }
inline void to_json_stream(float f, json_writer& w)                { w.value_double(static_cast<double>(f)); }
inline void to_json_stream(double d, json_writer& w)               { w.value_double(d); }
inline void to_json_stream(std::string_view s, json_writer& w)     { w.value_string(s); }
inline void to_json_stream(const std::string& s, json_writer& w)   { w.value_string(s); }
inline void to_json_stream(const char* s, json_writer& w)          { w.value_string(s ? std::string_view(s) : std::string_view()); }

// Container overloads (std::vector, std::array, std::optional, std::map, std::set,
// std::pair, std::deque, std::unordered_map, std::unordered_set, std::multimap) live
// alongside their to_variant siblings in fc/variant.hpp.  flat_set / flat_multiset live
// in fc/container/flat.hpp.  Co-locating with to_variant keeps the two paths' shape
// and MAX_NUM_ARRAY_ELEMENTS guards in lock-step.

// -- Reflector visitor --------------------------------------------------------------------

template<typename T>
class to_json_stream_visitor {
public:
   to_json_stream_visitor(json_writer& w, const T& v)
   : w_(w), val_(v) {}

   template<typename Member, class Class, Member (Class::*member)>
   void operator()(const char* name) const {
      emit(name, val_.*member);
   }

private:
   template<typename M>
   void emit(const char* name, const std::optional<M>& v) const {
      // Mirror to_variant_visitor::add semantics: unset optionals are omitted rather than
      // emitted as explicit null, so downstream JSON matches the existing HTTP responses.
      if (v) {
         w_.key(name);
         to_json_stream(*v, w_);
      }
   }

   template<typename M>
   void emit(const char* name, const M& v) const {
      w_.key(name);
      to_json_stream(v, w_);
   }

   json_writer& w_;
   const T&     val_;
};

// -- Dispatch for reflected user types ---------------------------------------------------

namespace detail {

   template<typename IsEnum = fc::false_type>
   struct if_enum_json {
      template<typename T>
      static void to_json_stream(const T& v, json_writer& w) {
         w.begin_object();
         fc::reflector<T>::visit(to_json_stream_visitor<T>(w, v));
         w.end_object();
      }
   };

   template<>
   struct if_enum_json<fc::true_type> {
      template<typename T>
      static void to_json_stream(const T& o, json_writer& w) {
         w.value_string(fc::reflector<T>::to_fc_string(o));
      }
   };

} // namespace detail

template<typename T>
void to_json_stream(const T& o, json_writer& w) {
   detail::if_enum_json<typename fc::reflector<T>::is_enum>::to_json_stream(o, w);
}

// -- Convenience entry point --------------------------------------------------------------

/// One-shot helper: serialize `v` into a freshly-allocated std::string.  Hot-path callers
/// should construct json_writer directly against their output buffer to avoid the copy.
template<typename T>
std::string to_json_string(const T& v) {
   std::string out;
   {
      json_writer w(out);
      to_json_stream(v, w);
   }
   return out;
}

/// Explicit escape hatch for types that already have a to_variant overload but have not
/// yet grown a native to_json_stream.  Emits the value by converting to fc::variant and
/// running fc::json::to_string, then splicing the result at a value position.  Use at
/// migrated-endpoint field granularity to avoid blocking on a full library sweep.
template<typename T>
void to_json_stream_via_variant(const T& v, json_writer& w) {
   fc::variant tmp;
   fc::to_variant(v, tmp);
   w.raw_value(fc::json::to_string(tmp, fc::json::yield_function_t()));
}

} // namespace fc
