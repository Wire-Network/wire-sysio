#include <boost/test/unit_test.hpp>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/exception/exception.hpp>
#include <fc/utility.hpp>

#include <random>
#include <string>
#include <vector>

namespace {

/// Convert a lowercase hex string to a byte vector. Used to keep Bitcoin
/// Core's published test vector format readable in source.
std::vector<char> hex_to_bytes(const std::string& hex) {
   std::vector<char> out(hex.size() / 2);
   fc::from_hex(hex, out.data(), out.size());
   return out;
}

/// Bitcoin Core's standard base58 test vectors, taken verbatim from
/// src/test/data/base58_encode_decode.json. Passing these gives us
/// bit-identical encoding to every Bitcoin-derived implementation.
const std::vector<std::pair<std::string, std::string>> k_bitcoin_vectors = {
   {"",                                                       ""},
   {"61",                                                     "2g"},
   {"626262",                                                 "a3gV"},
   {"636363",                                                 "aPEr"},
   {"73696d706c792061206c6f6e6720737472696e67",               "2cFupjhnEsSn59qHXstmK2ffpLv2"},
   {"00eb15231dfceb60925886b67d065299925915aeb172c06647",     "1NS17iag9jJgTHD1VXjvLCEnZuQ3rJDE9L"},
   {"516b6fcd0f",                                             "ABnLTmg"},
   {"bf4f89001e670274dd",                                     "3SEo3LWLoPntC"},
   {"572e4794",                                               "3EFU7m"},
   {"ecac89cad93923c02321",                                   "EJDM8drfXA6uyA"},
   {"10c8511e",                                               "Rt5zm"},
   {"00000000000000000000",                                   "1111111111"},
};

} // namespace

BOOST_AUTO_TEST_SUITE(base58_tests)

BOOST_AUTO_TEST_CASE(empty_input_encodes_to_empty_string) {
   BOOST_CHECK_EQUAL(fc::to_base58(nullptr, 0, fc::yield_function_t{}), "");
   BOOST_CHECK_EQUAL(fc::to_base58(std::vector<char>{}, fc::yield_function_t{}), "");
}

BOOST_AUTO_TEST_CASE(empty_string_decodes_to_empty_vector) {
   auto out = fc::from_base58("");
   BOOST_CHECK(out.empty());
}

BOOST_AUTO_TEST_CASE(leading_zero_bytes_encode_as_leading_ones) {
   // Single zero byte -> "1"
   {
      const char one_zero[1] = {0};
      BOOST_CHECK_EQUAL(fc::to_base58(one_zero, 1, fc::yield_function_t{}), "1");
   }
   // Four zero bytes -> "1111"
   {
      const char four_zeros[4] = {0, 0, 0, 0};
      BOOST_CHECK_EQUAL(fc::to_base58(four_zeros, 4, fc::yield_function_t{}), "1111");
   }
   // 32 zero bytes -> 32 '1's (typical pubkey-shaped input)
   {
      std::vector<char> thirty_two_zeros(32, 0);
      BOOST_CHECK_EQUAL(fc::to_base58(thirty_two_zeros, fc::yield_function_t{}), std::string(32, '1'));
   }
}

BOOST_AUTO_TEST_CASE(leading_ones_decode_to_leading_zero_bytes) {
   auto out = fc::from_base58("1111111111");
   BOOST_CHECK_EQUAL(out.size(), 10u);
   for (char b : out)
      BOOST_CHECK_EQUAL(b, 0);
}

BOOST_AUTO_TEST_CASE(bitcoin_test_vectors_match) {
   for (const auto& [hex_in, b58_expected] : k_bitcoin_vectors) {
      const auto bytes = hex_to_bytes(hex_in);
      const auto encoded = fc::to_base58(bytes.data(), bytes.size(), fc::yield_function_t{});
      BOOST_CHECK_EQUAL(encoded, b58_expected);

      const auto decoded = fc::from_base58(b58_expected);
      BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(),
                                    bytes.begin(), bytes.end());
   }
}

BOOST_AUTO_TEST_CASE(roundtrip_random_signature_sized_inputs) {
   // 65 bytes mirrors the secp256k1/r1 compact-signature size that
   // dominates trace_api/get_block allocation profiles.
   std::mt19937_64 rng(0xC0FFEEULL);
   std::uniform_int_distribution<int> byte_dist(0, 255);
   for (int trial = 0; trial < 32; ++trial) {
      std::vector<char> in(65);
      for (auto& b : in)
         b = static_cast<char>(byte_dist(rng));
      const auto encoded = fc::to_base58(in, fc::yield_function_t{});
      const auto decoded = fc::from_base58(encoded);
      BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(),
                                    in.begin(), in.end());
   }
}

