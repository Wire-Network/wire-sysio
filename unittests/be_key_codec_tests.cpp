#include <boost/test/unit_test.hpp>

#include <sysio/chain/database_utils.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>

using namespace sysio::chain;
namespace codec = sysio::chain::be_key_codec;

/**
 * Direct coverage for the ABI-aware BE key codec: typedef resolution, struct
 * key expansion (the `slug_name` shape keying the v6 registry tables),
 * abigen template-spelling canonicalization, the float128 leaf, and the
 * shape builder's rejection paths. End-to-end bound/pagination behaviour is
 * covered in tests/get_table_tests.cpp; these pin the codec layer itself.
 */

namespace {

/// ABI fixture: a slug_name-style struct key, a two-hop typedef chain onto
/// it, a nested struct key, and a based struct (rejected by design).
abi_def make_test_abi() {
   abi_def abi;
   abi.types.emplace_back(type_def{"chain_code_t", "code_alias"});
   abi.types.emplace_back(type_def{"code_alias", "slug_name"});
   abi.structs.emplace_back(struct_def{"slug_name", "", {field_def{"value", "uint64"}}});
   abi.structs.emplace_back(
      struct_def{"pair_key", "", {field_def{"code", "slug_name"}, field_def{"idx", "uint32"}}});
   abi.structs.emplace_back(struct_def{"based_key", "slug_name", {field_def{"extra", "uint64"}}});
   return abi;
}

fc::variant slug(uint64_t v) {
   return fc::variant(fc::mutable_variant_object("value", v));
}

std::vector<char> encode_single(const abi_def& abi, const std::string& type, const fc::variant& val) {
   auto shapes = codec::build_key_shapes(abi, {"k"}, {type});
   return codec::encode_key(fc::variant(fc::mutable_variant_object("k", val)), shapes);
}

/// Unsigned byte-order comparison — the chain compares stored keys via
/// std::string_view (char_traits/memcmp semantics), NOT signed char.
bool key_less(const std::vector<char>& a, const std::vector<char>& b) {
   return std::string_view(a.data(), a.size()) < std::string_view(b.data(), b.size());
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(be_key_codec_tests)

BOOST_AUTO_TEST_CASE(slug_name_struct_roundtrip) {
   auto abi    = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});

   auto bytes = codec::encode_key(fc::variant(fc::mutable_variant_object("code", slug(42))), shapes);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u); // single uint64 field, BE

   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK_EQUAL(
      decoded.get_object()["code"].get_object()["value"].as_uint64(), 42u);
}

BOOST_AUTO_TEST_CASE(slug_name_byte_order_matches_value_order) {
   auto abi = make_test_abi();
   auto lo  = encode_single(abi, "slug_name", slug(2));
   auto hi  = encode_single(abi, "slug_name", slug(7));
   BOOST_CHECK(key_less(lo, hi));
}

BOOST_AUTO_TEST_CASE(typedef_chain_resolves_to_struct) {
   auto abi = make_test_abi();
   // chain_code_t -> code_alias -> slug_name: same encoding as the struct itself.
   auto direct  = encode_single(abi, "slug_name", slug(99));
   auto aliased = encode_single(abi, "chain_code_t", slug(99));
   BOOST_CHECK(direct == aliased);
}

BOOST_AUTO_TEST_CASE(nested_struct_key) {
   auto abi    = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"k"}, {"pair_key"});

   fc::variant key(fc::mutable_variant_object(
      "k", fc::mutable_variant_object("code", slug(5))("idx", 9)));
   auto bytes = codec::encode_key(key, shapes);
   BOOST_REQUIRE_EQUAL(bytes.size(), 12u); // uint64 + uint32

   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   const auto& k = decoded.get_object()["k"].get_object();
   BOOST_CHECK_EQUAL(k["code"].get_object()["value"].as_uint64(), 5u);
   BOOST_CHECK_EQUAL(k["idx"].as_uint64(), 9u);
}

BOOST_AUTO_TEST_CASE(fixed_bytes_32_aliases_checksum256) {
   auto abi  = make_test_abi();
   auto hash = fc::sha256::hash(std::string("abc"));
   fc::variant hex(hash.str());

   auto as_checksum = encode_single(abi, "checksum256", hex);
   auto as_template = encode_single(abi, "fixed_bytes<32>", hex);
   BOOST_REQUIRE_EQUAL(as_checksum.size(), 32u);
   BOOST_CHECK(as_checksum == as_template);
}

BOOST_AUTO_TEST_CASE(float128_roundtrip_and_ordering) {
   auto abi    = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"k"}, {"float128"});

   auto f128_var = [](double d) {
      float128_t f = ::f64_to_f128(to_softfloat64(d));
      fc::variant v;
      fc::to_variant(f, v);
      return v;
   };

   // Sort order must match numeric order across the sign boundary.
   std::vector<double> ordered{-2.5, -1.0, 0.0, 1.0, 2.5};
   std::vector<std::vector<char>> encoded;
   encoded.reserve(ordered.size());
   for (double d : ordered)
      encoded.push_back(encode_single(abi, "float128", f128_var(d)));
   for (size_t i = 0; i + 1 < encoded.size(); ++i)
      BOOST_CHECK(key_less(encoded[i], encoded[i + 1]));

   // Round-trip: decode(encode(x)) reproduces the canonical variant spelling.
   auto in    = f128_var(-1.0);
   auto bytes = encode_single(abi, "float128", in);
   auto out   = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK_EQUAL(out.get_object()["k"].as_string(), in.as_string());
}

BOOST_AUTO_TEST_CASE(rejections) {
   auto abi = make_test_abi();

   // Unknown type: neither leaf, typedef, nor struct.
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"mystery_type"}), fc::exception);

   // Struct with a base has no defined to_key field order — rejected.
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"based_key"}), fc::exception);

   // Bound object missing a struct field.
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});
   fc::variant missing(fc::mutable_variant_object(
      "code", fc::mutable_variant_object("wrong_field", 1)));
   BOOST_CHECK_THROW(codec::encode_key(missing, shapes), fc::exception);

   // Nesting depth guard.
   abi_def deep;
   deep.structs.emplace_back(struct_def{"level0", "", {field_def{"v", "uint64"}}});
   for (int i = 1; i <= 10; ++i)
      deep.structs.emplace_back(struct_def{
         "level" + std::to_string(i), "", {field_def{"inner", "level" + std::to_string(i - 1)}}});
   BOOST_CHECK_THROW(codec::build_key_shapes(deep, {"k"}, {"level10"}), fc::exception);
}

BOOST_AUTO_TEST_SUITE_END()
