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
      softfloat128_t f = ::f64_to_f128(to_softfloat64(d));
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

   // 256-bit integers have no codec leaf and no CDT producer. build_key_shapes
   // must reject them — this is exactly what drives chain_plugin to leave
   // key_shapes unset and fall back to hex bounds (the defensive nullopt path).
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"uint256"}), fc::exception);
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"int256"}), fc::exception);

   // Struct with a base has no defined to_key field order — rejected.
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"based_key"}), fc::exception);

   // Bound object missing a struct field.
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});
   fc::variant missing(fc::mutable_variant_object(
      "code", fc::mutable_variant_object("wrong_field", 1)));
   BOOST_CHECK_THROW(codec::encode_key(missing, shapes), fc::exception);

   // Nesting depth guard: a chain deeper than max_key_struct_depth is rejected.
   // (Also bounds self-referential struct definitions, which recurse until this
   // limit trips.)
   const int too_deep = static_cast<int>(codec::max_key_struct_depth) + 2;
   abi_def deep;
   deep.structs.emplace_back(struct_def{"level0", "", {field_def{"v", "uint64"}}});
   for (int i = 1; i <= too_deep; ++i)
      deep.structs.emplace_back(struct_def{
         "level" + std::to_string(i), "", {field_def{"inner", "level" + std::to_string(i - 1)}}});
   BOOST_CHECK_THROW(
      codec::build_key_shapes(deep, {"k"}, {"level" + std::to_string(too_deep)}), fc::exception);
}

// Pins the leaf_key_spellings table as the single source of truth: every
// spelling must resolve (via leaf_kind_of) to a key_leaf_kind handled by BOTH
// the encode_field and decode_field switches. A representative value is encoded
// then decoded for each spelling; a spelling that maps to an unhandled kind
// would trip the switches' defensive assert and fail here. The switches are
// exhaustive over the enum, so under -Wswitch a kind with no branch is a compile
// error; this test additionally exercises every spelling through the codec.
BOOST_AUTO_TEST_CASE(leaf_support_list_roundtrips) {
   auto sample = [](std::string_view t) -> fc::variant {
      if (t == "checksum256") return fc::variant(fc::sha256::hash(std::string("x")).str());
      if (t == "name")        return fc::variant(std::string("alice"));
      if (t == "bool")        return fc::variant(true);
      if (t == "string")      return fc::variant(std::string("hi"));
      if (t == "float128" || t == "long double") {
         softfloat128_t f = ::f64_to_f128(to_softfloat64(1.5));
         fc::variant v; fc::to_variant(f, v); return v;
      }
      if (t == "float32" || t == "float" || t == "float64" || t == "double")
         return fc::variant(1.5);
      // 128-bit ints: use fc's matched to_variant/from_variant spelling.
      if (t == "uint128") { fc::variant v; fc::to_variant(fc::uint128(7), v); return v; }
      if (t == "int128")  { fc::variant v; fc::to_variant(static_cast<fc::int128>(-3), v); return v; }
      if (t == "int8" || t == "int16" || t == "int32" || t == "int64")
         return fc::variant(static_cast<int64_t>(-3));
      return fc::variant(static_cast<uint64_t>(7)); // remaining unsigned ints
   };

   const abi_def abi; // builtin leaves need no typedefs/structs
   for (const auto& entry : codec::leaf_key_spellings) {
      const std::string_view t = entry.spelling;
      const std::string type{t};
      std::vector<char> bytes;
      BOOST_REQUIRE_NO_THROW(bytes = encode_single(abi, type, sample(t)));
      auto shapes = codec::build_key_shapes(abi, {"k"}, {type});
      BOOST_CHECK_NO_THROW(codec::decode_key(bytes.data(), bytes.size(), shapes));
   }
}

// The float aliases (and "bool") must collapse onto the same key_leaf_kind as
// their canonical spelling — i.e. produce byte-identical encodings. The
// round-trip test above only exercises each spelling on its own, so a
// mis-mapped alias (e.g. "double" -> float32) would still round-trip and slip
// past it; the encoded-size/byte mismatch is caught here.
BOOST_AUTO_TEST_CASE(leaf_spelling_aliases_match_canonical) {
   const abi_def abi;

   auto f128_var = [](double d) {
      softfloat128_t f = ::f64_to_f128(to_softfloat64(d));
      fc::variant v;
      fc::to_variant(f, v);
      return v;
   };

   BOOST_CHECK(encode_single(abi, "float", fc::variant(1.5))
               == encode_single(abi, "float32", fc::variant(1.5)));
   BOOST_CHECK(encode_single(abi, "double", fc::variant(1.5))
               == encode_single(abi, "float64", fc::variant(1.5)));
   BOOST_CHECK(encode_single(abi, "long double", f128_var(1.5))
               == encode_single(abi, "float128", f128_var(1.5)));
}

