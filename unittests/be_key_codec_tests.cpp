#include <fc/slug_name.hpp>
#include <boost/test/unit_test.hpp>

#include <sysio/chain/database_utils.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>

using namespace sysio::chain;
namespace codec = sysio::chain::be_key_codec;

/**
 * Direct coverage for the ABI-aware BE key codec: typedef resolution, struct
 * key expansion (a single-uint64 struct key, the pre-builtin `slug_name` shape),
 * abigen template-spelling canonicalization, the float128 leaf, and the
 * shape builder's rejection paths. End-to-end bound/pagination behaviour is
 * covered in tests/get_table_tests.cpp; these pin the codec layer itself.
 */

namespace {

/// ABI fixture: a single-uint64 struct key, a two-hop typedef chain onto it, a
/// nested struct key, and a based struct (rejected by design).
///
/// Deliberately NOT named `slug_name`: that spelling is an abi_serializer
/// builtin and a `leaf_key_spellings` entry, so a fixture using it would take
/// the leaf branch in `build_key_shape` and stop exercising the struct-key and
/// typedef-chain paths — while still passing. This is the repo's only coverage
/// of those paths.
abi_def make_test_abi() {
   abi_def abi;
   abi.types.emplace_back(type_def{"chain_code_t", "code_alias"});
   abi.types.emplace_back(type_def{"code_alias", "composite_key"});
   abi.structs.emplace_back(struct_def{"composite_key", "", {field_def{"value", "uint64"}}});
   abi.structs.emplace_back(
      struct_def{"pair_key", "", {field_def{"code", "composite_key"}, field_def{"idx", "uint32"}}});
   abi.structs.emplace_back(struct_def{"based_key", "composite_key", {field_def{"extra", "uint64"}}});
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

BOOST_AUTO_TEST_CASE(composite_key_struct_roundtrip) {
   auto abi    = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"composite_key"});

   auto bytes = codec::encode_key(fc::variant(fc::mutable_variant_object("code", slug(42))), shapes);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u); // single uint64 field, BE

   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK_EQUAL(
      decoded.get_object()["code"].get_object()["value"].as_uint64(), 42u);
}

// ── slug_name as a codec LEAF ───────────────────────────────────────────────
// `slug_name` is an abi_serializer builtin and a leaf_key_spellings entry, so
// it needs no abi.structs entry here — build_key_shapes resolves it through
// leaf_kind_of. These pin the leaf's carrier and the two properties the registry
// registry tables depend on: byte compatibility with the struct-key encoding it
// replaced, and prefix grouping.

BOOST_AUTO_TEST_CASE(slug_name_leaf_bytes_match_the_struct_node_it_replaced) {
   // The no-migration guarantee: the struct-node path recursed one uint64 child
   // to write_be64, and the leaf path IS write_be64. `composite_key` still
   // exercises the struct path, so it is the reference encoding.
   auto abi = make_test_abi();
   const uint64_t packed = fc::slug_name{"LIQSOL"}.value;
   BOOST_CHECK(encode_single(abi, "slug_name", fc::variant("LIQSOL"))
               == encode_single(abi, "composite_key", slug(packed)));
}

BOOST_AUTO_TEST_CASE(slug_name_leaf_roundtrips_a_canonical_slug_as_a_string) {
   auto abi    = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});
   auto bytes  = codec::encode_key(
      fc::variant(fc::mutable_variant_object("code", "LIQSOL")), shapes);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK_EQUAL(decoded.get_object()["code"].as_string(), "LIQSOL");
}

BOOST_AUTO_TEST_CASE(slug_name_leaf_roundtrips_the_zero_sentinel_as_empty) {
   auto abi = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});
   auto bytes  = codec::encode_key(
      fc::variant(fc::mutable_variant_object("code", "")), shapes);
   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK_EQUAL(decoded.get_object()["code"].as_string(), "");
   BOOST_CHECK(bytes == encode_single(abi, "composite_key", slug(0)));
}

