#include <fc/exception/exception.hpp>
#include <fc/io/raw.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace fc;

namespace {

/// Unpacks a signed_int from @p bytes and reports how many bytes were read.
int32_t unpack_signed_int(const std::vector<char>& bytes, size_t& consumed) {
   datastream<const char*> ds(bytes.data(), bytes.size());
   signed_int              vi;
   fc::raw::unpack(ds, vi);
   consumed = ds.tellp();
   return vi.value;
}

/// Packs @p v as a signed_int.
std::vector<char> pack_signed_int(int32_t v) {
   std::vector<char> buf(fc::raw::pack_size(signed_int(v)));
   datastream<char*> ds(buf.data(), buf.size());
   fc::raw::pack(ds, signed_int(v));
   return buf;
}

} // namespace

BOOST_AUTO_TEST_SUITE(signed_int_test_suite)

/// Every packed value fits in five bytes and unpacks to itself, reading every byte.
BOOST_AUTO_TEST_CASE(round_trip) {
   for (int32_t v : {0, 1, -1, 63, -64, 64, -65, 8191, -8192, 1 << 27, -(1 << 27),
                     std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min()}) {
      std::vector<char> buf = pack_signed_int(v);
      BOOST_TEST(buf.size() <= 5u);
      size_t consumed = 0;
      BOOST_TEST(unpack_signed_int(buf, consumed) == v);
      BOOST_TEST(consumed == buf.size());
   }
}

/// A fifth byte with the continuation bit set is rejected.
BOOST_AUTO_TEST_CASE(reject_continuation_on_fifth_byte) {
   const std::vector<char> six_bytes{char(0x80), char(0x80), char(0x80), char(0x80), char(0x80), char(0x01)};
   size_t                  consumed = 0;
   BOOST_CHECK_THROW(unpack_signed_int(six_bytes, consumed), fc::assert_exception);

   const std::vector<char> five_bytes{char(0x80), char(0x80), char(0x80), char(0x80), char(0x80)};
   BOOST_CHECK_THROW(unpack_signed_int(five_bytes, consumed), fc::assert_exception);

   const std::vector<char> long_run(64, char(0xff));
   BOOST_CHECK_THROW(unpack_signed_int(long_run, consumed), fc::assert_exception);
}

/// Encodings of up to five bytes that are longer than needed still unpack, as before.
BOOST_AUTO_TEST_CASE(accept_padded_within_five_bytes) {
   const std::vector<char> padded_zero{char(0x80), char(0x80), char(0x80), char(0x80), char(0x00)};
   size_t                  consumed = 0;
   BOOST_TEST(unpack_signed_int(padded_zero, consumed) == 0);
   BOOST_TEST(consumed == 5u);

   const std::vector<char> padded_minus_one{char(0x81), char(0x80), char(0x80), char(0x80), char(0x00)};
   BOOST_TEST(unpack_signed_int(padded_minus_one, consumed) == -1);
   BOOST_TEST(consumed == 5u);
}

/// Input that ends before the last byte of a signed_int is rejected.
BOOST_AUTO_TEST_CASE(reject_truncated) {
   const std::vector<char> truncated{char(0x80), char(0x80)};
   size_t                  consumed = 0;
   BOOST_CHECK_THROW(unpack_signed_int(truncated, consumed), fc::exception);
}

BOOST_AUTO_TEST_SUITE_END()
