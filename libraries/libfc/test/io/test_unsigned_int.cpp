#include <fc/exception/exception.hpp>
#include <fc/io/raw.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

using namespace fc;

namespace {

/// Unpacks an unsigned_int from @p bytes and reports how many bytes were read.
uint32_t unpack_unsigned_int(const std::vector<char>& bytes, size_t& consumed) {
   datastream<const char*> ds(bytes.data(), bytes.size());
   unsigned_int            vi;
   fc::raw::unpack(ds, vi);
   consumed = ds.tellp();
   return vi.value;
}

/// Packs @p v as an unsigned_int.
std::vector<char> pack_unsigned_int(uint32_t v) {
   std::vector<char> buf(fc::raw::pack_size(unsigned_int(v)));
   datastream<char*> ds(buf.data(), buf.size());
   fc::raw::pack(ds, unsigned_int(v));
   return buf;
}

} // namespace

BOOST_AUTO_TEST_SUITE(unsigned_int_test_suite)

/// Every packed value fits in five bytes and unpacks to itself, reading every byte.
BOOST_AUTO_TEST_CASE(round_trip) {
   for (uint32_t v : {0u, 1u, 127u, 128u, 300u, 16383u, 16384u, 0x0FFFFFFFu, 0x10000000u, 0xFFFFFFFFu}) {
      std::vector<char> buf = pack_unsigned_int(v);
      BOOST_TEST(buf.size() <= 5u);
      size_t consumed = 0;
      BOOST_TEST(unpack_unsigned_int(buf, consumed) == v);
      BOOST_TEST(consumed == buf.size());
   }
}

/// A fifth byte with the continuation bit set is rejected.
BOOST_AUTO_TEST_CASE(reject_continuation_on_fifth_byte) {
   const std::vector<char> six_bytes{char(0x80), char(0x80), char(0x80), char(0x80), char(0x80), char(0x01)};
   size_t                  consumed = 0;
   BOOST_CHECK_THROW(unpack_unsigned_int(six_bytes, consumed), fc::assert_exception);

   const std::vector<char> five_bytes{char(0x80), char(0x80), char(0x80), char(0x80), char(0x80)};
   BOOST_CHECK_THROW(unpack_unsigned_int(five_bytes, consumed), fc::assert_exception);
}

/// Encodings of up to five bytes that are longer than needed still unpack, as before.
BOOST_AUTO_TEST_CASE(accept_padded_within_five_bytes) {
   const std::vector<char> padded_zero{char(0x80), char(0x80), char(0x80), char(0x80), char(0x00)};
   size_t                  consumed = 0;
   BOOST_TEST(unpack_unsigned_int(padded_zero, consumed) == 0u);
   BOOST_TEST(consumed == 5u);
}

/// Input that ends before the last byte of an unsigned_int is rejected.
BOOST_AUTO_TEST_CASE(reject_truncated) {
   const std::vector<char> truncated{char(0x80), char(0x80)};
   size_t                  consumed = 0;
   BOOST_CHECK_THROW(unpack_unsigned_int(truncated, consumed), fc::exception);
}

BOOST_AUTO_TEST_SUITE_END()
