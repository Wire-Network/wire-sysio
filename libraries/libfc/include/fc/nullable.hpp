#pragma once

#include <optional>
#include <utility>

namespace fc {

/**
 *  Optional-valued serialization wrapper whose JSON key is ALWAYS present.
 *
 *  Reflected std::optional members are OMITTED from serialized output when disengaged --
 *  both the to_variant reflector (to_variant_visitor::add) and the streaming reflector
 *  (to_json_stream_visitor::emit) skip them.  Some endpoint schemas instead pin
 *  "key always present, value null when absent"; eg get_finalizer_info's active/pending
 *  policy fields, whose original fc::variant representation emitted explicit nulls that
 *  clients dereference before testing.  Wrapping such a member as fc::nullable<T> keeps
 *  that shape while the member stays strongly typed:
 *
 *    - engaged    -> serializes exactly like a plain T member
 *    - disengaged -> serializes as an explicit JSON null under the member's key
 *
 *  from_variant mirrors the emission: a JSON null deserializes to the disengaged state.
 *
 *  The serializer overloads (to_variant / from_variant / to_json_stream) live in
 *  fc/variant.hpp alongside the std::optional trio so the two families' shapes stay in
 *  lock-step; this header carries only the vocabulary type.
 */
template<typename T>
struct nullable {
   std::optional<T> value;

   nullable() = default;
   nullable(const T& v) : value(v) {}
   nullable(T&& v) : value(std::move(v)) {}
   nullable& operator=(const T& v) { value = v; return *this; }
   nullable& operator=(T&& v) { value = std::move(v); return *this; }

   bool has_value() const { return value.has_value(); }
   explicit operator bool() const { return value.has_value(); }
   const T& operator*() const { return *value; }
   T& operator*() { return *value; }
   const T* operator->() const { return &*value; }
   T* operator->() { return &*value; }
   void reset() { value.reset(); }

   friend bool operator==(const nullable& a, const nullable& b) { return a.value == b.value; }
};

} // namespace fc
