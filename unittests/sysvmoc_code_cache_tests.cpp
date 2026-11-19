#include <sysio/chain/webassembly/sys-vm-oc/code_cache_header.hpp>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstring>
#include <limits>

using namespace sysio::chain::sysvmoc;

namespace {

constexpr auto largest_byte = std::numeric_limits<uint8_t>::max();
constexpr uintptr_t descriptor_offset = 1024;
constexpr uint8_t clean_flag = 0;
constexpr uint8_t dirty_flag = 1;

} // namespace

BOOST_AUTO_TEST_SUITE(sysvmoc_code_cache_tests)

/// Decode both flag values from unaligned storage without changing the packed native layout or bytes.
BOOST_AUTO_TEST_CASE(header_round_trip) {
   static_assert(offsetof(code_cache_header, dirty) == sizeof(uint64_t));
   static_assert(offsetof(code_cache_header, serialized_descriptor_index) == sizeof(uint64_t) + sizeof(bool));
   static_assert(sizeof(code_cache_header) == sizeof(uint64_t) + sizeof(bool) + sizeof(uintptr_t));
   for (const bool dirty : {false, true}) {
      code_cache_header original;
      original.dirty = dirty;
      original.serialized_descriptor_index = descriptor_offset;
      alignas(uint64_t) std::array<char, sizeof(original) + 1> bytes{};
      std::memcpy(bytes.data() + 1, &original, sizeof(original));
      const auto decoded = read_code_cache_header({bytes.data() + 1, sizeof(original)});
      BOOST_CHECK(decoded.id == original.id);
      BOOST_CHECK_EQUAL(decoded.dirty, dirty);
      BOOST_CHECK(decoded.serialized_descriptor_index == descriptor_offset);
      BOOST_CHECK_EQUAL(std::memcmp(&decoded, &original, sizeof(original)), 0);
   }
}

/// Reject every incomplete native header before forming a pointer to its dirty flag.
BOOST_AUTO_TEST_CASE(truncated_headers) {
   code_cache_header original;
   std::array<char, sizeof(original)> bytes{};
   std::memcpy(bytes.data(), &original, sizeof(original));
   for (size_t size = 0; size < bytes.size(); ++size)
      BOOST_CHECK_THROW(read_code_cache_header({bytes.data(), size}), fc::exception);
}

/// Accept exactly the two canonical flag bytes and reject all other byte patterns through an fc exception.
BOOST_AUTO_TEST_CASE(all_flag_representations) {
   code_cache_header original;
   std::array<char, sizeof(original)> bytes{};
   std::memcpy(bytes.data(), &original, sizeof(original));
   for (unsigned value = 0; value <= largest_byte; ++value) {
      const auto encoded = static_cast<uint8_t>(value);
      std::memcpy(bytes.data() + offsetof(code_cache_header, dirty), &encoded, sizeof(encoded));
      if (encoded == clean_flag || encoded == dirty_flag) {
         const auto decoded = read_code_cache_header(bytes);
         BOOST_CHECK_EQUAL(decoded.dirty, encoded == dirty_flag);
         BOOST_CHECK(decoded.id == original.id);
         BOOST_CHECK(decoded.serialized_descriptor_index == original.serialized_descriptor_index);
      } else {
         BOOST_CHECK_THROW(read_code_cache_header(bytes), fc::exception);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
