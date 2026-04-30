#include <boost/test/unit_test.hpp>

#include <fc/bitset.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/io/enum_type.hpp>
#include <fc/io/json.hpp>
#include <fc/log/log_message.hpp>
#include <fc/network/url.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/static_variant.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>

#include <fc/scoped_exit.hpp>

#include <charconv>
#include <clocale>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct point_t {
   int32_t     x = 0;
   int32_t     y = 0;
   std::string label;
};

struct nested_t {
   std::string           name;
   std::vector<int32_t>  values;
   std::optional<int32_t> maybe;
   point_t               where;
};

enum class color_t { red, green, blue };

struct with_optional_t {
   int32_t                  id = 0;
   std::optional<std::string> note;
};

struct with_map_t {
   std::map<std::string, int32_t> counts;
};

struct block_like_t {
   fc::time_point timestamp;
   fc::sha256     digest;
   std::string    producer;
};

} // namespace

FC_REFLECT(point_t, (x)(y)(label))
FC_REFLECT(nested_t, (name)(values)(maybe)(where))
FC_REFLECT_ENUM(color_t, (red)(green)(blue))
FC_REFLECT(with_optional_t, (id)(note))
FC_REFLECT(with_map_t, (counts))
FC_REFLECT(block_like_t, (timestamp)(digest)(producer))

BOOST_AUTO_TEST_SUITE(json_stream_test)

BOOST_AUTO_TEST_CASE(primitives) {
   // Scalars serialize the same way fc::json::to_string(variant(v)) would.
   BOOST_CHECK_EQUAL(fc::to_json_string(true),         "true");
   BOOST_CHECK_EQUAL(fc::to_json_string(false),        "false");
   BOOST_CHECK_EQUAL(fc::to_json_string(int32_t{0}),   "0");
   BOOST_CHECK_EQUAL(fc::to_json_string(int32_t{-7}),  "-7");
   BOOST_CHECK_EQUAL(fc::to_json_string(uint64_t{42}), "42");
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"hi"}), "\"hi\"");
}

