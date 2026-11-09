#pragma once

#include <fc/io/datastream.hpp>
#include <fc/io/raw.hpp>
#include <fc/variant.hpp>

#include <optional>
#include <utility>

namespace sysio::chain {

/**
 * An optional field at the end of an action's data, encoded as the CDT's `binary_extension<T>` is: the field is
 * present when bytes remain where it starts, and it adds no bytes when absent.
 *
 * The host declares one wherever the system contract adds such a field to a native action, so that the host's
 * struct describes all of the action's data and nothing past it.
 *
 * @tparam T - The field's type
 */
template <typename T>
struct binary_extension {
   std::optional<T> value;

   /// True when the field is present.
   bool has_value() const { return value.has_value(); }

   friend bool operator==(const binary_extension&, const binary_extension&) = default;
};

} // namespace sysio::chain

namespace fc {

/// Packs the field when it is present, and nothing when it is absent.
template <typename ST, typename T>
datastream<ST>& operator<<(datastream<ST>& s, const sysio::chain::binary_extension<T>& v) {
   if (v.value)
      raw::pack(s, *v.value);
   return s;
}

/// Reads the field when bytes remain, and leaves it absent otherwise.
template <typename ST, typename T>
datastream<ST>& operator>>(datastream<ST>& s, sysio::chain::binary_extension<T>& v) {
   if (s.remaining()) {
      T value;
      raw::unpack(s, value);
      v.value = std::move(value);
   } else {
      v.value.reset();
   }
   return s;
}

/// An absent field converts to null, a present one to its value.
template <typename T>
void to_variant(const sysio::chain::binary_extension<T>& e, fc::variant& v) {
   if (e.value)
      to_variant(*e.value, v);
   else
      v = fc::variant();
}

/// Null converts to an absent field, anything else to a present one.
template <typename T>
void from_variant(const fc::variant& v, sysio::chain::binary_extension<T>& e) {
   if (v.is_null()) {
      e.value.reset();
   } else {
      T value;
      from_variant(v, value);
      e.value = std::move(value);
   }
}

} // namespace fc