BOOST_AUTO_TEST_CASE(slug_name_leaf_decodes_a_non_canonical_value_lossily) {
   // A value below 2^42 has no string spelling (to_string truncates at the first
   // zero symbol slot). The leaf converts it anyway — you get what you get,
   // identical to the `name` leaf (`name(raw).to_string()`). It neither throws
   // nor falls back to hex: a read path that throws costs the whole scan, and a
   // raw uint64 that spells nothing is self-inflicted, since nothing validates
   // the raw ctor for either type.
   auto abi    = make_test_abi();
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});

   // A bound is still named by its SPELLING, so a bare integer is not one.
   BOOST_CHECK_THROW(
      codec::encode_key(fc::variant(fc::mutable_variant_object("code", 7u)), shapes),
      fc::exception);

   // A key already holding one decodes to "" — the same text zero renders, so
   // feeding it back re-encodes to 0. That is the price of a total conversion.
   // Reach past the carrier to build those bytes: the transitional object arm
   // is the only writer left that can express a raw value.
   auto bytes = codec::encode_key(
      fc::variant(fc::mutable_variant_object("code", slug(7))), shapes);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK_EQUAL(decoded.get_object()["code"].as_string(), "");
}

BOOST_AUTO_TEST_CASE(slug_name_leaf_wins_over_a_shadowing_struct_def) {
   // The collision spans TWO independent resolution sites. abi_tests covers the
   // action/row data path (built_in_types at abi_serializer.cpp:706 before
   // structs at :778); the key codec is its own lookup (leaf_kind_of at
   // database_utils.hpp:558 before the struct table), and it needs its own pin.
   //
   // Every shipped registry ABI carries a `slug_name` struct_def alongside the
   // field, and `slug_name` is the ONLY builtin name so shadowed. `set_abi` has
   // no collision check, so the ABI is genuinely ambiguous and resolved only by
   // lookup order. If the struct won here, `encode_key` would demand the nested
   // `{"code":{"value":N}}` form and `decode_key` would emit it — so `is_leaf`
   // is the exact discriminator for the bounds and `next_key` path.
   //
   // Deliberately NOT built on make_test_abi(): that fixture avoids the name
   // `slug_name` on purpose, and its avoidance is load-bearing for the
   // struct-expansion and typedef-chain cases.
   abi_def abi;
   abi.structs.emplace_back(struct_def{"slug_name", "", {field_def{"value", "uint64"}}});

   auto shapes = codec::build_key_shapes(abi, {"code"}, {"slug_name"});
   BOOST_REQUIRE_EQUAL(shapes.size(), 1u);
   BOOST_REQUIRE(shapes[0].is_leaf);
   BOOST_CHECK(shapes[0].kind == codec::key_leaf_kind::slug_name);
   BOOST_CHECK(shapes[0].children.empty());

   // And it round-trips as the leaf carrier, not as a nested object.
   auto bytes = codec::encode_key(
      fc::variant(fc::mutable_variant_object("code", "LIQSOL")), shapes);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u);
   auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
   BOOST_CHECK(decoded.get_object()["code"].is_string());
   BOOST_CHECK_EQUAL(decoded.get_object()["code"].as_string(), "LIQSOL");
}