BOOST_AUTO_TEST_CASE(array_of_char_emits_hex_string) {
   // std::array<char,N> serializes as a lowercase hex string in fc::variant
   // (matches vector<char>); streaming must produce the same shape, not an
   // array of byte-numbers from the generic std::array<T,S> template.
   auto via_variant = [](const auto& v) { return fc::json::to_string(fc::variant(v), fc::json::yield_function_t()); };

   std::array<char, 4> bytes{0x00, 0x01, 0x02, 0x03};
   BOOST_CHECK_EQUAL(fc::to_json_string(bytes), "\"00010203\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(bytes), via_variant(bytes));

   std::array<char, 32> wider{};
   wider[0] = static_cast<char>(0xff); wider[31] = 0x42;
   BOOST_CHECK_EQUAL(fc::to_json_string(wider), via_variant(wider));
}

BOOST_AUTO_TEST_CASE(large_int_quoting_matches_variant) {
   // Variant emits 64-bit integers with |v| > 0xffffffff as quoted strings to
   // preserve precision past JS's 2^53 limit.  Streaming must match.
   auto via_variant = [](auto v) { return fc::json::to_string(fc::variant(v), fc::json::yield_function_t()); };

   BOOST_CHECK_EQUAL(fc::to_json_string(int64_t{0xffffffff}),       via_variant(int64_t{0xffffffff}));
   BOOST_CHECK_EQUAL(fc::to_json_string(int64_t{0x100000000}),      via_variant(int64_t{0x100000000}));
   BOOST_CHECK_EQUAL(fc::to_json_string(int64_t{-0x100000000}),     via_variant(int64_t{-0x100000000}));
   BOOST_CHECK_EQUAL(fc::to_json_string(uint64_t{0xffffffff}),      via_variant(uint64_t{0xffffffff}));
   BOOST_CHECK_EQUAL(fc::to_json_string(uint64_t{0x100000000}),     via_variant(uint64_t{0x100000000}));
   BOOST_CHECK_EQUAL(fc::to_json_string(uint64_t{0xffffffffffffffff}), via_variant(uint64_t{0xffffffffffffffff}));
}

BOOST_AUTO_TEST_CASE(value_double_locale_independent) {
   // %g honors LC_NUMERIC: under de_DE the radix becomes ','  -> invalid JSON.
   // value_double must use a locale-independent formatter.  scoped_exit ensures
   // the locale is restored even if a BOOST_CHECK throws.
   const char* saved = std::setlocale(LC_NUMERIC, nullptr);
   const std::string saved_locale = saved ? saved : "C";
   auto restore = fc::make_scoped_exit([&]{ std::setlocale(LC_NUMERIC, saved_locale.c_str()); });

   const char* comma_locales[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "C-decimal-comma"};
   bool found = false;
   for (const char* loc : comma_locales) {
      if (std::setlocale(LC_NUMERIC, loc)) { found = true; break; }
   }
   if (!found) {
      BOOST_TEST_MESSAGE("no comma-radix locale available; skipping locale-independence assertion");
      return;
   }
   BOOST_CHECK_EQUAL(fc::to_json_string(1.5), "1.5");
   BOOST_CHECK_EQUAL(fc::to_json_string(-2.25), "-2.25");
   // Round-trip: any finite double must parse back to its bit-exact value.
   // strtod_l with the C locale avoids re-introducing the comma-radix issue.
   auto round_trip = [](double d) {
      const std::string s = fc::to_json_string(d);
      double parsed = 0.0;
      auto [_, ec] = std::from_chars(s.data(), s.data() + s.size(), parsed);
      BOOST_REQUIRE(ec == std::errc{});
      BOOST_CHECK_EQUAL(parsed, d);
   };
   round_trip(0.1);
   round_trip(1.0 / 3.0);
   round_trip(std::numeric_limits<double>::min());
   round_trip(std::numeric_limits<double>::max());
}

BOOST_AUTO_TEST_CASE(value_double_rejects_non_finite) {
   // NaN / +-inf have no JSON representation; value_double must throw rather than
   // emit unquoted nan/inf tokens that downstream parsers reject.
   const auto nan = std::numeric_limits<double>::quiet_NaN();
   const auto pos_inf = std::numeric_limits<double>::infinity();
   const auto neg_inf = -std::numeric_limits<double>::infinity();

   std::string out;
   {
      fc::json_writer w(out);
      BOOST_CHECK_THROW(w.value_double(nan),     std::invalid_argument);
   }
   BOOST_CHECK(out.empty()); // nothing written, frame state untouched
   {
      fc::json_writer w(out);
      BOOST_CHECK_THROW(w.value_double(pos_inf), std::invalid_argument);
      BOOST_CHECK_THROW(w.value_double(neg_inf), std::invalid_argument);
   }
   // Finite values: to_json_stream(double) emits the quoted fixed-precision form
   // matching the variant path's emission shape.
   BOOST_CHECK_EQUAL(fc::to_json_string(0.0), "\"0.00000000000000000\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(1.5), "\"1.50000000000000000\"");
}

BOOST_AUTO_TEST_CASE(string_escaping) {
   // Control characters must be JSON-escaped.
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"a\"b"}),  "\"a\\\"b\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"a\nb"}),  "\"a\\nb\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"\t"}),     "\"\\t\"");
}

BOOST_AUTO_TEST_CASE(vector_and_optional) {
   std::vector<int32_t> v{1, 2, 3};
   BOOST_CHECK_EQUAL(fc::to_json_string(v), "[1,2,3]");

   std::optional<int32_t> unset;
   BOOST_CHECK_EQUAL(fc::to_json_string(unset), "null");

   std::optional<int32_t> set{7};
   BOOST_CHECK_EQUAL(fc::to_json_string(set), "7");
}

BOOST_AUTO_TEST_CASE(reflected_struct) {
   point_t p{.x = 3, .y = -4, .label = "origin"};
   BOOST_CHECK_EQUAL(fc::to_json_string(p), "{\"x\":3,\"y\":-4,\"label\":\"origin\"}");
}

BOOST_AUTO_TEST_CASE(nested_struct) {
   nested_t n{
      .name   = "n",
      .values = {1, 2},
      .maybe  = std::nullopt,
      .where  = {.x = 0, .y = 1, .label = "p"}
   };
   // `maybe` is std::nullopt so it's omitted per to_variant_visitor::add semantics.
   BOOST_CHECK_EQUAL(
      fc::to_json_string(n),
      "{\"name\":\"n\",\"values\":[1,2],\"where\":{\"x\":0,\"y\":1,\"label\":\"p\"}}");
}

BOOST_AUTO_TEST_CASE(nested_optional_present) {
   nested_t n{
      .name   = "n",
      .values = {},
      .maybe  = 5,
      .where  = {.x = 0, .y = 0, .label = ""}
   };
   BOOST_CHECK_EQUAL(
      fc::to_json_string(n),
      "{\"name\":\"n\",\"values\":[],\"maybe\":5,\"where\":{\"x\":0,\"y\":0,\"label\":\"\"}}");
}