BOOST_AUTO_TEST_CASE(roundtrip_random_public_key_sized_inputs) {
   std::mt19937_64 rng(0xBADCAFEULL);
   std::uniform_int_distribution<int> byte_dist(0, 255);
   for (int trial = 0; trial < 32; ++trial) {
      std::vector<char> in(33);
      for (auto& b : in)
         b = static_cast<char>(byte_dist(rng));
      const auto encoded = fc::to_base58(in, fc::yield_function_t{});
      const auto decoded = fc::from_base58(encoded);
      BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(),
                                    in.begin(), in.end());
   }
}

BOOST_AUTO_TEST_CASE(roundtrip_random_private_key_sized_inputs) {
   std::mt19937_64 rng(0xDEADBEEFULL);
   std::uniform_int_distribution<int> byte_dist(0, 255);
   for (int trial = 0; trial < 32; ++trial) {
      std::vector<char> in(32);
      for (auto& b : in)
         b = static_cast<char>(byte_dist(rng));
      const auto encoded = fc::to_base58(in, fc::yield_function_t{});
      const auto decoded = fc::from_base58(encoded);
      BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(),
                                    in.begin(), in.end());
   }
}

BOOST_AUTO_TEST_CASE(invalid_characters_throw) {
   // 0, O, I, l, and various punctuation are intentionally absent from
   // the base58 alphabet to avoid visually-similar glyphs. Each must
   // be rejected.
   BOOST_CHECK_THROW(fc::from_base58("0OIl"), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::from_base58("hello!"), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::from_base58("ab cd"), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(leading_and_trailing_whitespace_tolerated) {
   // Old BIGNUM implementation skipped leading and trailing whitespace
   // around an otherwise valid base58 string; preserve that behavior so
   // existing callers parsing keys with stray whitespace keep working.
   BOOST_CHECK_NO_THROW(fc::from_base58("   2g"));
   BOOST_CHECK_NO_THROW(fc::from_base58("2g   "));
   const auto a = fc::from_base58("2g");
   const auto b = fc::from_base58("   2g   ");
   BOOST_CHECK_EQUAL_COLLECTIONS(a.begin(), a.end(), b.begin(), b.end());
}

BOOST_AUTO_TEST_CASE(from_base58_into_fixed_buffer_matches_vector_overload) {
   // The two from_base58 overloads share an underlying decode; verify
   // the fixed-buffer variant writes the same bytes and reports the
   // correct length.
   const std::string b58 = "2cFupjhnEsSn59qHXstmK2ffpLv2";
   const auto expected = fc::from_base58(b58);
   std::vector<char> buf(expected.size() + 8, char{0x55});
   const size_t written = fc::from_base58(b58, buf.data(), buf.size());
   BOOST_CHECK_EQUAL(written, expected.size());
   BOOST_CHECK_EQUAL_COLLECTIONS(buf.begin(), buf.begin() + written,
                                 expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(from_base58_into_undersized_buffer_throws) {
   const std::string b58 = "2cFupjhnEsSn59qHXstmK2ffpLv2";
   const auto expected = fc::from_base58(b58);
   std::vector<char> buf(expected.size() - 1);
   BOOST_CHECK_THROW(fc::from_base58(b58, buf.data(), buf.size()), fc::assert_exception);
}

BOOST_AUTO_TEST_CASE(to_base58_invokes_yield_even_for_small_inputs) {
   // Deadline-bound callers (e.g. abi_serializer) pass a yield that throws
   // once a deadline expires, and rely on to_base58 invoking it so an
   // already-expired deadline is observed. The in-loop yield only fires
   // every 64 input bytes and sits after the leading-zero skip, so key-,
   // signature-, empty-, and all-zero-sized inputs must be covered by the
   // unconditional entry/exit yields.
   const auto count_yields = [](const std::vector<char>& in) {
      int calls = 0;
      fc::to_base58(in.data(), in.size(), fc::yield_function_t{[&calls]() { ++calls; }});
      return calls;
   };

   // 32-byte private key: well under the 64-byte in-loop cadence.
   BOOST_CHECK_GT(count_yields(std::vector<char>(32, char{0x11})), 0);
   // 65-byte signature: still under two full cadence intervals.
   BOOST_CHECK_GT(count_yields(std::vector<char>(65, char{0x22})), 0);
   // Empty input: the conversion loop never runs.
   BOOST_CHECK_GT(count_yields(std::vector<char>{}), 0);
   // All-zero input: every byte is skipped before the loop, so the
   // in-loop yield is unreachable.
   BOOST_CHECK_GT(count_yields(std::vector<char>(40, char{0})), 0);

   // A large non-zero input must additionally hit the periodic in-loop
   // yield, so it observes a deadline that expires mid-encode rather than
   // only at entry/exit.
   const int small_calls = count_yields(std::vector<char>(32, char{0x11}));
   const int large_calls = count_yields(std::vector<char>(256, char{0x33}));
   BOOST_CHECK_GT(large_calls, small_calls);
}

BOOST_AUTO_TEST_SUITE_END()