BOOST_AUTO_TEST_CASE(slug_name_multi_leaf_keys_preserve_field_order_and_offsets) {
   // THREE of the five registry tables key on more than one slug:
   //   sysio.tokens::chaintokens ["slug_name","slug_name"]
   //   sysio.reserv::reserves    ["slug_name","slug_name","slug_name"]
   //   sysio.uwrit::locksums     ["name","slug_name","slug_name"]
   // Every other slug case here builds a SINGLE leaf, and a single leaf cannot
   // observe a field-ordering or offset error because there is only one field to
   // misplace. decode_key walks the shapes in order consuming a fixed width each,
   // and encode_key must reproduce that ordering from the decoded object.
   auto abi = make_test_abi();

   // 2-leaf, mirroring chaintokens.
   {
      auto shapes = codec::build_key_shapes(abi, {"chain_code", "token_code"},
                                            {"slug_name", "slug_name"});
      auto bytes  = codec::encode_key(
         fc::variant(fc::mutable_variant_object("chain_code", "ETH")("token_code", "USDC")),
         shapes);
      BOOST_REQUIRE_EQUAL(bytes.size(), 16u);
      // Ordering is observable: each leaf must occupy its own 8-byte window, in
      // declaration order. Swapping the two would keep the size and fail here.
      BOOST_CHECK(std::vector<char>(bytes.begin(), bytes.begin() + 8)
                  == encode_single(abi, "slug_name", fc::variant("ETH")));
      BOOST_CHECK(std::vector<char>(bytes.begin() + 8, bytes.end())
                  == encode_single(abi, "slug_name", fc::variant("USDC")));
      auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
      BOOST_CHECK_EQUAL(decoded.get_object()["chain_code"].as_string(), "ETH");
      BOOST_CHECK_EQUAL(decoded.get_object()["token_code"].as_string(), "USDC");
      BOOST_CHECK(bytes == codec::encode_key(decoded, shapes));
   }

   // 3-leaf, mirroring reserves. The middle leaf is a DIFFERENT length from its
   // neighbours, so a decoder that mis-tracked its offset would land inside an
   // adjacent slug and still produce a string.
   {
      auto shapes = codec::build_key_shapes(abi, {"chain_code", "token_code", "reserve_code"},
                                            {"slug_name", "slug_name", "slug_name"});
      auto bytes  = codec::encode_key(
         fc::variant(fc::mutable_variant_object("chain_code", "ETH")
                                               ("token_code", "USDC")
                                               ("reserve_code", "PRIMARY")),
         shapes);
      BOOST_REQUIRE_EQUAL(bytes.size(), 24u);
      BOOST_CHECK(std::vector<char>(bytes.begin() + 8, bytes.begin() + 16)
                  == encode_single(abi, "slug_name", fc::variant("USDC")));
      auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
      BOOST_CHECK_EQUAL(decoded.get_object()["chain_code"].as_string(), "ETH");
      BOOST_CHECK_EQUAL(decoded.get_object()["token_code"].as_string(), "USDC");
      BOOST_CHECK_EQUAL(decoded.get_object()["reserve_code"].as_string(), "PRIMARY");
      BOOST_CHECK(bytes == codec::encode_key(decoded, shapes));
   }

   // name + 2 slugs, mirroring locksums — a mixed-KIND composite.
   {
      auto shapes = codec::build_key_shapes(abi, {"underwriter", "chain_code", "token_code"},
                                            {"name", "slug_name", "slug_name"});
      auto bytes  = codec::encode_key(
         fc::variant(fc::mutable_variant_object("underwriter", "uw.a")
                                               ("chain_code", "SOLANA")
                                               ("token_code", "SOL")),
         shapes);
      BOOST_REQUIRE_EQUAL(bytes.size(), 24u);
      auto decoded = codec::decode_key(bytes.data(), bytes.size(), shapes);
      BOOST_CHECK_EQUAL(decoded.get_object()["underwriter"].as_string(), "uw.a");
      BOOST_CHECK_EQUAL(decoded.get_object()["chain_code"].as_string(), "SOLANA");
      BOOST_CHECK_EQUAL(decoded.get_object()["token_code"].as_string(), "SOL");
      BOOST_CHECK(bytes == codec::encode_key(decoded, shapes));
   }
}

