#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/webassembly/return_codes.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/variant_object.hpp>
#include <fmt/format.h>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>

#include "fork_test_utilities.hpp"
#include "test_utils.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace sysio::chain;
using namespace sysio::testing;
using namespace sysio::chain::webassembly;
using namespace std::literals;

using bytes = std::vector<char>;

bytes h2bin(const std::string& source) {
   bytes output(source.length()/2);
   fc::from_hex(source, output.data(), output.size());
   return output;
}

namespace {

/// Where an output size contract keeps its abort message, its inputs and its output buffer.
constexpr uint32_t message_offset = 8;
constexpr uint32_t input_offset   = 1024;
constexpr uint32_t output_offset  = 8192;

constexpr auto wrong_return_code = "wrong return code";

constexpr auto six_i32_params   = "(param i32 i32 i32 i32 i32 i32) (result i32)";
constexpr auto eight_i32_params = "(param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)";
constexpr auto blake2_f_params  = "(param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)";
constexpr auto blake2_f_rounds  = "(i32.const 0)";
constexpr auto blake2_f_final   = "(i32.const 1)";

/// Hex digits of padding after the 3 bytes of the BLAKE2 message, which is 128 bytes.
constexpr size_t blake2_f_message_zeros = 250;

/// One call to a crypto intrinsic with valid inputs.
struct output_size_case {
   std::string_view         intrinsic;
   std::string_view         signature;
   std::string_view         before_inputs; ///< wast arguments ahead of the inputs
   std::vector<std::string> inputs;        ///< hex, each passed as a pointer and a size
   std::string_view         after_inputs;  ///< wast arguments between the inputs and the output
   std::string              output;        ///< hex of the result, whose size is the only one a call may pass
};

/// The vectors come from the tests below. The mod_exp modulus is wider than its value, so the size asked for is the
/// modulus's and not the result's.
std::vector<output_size_case> output_size_cases() {
   return {
      {"alt_bn128_add", six_i32_params, "",
       {"222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
        "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844"},
       "",
       "16c7c4042e3a725ddbacf197c519c3dcad2bc87dfd9ac7e1e1631154ee0b7d9c19cd640dd28c9811ebaaa095a16b16190d08d6906c4f926fce581985fe35be0e"},
      {"alt_bn128_mul", six_i32_params, "",
       {"007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
        "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b"},
       "",
       "2d66cdeca5e1715896a5a924c50a149be87ddd2347b862150fbb0fd7d0b1833c11c76319ebefc5379f7aa6d85d40169a612597637242a4bbb39e5cd3b844becd"},
      {"mod_exp", eight_i32_params, "", {"03", "02", "00000005"}, "", "00000004"},
      {"blake2_f", blake2_f_params, blake2_f_rounds,
       {"48c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e1319cde05b",
        "616263" + std::string(blake2_f_message_zeros, '0'),
        "0300000000000000", "0000000000000000"},
       blake2_f_final,
       "08c9bcf367e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d282e6ad7f520e511f6c3e2b8c68059b9442be0454267ce079217e1319cde05b"},
      {"k1_recover", six_i32_params, "",
       {"1b174de755b55bd29026d626f7313a5560353dc5175f29c78d79d961b81a0c04360d833ca789bc16d4ee714a6d1a19461d890966e0ec5c074f67be67e631d33aa7",
        "45fd65f6dd062fe7020f11d19fe5c35dc4d425e1479c0968c8e932c208f25399"},
       "",
       "0407521b8289ec7b603bd60b1d7efc5f7ad91cda280a6bebbe6d95d0ac96ef93fb12f99b751dba9238cd35e3c43b44b11474d2a6561afe331ec48c77cd287e438b"},
   };
}

/**
 * A contract that calls the intrinsic with an output buffer of `output_size` bytes, aborts unless the call returns
 * `expected`, and returns the buffer as its action return value, so that a test sees what was written.
 */
std::string wast_with_output_size(const output_size_case& call, uint32_t output_size, int32_t expected) {
   bytes       input;
   std::string arguments{call.before_inputs};
   for (const auto& hex : call.inputs) {
      const bytes part = h2bin(hex);
      arguments += fmt::format(" (i32.const {}) (i32.const {})", input_offset + input.size(), part.size());
      input.insert(input.end(), part.begin(), part.end());
   }
   arguments += ' ';
   arguments += call.after_inputs;
   return fmt::format(R"=====(
(module
 (import "env" "sysio_assert" (func $assert (param i32 i32)))
 (import "env" "set_action_return_value" (func $return_value (param i32 i32)))
 (import "env" "{intrinsic}" (func $f {signature}))
 (memory $0 1)
 (data (i32.const {message}) "{wrong}\00")
 (data (i32.const {input_at}) "{input}")
 (export "apply" (func $apply))
 (func $apply (param $0 i64) (param $1 i64) (param $2 i64)
  (call $assert (i32.eq (call $f {arguments} (i32.const {output}) (i32.const {size})) (i32.const {expected}))
                (i32.const {message}))
  (call $return_value (i32.const {output}) (i32.const {size}))
 )
)
)=====",
                      fmt::arg("intrinsic", call.intrinsic), fmt::arg("signature", call.signature),
                      fmt::arg("message", message_offset), fmt::arg("wrong", wrong_return_code),
                      fmt::arg("input_at", input_offset),
                      fmt::arg("input", sysio::test_utils::to_wast_string(input)), fmt::arg("arguments", arguments),
                      fmt::arg("output", output_offset), fmt::arg("size", output_size), fmt::arg("expected", expected));
}