BOOST_AUTO_TEST_CASE(enum_value) {
   // FC_REFLECT_ENUM emits the enum name via reflector::to_fc_string.
   BOOST_CHECK_EQUAL(fc::to_json_string(color_t::red),   "\"red\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(color_t::green), "\"green\"");
}

BOOST_AUTO_TEST_CASE(optional_field_omitted) {
   // Unset optional field on a reflected struct is omitted entirely.
   with_optional_t o{.id = 1, .note = std::nullopt};
   BOOST_CHECK_EQUAL(fc::to_json_string(o), "{\"id\":1}");
}

BOOST_AUTO_TEST_CASE(map_object_emission) {
   with_map_t m;
   m.counts["a"] = 1;
   m.counts["b"] = 2;
   // std::map iterates in sorted order so output is deterministic.  Maps serialize as an
   // array of [key, value] pairs (matching the long-standing to_variant_from_map shape),
   // not a JSON object -- this preserves round-trip support for non-string key types.
   BOOST_CHECK_EQUAL(fc::to_json_string(m), "{\"counts\":[[\"a\",1],[\"b\",2]]}");
}

BOOST_AUTO_TEST_CASE(raw_value_embeds_fragment) {
   // raw_value should splice a pre-serialized JSON fragment at a value position.
   std::string out;
   {
      fc::json_writer w(out);
      w.begin_object();
      w.key("a");
      w.value_int32(1);
      w.key("b");
      w.raw_value("{\"x\":42}");
      w.end_object();
   }
   BOOST_CHECK_EQUAL(out, "{\"a\":1,\"b\":{\"x\":42}}");
}

BOOST_AUTO_TEST_CASE(array_of_reflected) {
   std::vector<point_t> pts{
      {.x = 1, .y = 2, .label = "a"},
      {.x = 3, .y = 4, .label = "b"},
   };
   BOOST_CHECK_EQUAL(
      fc::to_json_string(pts),
      "[{\"x\":1,\"y\":2,\"label\":\"a\"},{\"x\":3,\"y\":4,\"label\":\"b\"}]");
}

// -- fc type overloads --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(fc_microseconds) {
   // fc::to_variant renders microseconds as the int64 count; match that here.
   BOOST_CHECK_EQUAL(fc::to_json_string(fc::microseconds{0}),        "0");
   BOOST_CHECK_EQUAL(fc::to_json_string(fc::microseconds{1'234'567}), "1234567");
   BOOST_CHECK_EQUAL(fc::to_json_string(fc::microseconds{-5}),       "-5");
}

BOOST_AUTO_TEST_CASE(fc_time_point_roundtrip_vs_variant) {
   // Golden: JSON form must be identical to the existing to_variant -> json::to_string path
   // so clients see no change when an endpoint migrates to the streaming writer.
   const fc::time_point tp = fc::time_point::from_iso_string("2024-07-01T12:34:56.789");
   std::string stream_out = fc::to_json_string(tp);
   fc::variant v;
   fc::to_variant(tp, v);
   std::string variant_out = fc::json::to_string(v, fc::json::yield_function_t());
   BOOST_CHECK_EQUAL(stream_out, variant_out);
}