BOOST_AUTO_TEST_CASE(slug_name_leaf_groups_shared_prefixes) {
   // The property slug_name was designed for: MSB-first 6-bit packing puts
   // char[0] at bits [42..47], so a shared textual prefix is a shared leading
   // BYTE prefix of the key — k symbols share 2 + floor(6k/8) bytes, the 2 from
   // the unused top 16 bits. Grouping is byte-exact only at k = 4 and k = 8.
   auto abi = make_test_abi();
   auto shared_bytes = [&](const char* a, const char* b) {
      auto ka = encode_single(abi, "slug_name", fc::variant(a));
      auto kb = encode_single(abi, "slug_name", fc::variant(b));
      size_t n = 0;
      while (n < ka.size() && n < kb.size() && ka[n] == kb[n]) ++n;
      return n;
   };
   BOOST_CHECK_EQUAL(shared_bytes("LIQSOL", "LIQETH"), 4u);  // k=3 -> 2 + 2
   BOOST_CHECK_EQUAL(shared_bytes("WIRE", "WIREUSD"), 5u);   // k=4 -> 2 + 3, aligned
   BOOST_CHECK_EQUAL(shared_bytes("USDC", "USDT"), 4u);      // k=3 -> 2 + 2
}

BOOST_AUTO_TEST_CASE(composite_key_byte_order_matches_value_order) {
   auto abi = make_test_abi();
   auto lo  = encode_single(abi, "composite_key", slug(2));
   auto hi  = encode_single(abi, "composite_key", slug(7));
   BOOST_CHECK(key_less(lo, hi));
}

BOOST_AUTO_TEST_CASE(typedef_chain_resolves_to_struct) {
   auto abi = make_test_abi();
   // chain_code_t -> code_alias -> composite_key: same encoding as the struct itself.
   auto direct  = encode_single(abi, "composite_key", slug(99));
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
   // key_shapes unset, so json=true rejects and hex bounds are the only form
   // (the defensive nullopt path).
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"uint256"}), fc::exception);
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"int256"}), fc::exception);

   // Struct with a base has no defined to_key field order — rejected.
   BOOST_CHECK_THROW(codec::build_key_shapes(abi, {"k"}, {"based_key"}), fc::exception);

   // Bound object missing a struct field.
   auto shapes = codec::build_key_shapes(abi, {"code"}, {"composite_key"});
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
      if (t == "slug_name")   return fc::variant(std::string("LIQSOL"));
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

// Scoped table whose within-scope primary key is a struct (composite_key). The real
// registry tables are unscoped, but chain_plugin supports scoped tables by
// stripping the leading scope field's shape from the bound shapes and encoding
// only the within-scope portion (see get_table_rows' scope_key_count erase).
// This pins that slice-then-encode path for a struct-typed within-scope key:
// build the full [scope=name, code=composite_key] shapes, drop the scope shape as
// the plugin does for a scoped JSON bound, and round-trip the struct remainder.
BOOST_AUTO_TEST_CASE(scoped_struct_key_within_scope_roundtrip) {
   auto abi = make_test_abi();

   // Full key list: a leading "scope" leaf (name) followed by a struct "code"
   // (composite_key) — the shape of a scoped kv table keyed by a struct per scope.
   auto full = codec::build_key_shapes(abi, {"scope", "code"}, {"name", "composite_key"});
   BOOST_REQUIRE_EQUAL(full.size(), 2u);
   BOOST_CHECK(full[0].is_leaf);   // scope resolves to the name leaf
   BOOST_CHECK(!full[1].is_leaf);  // code is the composite_key struct node

   // chain_plugin strips the leading scope shape for a scoped bound; the
   // remaining shapes encode/decode the within-scope key only.
   std::vector<codec::key_shape> within_scope(full.begin() + 1, full.end());

   auto bytes = codec::encode_key(fc::variant(fc::mutable_variant_object("code", slug(42))), within_scope);
   BOOST_REQUIRE_EQUAL(bytes.size(), 8u); // single uint64 struct field, BE

   auto decoded = codec::decode_key(bytes.data(), bytes.size(), within_scope);
   BOOST_CHECK_EQUAL(decoded.get_object()["code"].get_object()["value"].as_uint64(), 42u);

   // The leading scope is a pure prefix: the within-scope struct bytes are
   // identical to that struct keyed on its own.
   BOOST_CHECK(bytes == encode_single(abi, "composite_key", slug(42)));
}

BOOST_AUTO_TEST_SUITE_END()
