#include <sysio/chain/abi_bounds.hpp>

#include <fc/io/raw.hpp>
#include <sysio/chain/abi_def.hpp>
#include <sysio/chain/exceptions.hpp>

#include <type_traits>

namespace sysio::chain {
namespace {
constexpr auto nesting_limit_message = "Packed ABI nesting limit exceeded";
constexpr auto element_limit_message = "Packed ABI element limit exceeded";
constexpr auto length_overflow_message = "Packed ABI length overflows uint32";
constexpr auto unterminated_length_message = "Unterminated packed ABI length";
constexpr auto truncated_length_message = "Packed ABI length exceeds remaining input";
constexpr auto storage_limit_message = "Packed ABI decoded storage limit exceeded";
constexpr auto packed_size_limit_message = "Packed ABI size limit exceeded";

/// Recognize only the containers and trailing fields used in abi_def.
template <typename T> struct abi_vector : std::false_type {};
template <typename T> struct abi_vector<std::vector<T>> : std::true_type {};
template <typename T> struct abi_pair : std::false_type {};
template <typename A, typename B> struct abi_pair<std::pair<A, B>> : std::true_type {};
template <typename T> struct abi_trailing : std::false_type {};
template <typename T> struct abi_trailing<may_not_exist<T>> : std::true_type {};

/// Scan ABI fields in FC packing order without constructing any input-sized objects.
class abi_bounds_reader {
public:
   /// Bind packed bytes after the publication entry point has checked their size.
   abi_bounds_reader(const char* data, size_t size) : input(data, size) {}

   /// Check lengths and conservative decoded-storage charges without interpreting ABI type names.
   template <typename T>
   void check(size_t depth = 0) {
      SYS_ASSERT(depth < abi_limits::max_depth, abi_recursion_depth_exception, nesting_limit_message);
      if constexpr (std::is_same_v<T, std::string>) {
         const auto size = length();
         require_bytes(size);
         charge(size_t(size) + 1, string_storage_factor);
         input.skip(size);
      } else if constexpr (abi_vector<T>::value) {
         using element = typename T::value_type;
         const auto count = length();
         // Every supported element requires at least one packed byte; its fields are checked below.
         require_bytes(count);
         SYS_ASSERT(count <= elements_left, abi_exception, element_limit_message);
         elements_left -= count;
         if constexpr (std::is_same_v<element, char>) {
            charge(count, 1);
            input.skip(count);
         } else {
            static_assert(sizeof(element) <= vector_slot_bytes, "Increase the deterministic ABI slot charge");
            charge(count, vector_slot_bytes);
            for (uint32_t i = 0; i < count; ++i)
               check<element>(depth + 1);
         }
      } else if constexpr (abi_trailing<T>::value) {
         if (input.remaining())
            check<decltype(T::value)>(depth + 1);
      } else if constexpr (abi_pair<T>::value) {
         check<typename T::first_type>(depth + 1);
         check<typename T::second_type>(depth + 1);
      } else if constexpr (fc::reflector<T>::is_defined::value) {
         fc::reflector<T>::visit(visitor{*this, depth + 1});
      } else {
         static_assert(std::is_integral_v<T>, "Unsupported packed ABI field: add a bounds checker");
         require_bytes(sizeof(T));
         input.skip(sizeof(T));
      }
   }

private:
   /// Visit field types without constructing their containing ABI objects.
   struct visitor {
      abi_bounds_reader& reader;
      size_t depth;
      template <typename Member, typename Class, Member Class::*Ptr>
      void operator()(const char*) const { reader.check<Member>(depth); }
   };

   /// Decode a uint32 length without accepting overflow, truncated input, or unbounded continuations.
   uint32_t length() {
      uint32_t value = 0;
      for (uint32_t i = 0; i < varuint32_bytes; ++i) {
         uint8_t byte = 0;
         require_bytes(1);
         fc::raw::unpack(input, byte);
         SYS_ASSERT(i != varuint32_bytes - 1 || (byte & final_byte_overflow_mask) == 0,
                    abi_exception, length_overflow_message);
         value |= uint32_t(byte & payload_mask) << (i * payload_bits);
         if ((byte & continuation_mask) == 0)
            return value;
      }
      FC_THROW_EXCEPTION(abi_exception, unterminated_length_message);
   }

   /// Datastream skip is unchecked, so verify every skip against the remaining packed input.
   void require_bytes(size_t size) const {
      SYS_ASSERT(size <= input.remaining(), abi_exception, truncated_length_message);
   }

   /// Fixed charges avoid platform-dependent consensus limits; division prevents arithmetic overflow.
   void charge(size_t count, size_t unit) {
      SYS_ASSERT(count <= storage_left / unit, abi_exception, storage_limit_message);
      storage_left -= count * unit;
   }

   static constexpr size_t vector_slot_bytes = 256;
   static constexpr size_t string_storage_factor = 2;
   static constexpr uint32_t varuint32_bytes = 5;
   static constexpr uint32_t payload_bits = 7;
   static constexpr uint8_t payload_mask = 0x7f;
   static constexpr uint8_t continuation_mask = 0x80;
   static constexpr uint8_t final_byte_overflow_mask = 0xf0;

   fc::datastream<const char*> input;
   size_t storage_left = abi_limits::max_decoded_bytes;
   size_t elements_left = abi_limits::max_elements;
};
} // namespace

void check_abi_bounds(const char* data, size_t size) {
   SYS_ASSERT(size <= abi_limits::max_packed_bytes, abi_exception, packed_size_limit_message);
   if (size)
      abi_bounds_reader(data, size).check<abi_def>();
}

} // namespace sysio::chain