BOOST_AUTO_TEST_CASE(fc_time_point_sec_roundtrip_vs_variant) {
   const fc::time_point_sec tps{ 1'720'000'000u };
   std::string stream_out = fc::to_json_string(tps);
   fc::variant v;
   fc::to_variant(tps, v);
   std::string variant_out = fc::json::to_string(v, fc::json::yield_function_t());
   BOOST_CHECK_EQUAL(stream_out, variant_out);
}

BOOST_AUTO_TEST_CASE(fc_sha256_hex_form) {
   // sha256's canonical JSON form is its lowercase hex string (what .str() returns).
   // to_variant stores raw bytes and relies on json::to_string base16-encoding at write
   // time; to_json_stream shortcuts straight to the hex string, which must match.
   const fc::sha256 h{ "0000000000000000000000000000000000000000000000000000000000000abc" };
   BOOST_CHECK_EQUAL(
      fc::to_json_string(h),
      "\"0000000000000000000000000000000000000000000000000000000000000abc\"");
}

BOOST_AUTO_TEST_CASE(fc_variant_fallback) {
   // fc::variant overload defers to fc::json::to_string; result must match the existing
   // path byte-for-byte.
   fc::variant v{ int64_t{42} };
   BOOST_CHECK_EQUAL(fc::to_json_string(v),
                     fc::json::to_string(v, fc::json::yield_function_t()));
   v = std::string{"hello"};
   BOOST_CHECK_EQUAL(fc::to_json_string(v),
                     fc::json::to_string(v, fc::json::yield_function_t()));
}

BOOST_AUTO_TEST_CASE(fc_variant_object_fallback) {
   fc::mutable_variant_object mvo;
   mvo("a", 1)("b", "two");
   fc::variant as_var = fc::variant(mvo);
   BOOST_CHECK_EQUAL(fc::to_json_string(mvo),
                     fc::json::to_string(as_var, fc::json::yield_function_t()));
}

// Composition test: a reflected struct that has fc::time_point and fc::sha256 fields.
BOOST_AUTO_TEST_CASE(reflector_with_fc_types) {
   // The reflector-based path must find the to_json_stream overloads for fc::time_point
   // and fc::sha256 via ordinary (namespace-scope) lookup.  Without them, compilation
   // would fail; with them, output matches the to_variant -> json::to_string golden path.
   block_like_t b{
      .timestamp = fc::time_point::from_iso_string("2024-07-01T00:00:00.000"),
      .digest    = fc::sha256{ "deadbeef00000000000000000000000000000000000000000000000000000000" },
      .producer  = "producer1",
   };
   std::string stream_out = fc::to_json_string(b);
   fc::variant v;
   fc::to_variant(b, v);
   std::string variant_out = fc::json::to_string(v, fc::json::yield_function_t());
   BOOST_CHECK_EQUAL(stream_out, variant_out);
}

BOOST_AUTO_TEST_CASE(fc_public_key_and_signature) {
   // Signatures + public keys serialize as their prefixed base58 string form ("PUB_K1_...",
   // "SIG_K1_...") in existing JSON output.  Round-trip via the streaming path and
   // compare against the to_variant + json::to_string output.
   const fc::crypto::private_key priv = fc::crypto::private_key::generate();
   const fc::crypto::public_key pub = priv.get_public_key();

   fc::variant v_pub;
   fc::to_variant(pub, v_pub);
   BOOST_CHECK_EQUAL(fc::to_json_string(pub),
                     fc::json::to_string(v_pub, fc::json::yield_function_t()));

   const char* msg = "some-digest";
   const fc::crypto::signature sig = priv.sign(fc::sha256::hash(msg, std::strlen(msg)));
   fc::variant v_sig;
   fc::to_variant(sig, v_sig);
   BOOST_CHECK_EQUAL(fc::to_json_string(sig),
                     fc::json::to_string(v_sig, fc::json::yield_function_t()));
}

BOOST_AUTO_TEST_CASE(set_chained_object) {
   // w.set("name", value) is sugar over w.key("name") + to_json_stream(value, w).
   // Returns *this so call sites can chain.  The expected output is byte-identical
   // to the equivalent key()/value_*() sequence.
   std::string out;
   {
      fc::json_writer w(out);
      w.begin_object();
      w.set("a", 1)
       .set("b", std::string{"two"})
       .set("c", true)
       .set("d", 3.5);
      w.end_object();
   }
   BOOST_CHECK_EQUAL(out, R"({"a":1,"b":"two","c":true,"d":"3.50000000000000000"})");
}

BOOST_AUTO_TEST_CASE(set_dispatches_via_to_json_stream) {
   // Confirms set picks up to_json_stream overloads for fc leaf types and reflected
   // structs - same dispatch rule as the free-function fc::to_json_stream.
   const fc::sha256 h = fc::sha256::hash(std::string("hello"));
   point_t s{42, 7, "x"};
   std::vector<int32_t> nums{1, 2, 3};

   std::string out;
   {
      fc::json_writer w(out);
      w.begin_object();
      w.set("hash",     h)        // dispatches to_json_stream(sha256, w)
       .set("inner",    s)        // dispatches via reflector path
       .set("nums",     nums);    // dispatches to_json_stream(vector<int>, w)
      w.end_object();
   }

   // Cross-check against the variant + json::to_string output for the same shape.
   fc::variant v_hash, v_inner, v_nums;
   fc::to_variant(h, v_hash);
   fc::to_variant(s, v_inner);
   fc::to_variant(nums, v_nums);
   const std::string expected =
      std::string{"{\"hash\":"} + fc::json::to_string(v_hash,  fc::json::yield_function_t())
      + ",\"inner\":"           + fc::json::to_string(v_inner, fc::json::yield_function_t())
      + ",\"nums\":"            + fc::json::to_string(v_nums,  fc::json::yield_function_t())
      + "}";
   BOOST_CHECK_EQUAL(out, expected);
}

// Parity helper: streaming JSON for `v` must equal the variant-built JSON.
// Catches divergence between to_json_stream(T) and to_variant(T)+json::to_string
// for libfc leaf types whose HTTP-API consumers depend on the existing shape.
template<typename T>
static void check_streaming_matches_variant(const T& v, const std::string& label) {
   const std::string streamed = fc::to_json_string(v);
   const std::string via_var  = fc::json::to_string(fc::variant(v), fc::json::yield_function_t());
   BOOST_CHECK_MESSAGE(streamed == via_var,
                       label << ": streaming JSON differs from variant path\n  streaming: " << streamed
                       << "\n  variant:   " << via_var);
}

BOOST_AUTO_TEST_CASE(streaming_vs_variant_parity_libfc_leaf_types) {
   // exception: shape comes from FC_REFLECT(fc::exception, ...) -- stack of log_messages.
   {
      fc::exception e(FC_LOG_MESSAGE(error, "test exception"),
                      fc::std_exception_code, "test_exception", "test what");
      check_streaming_matches_variant(e, "fc::exception");
   }
   // log_message: timestamp + context + formatted message.
   {
      fc::log_message lm(FC_LOG_CONTEXT(warn), std::string("hello world"));
      check_streaming_matches_variant(lm, "fc::log_message");
   }
   // 128-bit integers: emitted as decimal strings.
   {
      check_streaming_matches_variant(fc::int128_t{-1234567890123456789LL}, "fc::int128 negative");
      check_streaming_matches_variant(fc::uint128_t{0xffffffffffffffffULL} * 2 + 1, "fc::uint128 large");
   }
   // double / float: variant emits a JSON-quoted fixed-precision string
   // (digits10 + 2, std::fixed) so wire-format clients see a stable shape
   // regardless of magnitude.  Reflector-driven struct fields (e.g.
   // get_producers_result.total_producer_vote_weight) depend on this.
   {
      check_streaming_matches_variant(double{1.5},                      "double 1.5");
      check_streaming_matches_variant(double{-2.25},                    "double -2.25");
      check_streaming_matches_variant(double{0.1},                      "double 0.1 (round-trip)");
      check_streaming_matches_variant(double{1e10},                     "double 1e10");
      check_streaming_matches_variant(double{0.0},                      "double 0.0");
      check_streaming_matches_variant(float{3.5f},                      "float 3.5");
   }
   // bls public_key + signature: derived from a deterministic seed via private_key::generate().
   // private_key itself has =delete'd to_json_stream so we don't include it here.
   {
      auto sk = fc::crypto::bls::private_key::generate();
      check_streaming_matches_variant(sk.get_public_key(), "fc::crypto::bls::public_key");
      const std::string msg = "ping";
      auto sig = sk.sign_raw(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(msg.data()), msg.size()));
      check_streaming_matches_variant(sig, "fc::crypto::bls::signature");
   }
   // url: opaque string round-trip.
   {
      check_streaming_matches_variant(fc::url(std::string{"https://example.com/path?q=1"}), "fc::url");
   }
   // bitset: '1'/'0' bit string via FC_SERIALIZE_AS_STRING.
   {
      check_streaming_matches_variant(fc::bitset(std::string_view{"1010110010"}), "fc::bitset");
   }
   // enum_type<>: reflected enum should emit the member-name string, not the underlying integer.
   {
      using et = fc::enum_type<uint8_t, color_t>;
      check_streaming_matches_variant(et{color_t::green}, "fc::enum_type<color_t>");
   }
   // static_variant (std::variant alias): index + tagged value pair.
   {
      using sv_t = std::variant<int32_t, std::string>;
      check_streaming_matches_variant(sv_t{int32_t{42}}, "static_variant alt 0");
      check_streaming_matches_variant(sv_t{std::string{"hi"}}, "static_variant alt 1");
   }
   // vector<char>: lowercase hex string.
   {
      std::vector<char> v{0x00, 0x01, char(0xfe), char(0xff)};
      check_streaming_matches_variant(v, "std::vector<char>");
   }
   // blob: base64-encoded bytes.
   {
      fc::blob b{{'a', 'b', 'c'}};
      check_streaming_matches_variant(b, "fc::blob");
   }
}

BOOST_AUTO_TEST_CASE(set_raw_splices_preformatted_fragment) {
   // set_raw is "key + raw_value": for embedding a JSON fragment that was already
   // serialized elsewhere (eg the abi_serializer + json::to_string path).
   std::string out;
   {
      fc::json_writer w(out);
      w.begin_object();
      w.set("name",   std::string{"alice"})
       .set_raw("payload", R"({"a":1,"b":"two"})")
       .set("done",   true);
      w.end_object();
   }
   BOOST_CHECK_EQUAL(out, R"({"name":"alice","payload":{"a":1,"b":"two"},"done":true})");
}

BOOST_AUTO_TEST_SUITE_END()