// A struct key with zero fields is a node with no children: it must encode to
// zero bytes (matching to_key's reflected walk over no fields) rather than be
// misrouted to the leaf codec and rejected. Guards the explicit key_shape
// is_leaf flag against the old `children.empty()` inference.
BOOST_AUTO_TEST_CASE(empty_struct_key_encodes_to_zero_bytes) {
   abi_def abi;
   abi.structs.emplace_back(struct_def{"empty_key", "", {}});
   auto shapes = codec::build_key_shapes(abi, {"k"}, {"empty_key"});

   auto bytes = codec::encode_key(
      fc::variant(fc::mutable_variant_object("k", fc::mutable_variant_object())), shapes);
   BOOST_CHECK(bytes.empty());

   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_REQUIRE(decoded.get_object()["k"].is_object());
   BOOST_CHECK_EQUAL(decoded.get_object()["k"].get_object().size(), 0u);
}

// A typedef alias cycle must be reported precisely as a cycle, not as the
// generic "Unsupported BE key type" the shape builder would otherwise surface
// once the walk landed on the still-unresolved alias. Pins resolve_key_type's
// visited-set guard and its diagnostic.
BOOST_AUTO_TEST_CASE(typedef_cycle_is_rejected) {
   auto has_cycle_msg = [](const fc::exception& e) {
      return e.top_message().find("Typedef cycle") != std::string::npos;
   };

   // Two-hop cycle: a -> b -> a.
   abi_def two;
   two.types.emplace_back(type_def{"a", "b"});
   two.types.emplace_back(type_def{"b", "a"});
   BOOST_CHECK_EXCEPTION(codec::build_key_shapes(two, {"k"}, {"a"}), fc::exception, has_cycle_msg);

   // Self-cycle: s -> s.
   abi_def self;
   self.types.emplace_back(type_def{"s", "s"});
   BOOST_CHECK_EXCEPTION(codec::build_key_shapes(self, {"k"}, {"s"}), fc::exception, has_cycle_msg);
}

// Scoped table whose within-scope primary key is a struct (slug_name). The real
// v6 registry tables are unscoped, but chain_plugin supports scoped tables by
// stripping the leading scope field's shape from the bound shapes and encoding
// only the within-scope portion (see get_table_rows' scope_key_count erase).
// This pins that slice-then-encode path for a struct-typed within-scope key:
// build the full [scope=name, code=slug_name] shapes, drop the scope shape as
// the plugin does for a scoped JSON bound, and round-trip the struct remainder.
BOOST_AUTO_TEST_CASE(scoped_struct_key_within_scope_roundtrip) {
   auto abi = make_test_abi();

   // Full key list: a leading "scope" leaf (name) followed by a struct "code"
   // (slug_name) — the shape of a scoped kv table keyed by a struct per scope.
   auto full = codec::build_key_shapes(abi, {"scope", "code"}, {"name", "slug_name"});
   BOOST_REQUIRE_EQUAL(full.size(), 2u);
   BOOST_CHECK(full[0].is_leaf);   // scope resolves to the name leaf
   BOOST_CHECK(!full[1].is_leaf);  // code is the slug_name struct node

   // chain_plugin strips the leading scope shape for a scoped bound; the
   // remaining shapes encode/decode the within-scope key only.
   std::vector<codec::key_shape> within_scope(full.begin() + 1, full.end());

   auto bytes = codec::encode_key(fc::variant(fc::mutable_variant_object("code", slug(42))), within_scope);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u); // single uint64 struct field, BE

   auto decoded = codec::decode_key(bytes.data(), bytes.size(), within_scope);
   BOOST_CHECK_EQUAL(decoded.get_object()["code"].get_object()["value"].as_uint64(), 42u);

   // The leading scope is a pure prefix: the within-scope struct bytes are
   // identical to that struct keyed on its own.
   BOOST_CHECK(bytes == encode_single(abi, "slug_name", slug(42)));
}

BOOST_AUTO_TEST_SUITE_END()