/// The account the mod_exp tests deploy crypto_primitives_test to.
constexpr account_name modexp_account = "tester1"_n;

/// Byte size of the largest base and modulus mod_exp accepts.
constexpr size_t mod_exp_operand_limit = config::max_mod_exp_operand_size;

/// CPU limits of the transaction modexp_deadline_test expects to be stopped, and how long stopping it may take.
constexpr uint32_t modexp_timeout_ms       = 100;
constexpr uint32_t modexp_block_timeout_ms = 1'000;
constexpr int64_t  modexp_stop_bound_ms    = 5'000;

/// Selects the testmodexp action of the contract on modexp_account for push_trx.
struct testmodexp_call {
   static account_name get_account() { return modexp_account; }
   static action_name  get_name() { return "testmodexp"_n; }
};

/// The bytes of `value` with one zero byte in front: the same big-endian integer, one byte longer.
bytes zero_prefixed(const bytes& value) {
   bytes result{'\0'};
   result.insert(result.end(), value.begin(), value.end());
   return result;
}

/// Push testmodexp, which aborts unless mod_exp returns `expected_error` and writes `expected_result`.
void push_testmodexp(base_tester& c, const bytes& base, const bytes& exp, const bytes& modulus, int32_t expected_error,
                     const bytes& expected_result) {
   c.push_action( modexp_account, "testmodexp"_n, modexp_account, mutable_variant_object()
      ("base", base)
      ("exp", exp)
      ("modulo", modulus)
      ("expected_error", expected_error)
      ("expected_result", expected_result)
   );
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(crypto_primitives_tests)

BOOST_AUTO_TEST_CASE( alt_bn128_add_test ) { try {
   tester c( setup_policy::full);

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using test_add = std::tuple<std::string, std::string, int32_t, std::string>;
   const std::vector<test_add> tests = {
        //test (2 valid points, both on curve)
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844",
            return_code::success,
            "16c7c4042e3a725ddbacf197c519c3dcad2bc87dfd9ac7e1e1631154ee0b7d9c19cd640dd28c9811ebaaa095a16b16190d08d6906c4f926fce581985fe35be0e"
        },

        //test (2 valid points, P1 not on curve)
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "2a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce46498441bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f5332",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test (invalid P1 length)
        {
            "2a",
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //|Fp| = 0x30644e72e131a029b85045b68181585d97816a916871ca8d3c208c16d87cfd47
        //test (P1.x=|Fp|)
        {
            "30644e72e131a029b85045b68181585d97816a916871ca8d3c208c16d87cfd472976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test (P1=(0,0))
        {
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844",
            return_code::success,
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844"
        },

        // test bigger P1 length
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae200",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        // test bigger P2 length
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce464984400",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        // test smaller P2 length
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce46498",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        // test smaller result length
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844",
            return_code::failure,
            "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        // test bigger result length, refused like a smaller one
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae2",
            "1bd20beca3d8d28e536d2b5bd3bf36d76af68af5e6c96ca6e5519ba9ff8f53322a53edf6b48bcf5cb1c0b4ad1d36dfce06a79dcd6526f1c386a14d8ce4649844",
            return_code::failure,
            "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },
   };

   for(const auto& test : tests) {
      auto op1 = h2bin(std::get<0>(test));
      auto op2 = h2bin(std::get<1>(test));
      auto expected_error = std::get<2>(test);
      auto expected_result = h2bin(std::get<3>(test));

      c.push_action( tester1_account, "testadd"_n, tester1_account, mutable_variant_object()
         ("op1", op1)
         ("op2", op2)
         ("expected_error", expected_error)
         ("expected_result", expected_result)
      );
   }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( alt_bn128_mul_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using test_mul = std::tuple<std::string, std::string, int32_t, std::string>;
   const std::vector<test_mul> tests = {
        //test (valid point on curve, scalar size = 256 bits)
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::success,
            "2d66cdeca5e1715896a5a924c50a149be87ddd2347b862150fbb0fd7d0b1833c11c76319ebefc5379f7aa6d85d40169a612597637242a4bbb39e5cd3b844becd"
        },

        //test (scalar size < 256 bits)
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
            "01",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test (P1 not on curve)
        {
            "0db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f9136",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test (invalid P1 length)
        {
            "222480c9f95409bfa4ac6ae890b9c150bc88542b87b352e92950c340458b0c092976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8a",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //|Fp| = 0x30644e72e131a029b85045b68181585d97816a916871ca8d3c208c16d87cfd47
        //test (P1.y=|Fp|)
        {
            "2976efd698cf23b414ea622b3f720dd9080d679042482ff3668cb2e32cad8ae230644e72e131a029b85045b68181585d97816a916871ca8d3c208c16d87cfd47",
            "0100010001000100010001000100010001000100010001000100010001000100",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test (P1=(0,0))
        {
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::success,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test bigger P1 length
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be00",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test bigger scalar length
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b00",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test smaller scalar length
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c",
            return_code::failure,
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test smaller result length
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::failure,
            "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },

        //test bigger result length, refused like a smaller one
        {
            "007c43fcd125b2b13e2521e395a81727710a46b34fe279adbf1b94c72f7f91360db2f980370fb8962751c6ff064f4516a6a93d563388518bb77ab9a6b30755be",
            "0312ed43559cf8ecbab5221256a56e567aac5035308e3f1d54954d8b97cd1c9b",
            return_code::failure,
            "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        },
   };

   for(const auto& test : tests) {
      auto point = h2bin(std::get<0>(test));
      auto scalar = h2bin(std::get<1>(test));
      auto expected_error = std::get<2>(test);
      auto expected_result = h2bin(std::get<3>(test));

      c.push_action( tester1_account, "testmul"_n, tester1_account, mutable_variant_object()
         ("point", point)
         ("scalar", scalar)
         ("expected_error", expected_error)
         ("expected_result", expected_result)
      );
   }


} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( alt_bn128_pair_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using g1g2_pair = std::vector<std::string>;
   using pair_test = std::tuple<std::vector<g1g2_pair>, int32_t>;

   const std::vector<pair_test> tests =
   {
      //test1: 2 pairs => (G1_a,G2_a),(G1_b,G2_b)
      {
         {
               { //G1_a G2_a
                  "0f25929bcb43d5a57391564615c9e70a992b10eafa4db109709649cf48c50dd2", //G1_a.x
                  "16da2f5cb6be7a0aa72c440c53c9bbdfec6c36c7d515536431b3a865468acbba", //G1_a.y
                  "2e89718ad33c8bed92e210e81d1853435399a271913a6520736a4729cf0d51eb", //G2_a.x
                  "01a9e2ffa2e92599b68e44de5bcf354fa2642bd4f26b259daa6f7ce3ed57aeb3",
                  "14a9a87b789a58af499b314e13c3d65bede56c07ea2d418d6874857b70763713", //G2_a.y
                  "178fb49a2d6cd347dc58973ff49613a20757d0fcc22079f9abd10c3baee24590",
               },

               { //G1_b G2_b
                  "1b9e027bd5cfc2cb5db82d4dc9677ac795ec500ecd47deee3b5da006d6d049b8", //G1_b.x
                  "11d7511c78158de484232fc68daf8a45cf217d1c2fae693ff5871e8752d73b21", //G1_b.y
                  "198e9393920d483a7260bfb731fb5d25f1aa493335a9e71297e485b7aef312c2", //G2_b.x
                  "1800deef121f1e76426a00665e5c4479674322d4f75edadd46debd5cd992f6ed",
                  "090689d0585ff075ec9e99ad690c3395bc4b313370b38ef355acdadcd122975b", //G2_b.y
                  "12c85ea5db8c6deb4aab71808dcb408fe3d1e7690c43d37b4ce6cc0166fa7daa",
               }
         },
         0 // With these input pairs, alt_bn128_pair should return 0 indicating a pair result of true.
      },

      //test2: 1 pair => (G1_a,G2_a) G1_a not on curve
      {
         {
               { //G1_a G2_a
                  "16da2f5cb6be7a0aa72c440c53c9bbdfec6c36c7d515536431b3a865468acbba", //G1_a.x
                  "0f25929bcb43d5a57391564615c9e70a992b10eafa4db109709649cf48c50dd2", //G1_a.y
                  "198e9393920d483a7260bfb731fb5d25f1aa493335a9e71297e485b7aef312c2", //G2_b.x
                  "1800deef121f1e76426a00665e5c4479674322d4f75edadd46debd5cd992f6ed",
                  "090689d0585ff075ec9e99ad690c3395bc4b313370b38ef355acdadcd122975b", //G2_b.y
                  "12c85ea5db8c6deb4aab71808dcb408fe3d1e7690c43d37b4ce6cc0166fa7daa"
               },

         },
         return_code::failure
      },

      //test3: 1 pair => (G1_a,G2_a) ; G1_a.x wrong length
      {
         {
               { //G1_a G2_a
                  "000000000000000000000000000000000000000000000000000000000000001",  //G1_a.x
                  "0000000000000000000000000000000000000000000000000000000000000002", //G1_a.y
                  "198e9393920d483a7260bfb731fb5d25f1aa493335a9e71297e485b7aef312c2", //G2_b.x
                  "1800deef121f1e76426a00665e5c4479674322d4f75edadd46debd5cd992f6ed",
                  "090689d0585ff075ec9e99ad690c3395bc4b313370b38ef355acdadcd122975b", //G2_b.y
                  "12c85ea5db8c6deb4aab71808dcb408fe3d1e7690c43d37b4ce6cc0166fa7daa"
               },

         },
         return_code::failure
      },

      //test4: 1 pair => (G1_a,G2_a) ; G1_a=(0,0)
      {
         {
               { //G1_a G2_a
                  "0000000000000000000000000000000000000000000000000000000000000000", //G1_a.x
                  "0000000000000000000000000000000000000000000000000000000000000000", //G1_a.y
                  "198e9393920d483a7260bfb731fb5d25f1aa493335a9e71297e485b7aef312c2", //G2_a.x
                  "1800deef121f1e76426a00665e5c4479674322d4f75edadd46debd5cd992f6ed",
                  "090689d0585ff075ec9e99ad690c3395bc4b313370b38ef355acdadcd122975b", //G2_a.y
                  "12c85ea5db8c6deb4aab71808dcb408fe3d1e7690c43d37b4ce6cc0166fa7daa"
               },

         },
         0 // With these input pairs, alt_bn128_pair should return 0 indicating a pair result of true.
      },

      //test5: 1 pair => (G1_a,G2_a) ; G1_a.x == |Fp|
      {
         {
               { //G1_a G2_a
                  "30644e72e131a029b85045b68181585d97816a916871ca8d3c208c16d87cfd47", //G1_a.x
                  "0000000000000000000000000000000100000000000000000000000000000000", //G1_a.y
                  "198e9393920d483a7260bfb731fb5d25f1aa493335a9e71297e485b7aef312c2", //G2_b.x
                  "1800deef121f1e76426a00665e5c4479674322d4f75edadd46debd5cd992f6ed",
                  "090689d0585ff075ec9e99ad690c3395bc4b313370b38ef355acdadcd122975b", //G2_b.y
                  "12c85ea5db8c6deb4aab71808dcb408fe3d1e7690c43d37b4ce6cc0166fa7daa"
               },

         },
         return_code::failure
      }
   };

   auto concat = [&](const std::string& s, bytes& buffer) {
      auto res = h2bin(s);
      buffer.insert( buffer.end(), res.begin(), res.end());
   };

   for(const auto& test : tests) {
      const auto& pairs           = std::get<0>(test);
      const auto& expected_error  = std::get<1>(test);

      bytes g1_g2_pairs;
      for(const auto& pair : pairs) {
         BOOST_REQUIRE(pair.size() == 6);
         concat(pair[0], g1_g2_pairs);
         concat(pair[1], g1_g2_pairs);
         concat(pair[2], g1_g2_pairs);
         concat(pair[3], g1_g2_pairs);
         concat(pair[4], g1_g2_pairs);
         concat(pair[5], g1_g2_pairs);
      }

      c.push_action( tester1_account, "testpair"_n, tester1_account, mutable_variant_object()
         ("g1_g2_pairs", g1_g2_pairs)
         ("expected_error", expected_error)
      );
   }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( modexp_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using modexp_test = std::tuple<std::vector<string>, int32_t, std::string>;
   const std::vector<modexp_test> tests {
      //test1
      {
         {
               "03",
               "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e",
               "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f",
         },
         return_code::success,
         "0000000000000000000000000000000000000000000000000000000000000001",
      },

      //test2
      {
         {
               "",
               "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2e",
               "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f",
         },
         return_code::success,
         "0000000000000000000000000000000000000000000000000000000000000000",
      },

      //test3
      {
         {
               "01",
               "ff",
               "",
         },
         return_code::failure,
         "",
      },

   };

   for(const auto& test : tests) {
      const auto& parts           = std::get<0>(test);
      const auto& expected_error  = std::get<1>(test);
      const auto& expected_result = std::get<2>(test);

      auto base = h2bin(parts[0]);
      auto exponent = h2bin(parts[1]);
      auto modulus = h2bin(parts[2]);

      c.push_action( tester1_account, "testmodexp"_n, tester1_account, mutable_variant_object()
         ("base", base)
         ("exp", exponent)
         ("modulo", modulus)
         ("expected_error", expected_error)
         ("expected_result", expected_result)
      );
   }

} FC_LOG_AND_RETHROW() }


/// A producing node and a validating node applying its block return the same codes at and past the operand size limit;
/// a zero byte in front of an operand at the limit fails although its value is unchanged.
BOOST_AUTO_TEST_CASE( modexp_operand_size_limit_test ) { try {
   validating_tester c;

   c.create_accounts( {modexp_account} );
   c.produce_block();
   c.set_code( modexp_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( modexp_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   // Operands that fill every byte: modulus 2^(8 * (limit - 1)) and base modulus + 5, so the result is 5^3.
   bytes modulus(mod_exp_operand_limit, '\0');
   modulus.front() = 0x01;
   bytes base = modulus;
   base.back() = 0x05;
   const bytes exponent = h2bin("03");
   bytes at_limit_result(mod_exp_operand_limit, '\0');
   at_limit_result.back() = 0x7d;

   const bytes padded_base    = zero_prefixed(base);
   const bytes padded_modulus = zero_prefixed(modulus);

   push_testmodexp(c, base, exponent, modulus, return_code::success, at_limit_result);
   push_testmodexp(c, padded_base, exponent, modulus, return_code::failure, bytes(modulus.size(), '\0'));
   push_testmodexp(c, base, exponent, padded_modulus, return_code::failure, bytes(padded_modulus.size(), '\0'));
   push_testmodexp(c, padded_base, exponent, padded_modulus, return_code::failure,
                   bytes(padded_modulus.size(), '\0'));
   push_testmodexp(c, padded_base, exponent, h2bin("0f"), return_code::failure, h2bin("00"));

   c.produce_block();
   BOOST_CHECK_EQUAL( c.validating_node->head().id(), c.control->head().id() );

} FC_LOG_AND_RETHROW() }

/// The exponent may be longer than the base and the modulus, and moduli of any size up to the limit are computed.
BOOST_AUTO_TEST_CASE( modexp_long_exponent_and_large_modulus_test ) { try {
   validating_tester c;

   c.create_accounts( {modexp_account} );
   c.produce_block();
   c.set_code( modexp_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( modexp_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   const bytes exponent = h2bin("010001");
   push_testmodexp(c, h2bin("01"), exponent, h2bin("0f"), return_code::success, h2bin("01"));

   // 2^4 = 1 mod 15, so 2^e = 2^(e mod 4) mod 15; e = 2^512 - 1 is 3 mod 4.
   push_testmodexp(c, h2bin("02"), bytes(64, '\xff'), h2bin("0f"), return_code::success, h2bin("08"));

   for (const size_t size : {4096u, 4097u, 8191u}) {
      bytes modulus(size - 1, '\0');
      modulus.push_back(0x0f);
      bytes expected_result(size - 1, '\0');
      expected_result.push_back(0x01);
      push_testmodexp(c, h2bin("01"), exponent, modulus, return_code::success, expected_result);
   }

   c.produce_block();
   BOOST_CHECK_EQUAL( c.validating_node->head().id(), c.control->head().id() );

} FC_LOG_AND_RETHROW() }

/// An exponent within the operand limits that would run far past the transaction deadline is stopped by it.
BOOST_AUTO_TEST_CASE( modexp_deadline_test ) { try {
   tester c( setup_policy::full );

   c.create_accounts( {modexp_account} );
   c.produce_block();
   c.set_code( modexp_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( modexp_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   // Load the contract before the timed transaction.
   push_testmodexp(c, h2bin("03"), h2bin("02"), h2bin("0f"), return_code::success, h2bin("09"));

   const bytes modulus(mod_exp_operand_limit, '\xff');
   const bytes base(mod_exp_operand_limit, '\x5a');
   const bytes exponent(2 * mod_exp_operand_limit, '\xa5');
   const bytes payload = fc::raw::pack(std::make_tuple(base, exponent, modulus, int32_t{return_code::success},
                                                       bytes(modulus.size(), '\0')));

   const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
   BOOST_CHECK_EXCEPTION( sysio::test_utils::push_trx(c, testmodexp_call{}, 0, modexp_timeout_ms,
                                                      modexp_block_timeout_ms, false, payload, modexp_account),
                          tx_cpu_usage_exceeded, sysio::test_utils::is_tx_cpu_usage_exceeded );
   const int64_t elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
   BOOST_CHECK_LT( elapsed_ms, modexp_stop_bound_ms );

   push_testmodexp(c, h2bin("02"), h2bin("03"), h2bin("0f"), return_code::success, h2bin("08"));
   c.produce_block();

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( blake2f_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using compress_test = std::tuple<std::vector<string>, int32_t, std::string>;
   const std::vector<compress_test> tests {
      //test1
      {
         {
               "00000000",
               "48c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e1319cde05b6",
               "61626300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
               "03000000000000000",
               "00000000000000000",
               "01",
         },
         return_code::success,
         "08c9bcf367e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d282e6ad7f520e511f6c3e2b8c68059b9442be0454267ce079217e1319cde05b",
      },

      //test2
      {
         {
               "0000000c",
               "48c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e1319cde05b6",
               "61626300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
               "03000000000000000",
               "00000000000000000",
               "01",
         },
         return_code::success,
         "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"
      },

      //test3
      {
         {
               "0000000c",
               "48c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e1319cde05b6",
               "61626300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
               "03000000000000000",
               "00000000000000000",
               "00",
         },
         return_code::success,
         "75ab69d3190a562c51aef8d88f1c2775876944407270c42c9844252c26d2875298743e7f6d5ea2f2d3e8d226039cd31b4e426ac4f2d3d666a610c2116fde4735"
      },

      //test4
      {
         {
               "00000001",
               "48c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e1319cde05b6",
               "61626300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
               "03000000000000000",
               "00000000000000000",
               "01",
         },
         return_code::success,
         "b63a380cb2897d521994a85234ee2c181b5f844d2c624c002677e9703449d2fba551b3a8333bcdf5f2f7e08993d53923de3d64fcc68c034e717b9293fed7a421"
      },

      //test5
      {
         {
               "00000000",
               "c9bdf267e6096a3ba7ca8485ae67bb2bf894fe72f36e3cf1361d5f3af54fa5d182e6ad7f520e511f6c3e2b8c68059b6bbd41fbabd9831f79217e1319cde05b6",
               "61626300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
               "03000000000000000",
               "00000000000000000",
               "01",
         },
         return_code::failure,
         "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      }
   };

   auto to_uint32 = [](const std::string& s) -> uint32_t{
      size_t l = s.size();
      return (uint32_t)std::stoul(s.c_str(), &l, 16);
   };

   for(const auto& test : tests) {

      const auto& params          = std::get<0>(test);
      const auto& expected_error  = std::get<1>(test);
      const auto& expected_result = std::get<2>(test);

      BOOST_REQUIRE(params.size() == 6);

      uint32_t rounds  = to_uint32(params[0] );
      bytes    state   = h2bin( params[1] );
      bytes    message = h2bin( params[2] );
      bytes    t0      = h2bin( params[3] );
      bytes    t1      = h2bin( params[4] );
      bool     final   = params[5] == "00" ? false : true;

      c.push_action( tester1_account, "testblake2f"_n, tester1_account, mutable_variant_object()
         ("rounds", rounds)
         ("state", state)
         ("message", message)
         ("t0", t0)
         ("t1", t1)
         ("final", final)
         ("expected_error", expected_error)
         ("expected_result", expected_result)
      );

   }

} FC_LOG_AND_RETHROW() }


BOOST_AUTO_TEST_CASE( keccak256_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using test_keccak256 = std::tuple<std::string, std::string>;
   const std::vector<test_keccak256> tests {
      //test
      {
         "",
         "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
      },

      //test
      {
         "abc",
         "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
      },

      //test
      {
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "45d3b367a6904e6e8d502ee04999a7c27647f91fa845d456525fd352ae3d7371",
      }
   };

   for(const auto& test : tests) {
      auto tmp = std::get<0>(test);
      auto input = bytes{tmp.data(), tmp.data()+tmp.size()};
      auto expected_result = h2bin(std::get<1>(test));
      c.push_action( tester1_account, "testkeccak"_n, tester1_account, mutable_variant_object()
         ("input", input)
         ("expected_result", expected_result)
      );
   }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( sha3_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using test_sha3 = std::tuple<std::string, std::string>;
   const std::vector<test_sha3> tests {
      //test
      {
         "",
         "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
      },

      //test
      {
         "abc",
         "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
      },

      //test
      {
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376",
      }
   };

   for(const auto& test : tests) {
      auto tmp = std::get<0>(test);
      auto input = bytes{tmp.data(), tmp.data()+tmp.size()};
      auto expected_result = h2bin(std::get<1>(test));
      c.push_action( tester1_account, "testsha3"_n, tester1_account, mutable_variant_object()
         ("input", input)
         ("expected_result", expected_result)
      );
   }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( k1_recover_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_code( tester1_account, test_contracts::crypto_primitives_test_wasm() );
   c.set_abi( tester1_account, test_contracts::crypto_primitives_test_abi() );
   c.produce_block();

   using test_k1_recover = std::tuple<std::string, std::string, int32_t, std::string>;
   const std::vector<test_k1_recover> tests {
      //test
      {
         "1b174de755b55bd29026d626f7313a5560353dc5175f29c78d79d961b81a0c04360d833ca789bc16d4ee714a6d1a19461d890966e0ec5c074f67be67e631d33aa7",
         "45fd65f6dd062fe7020f11d19fe5c35dc4d425e1479c0968c8e932c208f25399",
         return_code::success,
         "0407521b8289ec7b603bd60b1d7efc5f7ad91cda280a6bebbe6d95d0ac96ef93fb12f99b751dba9238cd35e3c43b44b11474d2a6561afe331ec48c77cd287e438b",
      },

      //test (invalid signature v)
      {
         "01174de755b55bd29026d626f7313a5560353dc5175f29c78d79d961b81a0c04360d833ca789bc16d4ee714a6d1a19461d890966e0ec5c074f67be67e631d33aa7",
         "45fd65f6dd062fe7020f11d19fe5c35dc4d425e1479c0968c8e932c208f25399",
         return_code::failure,
         "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
      },

      //test (invalid signature len)
      {
         "174de755b55bd29026d626f7313a5560353dc5175f29c78d79d961b81a0c04360d833ca789bc16d4ee714a6d1a19461d890966e0ec5c074f67be67e631d33aa7",
         "45fd65f6dd062fe7020f11d19fe5c35dc4d425e1479c0968c8e932c208f25399",
         return_code::failure,
         "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
      },

      //test (invalid digest len)
      {
         "00174de755b55bd29026d626f7313a5560353dc5175f29c78d79d961b81a0c04360d833ca789bc16d4ee714a6d1a19461d890966e0ec5c074f67be67e631d33aa7",
         "fd65f6dd062fe7020f11d19fe5c35dc4d425e1479c0968c8e932c208f25399",
         return_code::failure,
         "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
      },

   };

   for(const auto& test : tests) {
      const auto& signature       = h2bin(std::get<0>(test));
      const auto& digest          = h2bin(std::get<1>(test));
      const auto& expected_error  = std::get<2>(test);
      const auto& expected_result = h2bin(std::get<3>(test));

      c.push_action( tester1_account, "testecrec"_n, tester1_account, mutable_variant_object()
         ("signature", signature)
         ("digest", digest)
         ("expected_error", expected_error)
         ("expected_result", expected_result)
      );
   }

} FC_LOG_AND_RETHROW() }

// A buffer of any size but the result's fails the call, and nothing is written to it. A larger buffer is refused too, so
// that no contract comes to rely on one. These contracts are hand-written wast so that they can pass any size.
BOOST_AUTO_TEST_CASE( output_size_is_exact_test ) { try {
   tester c( setup_policy::full );

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   const auto buffer_after_call = [&]( const output_size_case& call, uint32_t output_size,
                                       int32_t expected ) -> std::string {
      c.set_code( tester1_account, wast_with_output_size(call, output_size, expected).c_str() );
      c.produce_block();

      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{tester1_account, config::active_name}}, tester1_account, ""_n, bytes{} );
      c.set_transaction_headers( trx );
      trx.sign( c.get_private_key(tester1_account, "active"), c.get_chain_id() );
      try {
         const auto trace = c.push_transaction( trx );
         c.produce_block();
         return fc::to_hex( trace->action_traces.front().return_value );
      } catch( const sysio_assert_message_exception& ) {
         return wrong_return_code;
      }
   };
   const auto zeros = []( size_t size ) { return std::string(size * 2, '0'); };

   for( const auto& call : output_size_cases() ) {
      BOOST_TEST_CONTEXT( call.intrinsic ) {
         const size_t exact = call.output.size() / 2;
         BOOST_TEST( buffer_after_call(call, exact - 1, return_code::failure) == zeros(exact - 1) );
         BOOST_TEST( buffer_after_call(call, exact, return_code::success) == call.output );
         BOOST_TEST( buffer_after_call(call, exact + 1, return_code::failure) == zeros(exact + 1) );
      }
   }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
