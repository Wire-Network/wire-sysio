#include <sysio/chain/name.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/config.hpp>

#include <fc/io/raw.hpp>
#include <fc/variant.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
//  Characterization suite for sysio::chain::name.
//
//  name is consensus-critical: its 64-bit packed encoding must never change.
//  This suite pins the *current* observable behavior (encoding, decoding,
//  comparison, serialization, variant conversion) so a future refactor — e.g.
//  re-expressing name on a shared basic_name<Traits> template — is provably
//  behavior-preserving. Every test here must stay green on the unmodified
//  type before that refactor begins.
//
//  Encoding spec pinned here: 13 base-32 symbols over the charmap
//  ".12345abcdefghijklmnopqrstuvwxyz" (charmap index == symbol value).
//  Symbols 0..11 occupy 5 bits each, MSB-first (symbol 0 in bits [59,64));
//  symbol 12 occupies the low 4 bits. Hand-verified golden values below.
//
//  prefix()/suffix() coverage (name_suffix_tests, name_suffix_additional_tests,
//  name_prefix_tests) was moved here from misc_tests.cpp so all name coverage
//  lives in one file.
// ---------------------------------------------------------------------------

using namespace sysio::chain;

// --- compile-time behavior (constexpr paths: literal, string_to_name, raw) --
static_assert(name{}.to_uint64_t() == 0,                              "default name is 0");
static_assert("sysio"_n.to_uint64_t() == 0xC7B0EA0000000000ull,        "sysio literal encoding");
static_assert(string_to_name("sysio").to_uint64_t() == 0xC7B0EA0000000000ull, "string_to_name matches");
static_assert(name{uint64_t{0xC7B0EA0000000000ull}} == "sysio"_n,      "uint64 ctor round-trips literal");
static_assert("a"_n.to_uint64_t() == (6ull << 59),                     "single symbol 'a' lands MSB-first");
static_assert("1"_n.to_uint64_t() == (1ull << 59),                     "single symbol '1'");
static_assert("z"_n.to_uint64_t() == (31ull << 59),                    "single symbol 'z'");
static_assert("a"_n < "b"_n,                                           "ordering follows symbol value");
static_assert(""_n.empty() && !""_n.good(),                            "empty literal");

BOOST_AUTO_TEST_SUITE(name_tests)

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(construction_and_accessors) {
   name def;
   BOOST_CHECK_EQUAL(def.to_uint64_t(), 0u);
   BOOST_CHECK(def.empty());
   BOOST_CHECK(!def.good());
   BOOST_CHECK(!static_cast<bool>(def));
   BOOST_CHECK_EQUAL(def, name{});
   BOOST_CHECK_EQUAL(def, name{""});

   name s{"sysio"};
   BOOST_CHECK_EQUAL(s.to_uint64_t(), 0xC7B0EA0000000000ull);
   BOOST_CHECK(!s.empty());
   BOOST_CHECK(s.good());
   BOOST_CHECK(static_cast<bool>(s));

   // uint64 ctor is a pure passthrough (the deserialization path)
   BOOST_CHECK_EQUAL(name{uint64_t{0xC7B0EA0000000000ull}}, s);
   BOOST_CHECK_EQUAL(name{uint64_t{0xFFFFFFFFFFFFFFFFull}}.to_uint64_t(), 0xFFFFFFFFFFFFFFFFull);
   BOOST_CHECK_EQUAL(name{uint64_t{0u}}, name{});
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(encoding_golden_values) {
   BOOST_CHECK_EQUAL(name{""}.to_uint64_t(),      0u);
   BOOST_CHECK_EQUAL(name{"a"}.to_uint64_t(),     6ull  << 59);
   BOOST_CHECK_EQUAL(name{"z"}.to_uint64_t(),     31ull << 59);
   BOOST_CHECK_EQUAL(name{"1"}.to_uint64_t(),     1ull  << 59);
   BOOST_CHECK_EQUAL(name{"5"}.to_uint64_t(),     5ull  << 59);
   BOOST_CHECK_EQUAL(name{"ab"}.to_uint64_t(),    0x31C0000000000000ull);
   BOOST_CHECK_EQUAL(name{"sysio"}.to_uint64_t(), 0xC7B0EA0000000000ull);

   // the 13th symbol occupies only the low 4 bits
   BOOST_CHECK_EQUAL(name{std::string(12, '.') + "1"}.to_uint64_t(), 1u);
   BOOST_CHECK_EQUAL(name{std::string(12, '.') + "5"}.to_uint64_t(), 5u);
   BOOST_CHECK_EQUAL(name{std::string(12, '.') + "j"}.to_uint64_t(), 15u); // 'j' == 15, the max 13th symbol

   // all-bits-set decodes to the maximal encodable name and round-trips
   BOOST_CHECK_EQUAL(name{"zzzzzzzzzzzzj"}.to_uint64_t(), 0xFFFFFFFFFFFFFFFFull);
}

// --------------------------------------------------------------------------
// the three string->name paths (validating ctor, string_to_name, literal)
// must agree on valid input
BOOST_AUTO_TEST_CASE(encoding_paths_consistent) {
   const std::vector<std::string> valid = {
      "", "a", "z", "15", "sysio", "sysio.token", "a.b.c",
      ".leading", "zzzzzzzzzzzz", "abcdefghijklj"
   };
   for (const auto& s : valid) {
      BOOST_CHECK_EQUAL(name{s}, string_to_name(s));
   }
   BOOST_CHECK_EQUAL("sysio"_n,        name{"sysio"});
   BOOST_CHECK_EQUAL("sysio.token"_n,  name{"sysio.token"});
   BOOST_CHECK_EQUAL(""_n,             name{});
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(to_string_roundtrip) {
   const std::vector<std::string> canonical = {
      "", "a", "z", "15", "sysio", "sysio.token", "a.b.c",
      ".leading", "zzzzzzzzzzzz", "abcdefghijklj"
   };
   for (const auto& s : canonical) {
      BOOST_CHECK_EQUAL(name{s}.to_string(), s);
      BOOST_CHECK_EQUAL(name{name{s}.to_uint64_t()}, name{s}); // uint64 round-trip
   }
   // decode of arbitrary uint64 is total (never throws) and pinned
   BOOST_CHECK_EQUAL(name{uint64_t{0u}}.to_string(), "");
   BOOST_CHECK_EQUAL(name{uint64_t{0xC7B0EA0000000000ull}}.to_string(), "sysio");
   BOOST_CHECK_EQUAL(name{uint64_t{0xFFFFFFFFFFFFFFFFull}}.to_string(), "zzzzzzzzzzzzj");
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(comparisons) {
   const name a{"a"}, b{"b"}, a2{"a"};
   BOOST_CHECK(a == a2);
   BOOST_CHECK(a != b);
   BOOST_CHECK(a <  b);
   BOOST_CHECK(b >  a);
   BOOST_CHECK(a <= a2);
   BOOST_CHECK(a >= a2);
   BOOST_CHECK(!(a > b));

   // comparison against raw uint64
   BOOST_CHECK(name{"sysio"} == 0xC7B0EA0000000000ull);
   BOOST_CHECK(name{"sysio"} != 0ull);
   BOOST_CHECK(name{} == 0ull);
}

// --------------------------------------------------------------------------
// MSB-first packing means numeric order == charmap (string) order
BOOST_AUTO_TEST_CASE(ordering_is_msb_first) {
   const std::vector<name> ascending = {
      name{""}, name{"1"}, name{"5"}, name{"a"}, name{"ab"}, name{"b"},
      name{"z"}, name{"zzzzzzzzzzzz"}, name{"zzzzzzzzzzzzj"}
   };
   for (size_t i = 1; i < ascending.size(); ++i) {
      BOOST_CHECK_LT(ascending[i - 1].to_uint64_t(), ascending[i].to_uint64_t());
      BOOST_CHECK(ascending[i - 1] < ascending[i]);
   }
}

// --------------------------------------------------------------------------
// the validating string ctor (set()) rejects non-canonical / invalid input
BOOST_AUTO_TEST_CASE(validating_ctor_throws) {
   BOOST_CHECK_THROW(name{"."},             name_type_exception); // normalizes to ""
   BOOST_CHECK_THROW(name{"abc."},          name_type_exception); // trailing dot trimmed away
   BOOST_CHECK_THROW(name{"Invalid"},       name_type_exception); // uppercase not in charmap
   BOOST_CHECK_THROW(name{"hello world"},   name_type_exception); // space not in charmap
   BOOST_CHECK_THROW(name{"6789"},          name_type_exception); // digits 6-9 not in charmap
   BOOST_CHECK_THROW(name{"abcdefghijklm"}, name_type_exception); // 13th symbol 'm' > 'j'
   BOOST_CHECK_THROW(name{std::string(12, '.') + "k"}, name_type_exception); // 13th symbol 'k' > 'j'
   BOOST_CHECK_THROW(name{"aaaaaaaaaaaaaa"}, name_type_exception); // 14 chars, too long

   BOOST_CHECK_NO_THROW(name{"abcdefghijklj"});  // exactly 13, 13th symbol == 'j'
   BOOST_CHECK_NO_THROW(name{""});
}

// --------------------------------------------------------------------------
// string_to_name is the non-validating path: invalid chars encode as symbol 0
// without throwing (relied upon by producer_plugin)
BOOST_AUTO_TEST_CASE(string_to_name_is_silent) {
   BOOST_CHECK_NO_THROW(string_to_name("Invalid"));
   BOOST_CHECK_NO_THROW(string_to_name("hello world"));
   BOOST_CHECK_EQUAL(string_to_name("!@#$%"), name{});             // all invalid -> 0
   BOOST_CHECK_EQUAL(string_to_name("Invalid"), string_to_name(".nvalid")); // 'I' -> symbol 0 == '.'
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(fc_raw_serialization) {
   const std::vector<name> samples = {
      name{}, name{"a"}, name{"sysio"}, name{"sysio.token"}, name{"zzzzzzzzzzzzj"}
   };
   for (const auto& n : samples) {
      const std::vector<char> packed = fc::raw::pack(n);
      BOOST_CHECK_EQUAL(packed.size(), 8u);                        // a bare uint64
      BOOST_CHECK_EQUAL(fc::raw::unpack<name>(packed), n);         // round-trip
   }
   // packed bytes are the value, little-endian
   const std::vector<char> packed = fc::raw::pack(name{"sysio"});
   uint64_t reassembled = 0;
   for (int i = 7; i >= 0; --i)
      reassembled = (reassembled << 8) | static_cast<uint8_t>(packed[i]);
   BOOST_CHECK_EQUAL(reassembled, 0xC7B0EA0000000000ull);
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(variant_conversion) {
   fc::variant v;
   fc::to_variant(name{"sysio.token"}, v);
   BOOST_CHECK_EQUAL(v.as_string(), "sysio.token");

   name back;
   fc::from_variant(fc::variant("sysio.token"), back);
   BOOST_CHECK_EQUAL(back, name{"sysio.token"});

   // from_variant goes through the validating set(), so bad input throws
   name dummy;
   BOOST_CHECK_THROW(fc::from_variant(fc::variant("Invalid"), dummy), name_type_exception);
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(std_hash) {
   std::hash<name> h;
   BOOST_CHECK_EQUAL(h(name{"sysio"}), __builtin_bswap64(0xC7B0EA0000000000ull));
   BOOST_CHECK_NE(h(name{"sysio"}), h(name{"token"}));

   std::unordered_map<name, int> m;
   m[name{"alice"}]   = 1;
   m[name{"bob"}]     = 2;
   m[name{"alice"}]   = 3; // overwrite
   BOOST_CHECK_EQUAL(m.size(), 2u);
   BOOST_CHECK_EQUAL(m[name{"alice"}], 3);
   BOOST_CHECK_EQUAL(m[name{"bob"}],   2);
}

// --------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ostream_operator) {
   std::ostringstream os;
   os << name{"sysio.token"};
   BOOST_CHECK_EQUAL(os.str(), "sysio.token");

   std::ostringstream empty;
   empty << name{};
   BOOST_CHECK_EQUAL(empty.str(), "");
}

// --------------------------------------------------------------------------
// suffix() — moved verbatim from misc_tests.cpp
BOOST_AUTO_TEST_CASE(name_suffix_tests)
{
   BOOST_CHECK_EQUAL( name(0).suffix(), name{0} );
   BOOST_CHECK_EQUAL( name{"sysio"_n}.suffix(), name{"sysio"_n} );
   BOOST_CHECK_EQUAL( name{"sysio.any"_n}.suffix(), name{"any"_n} );
   BOOST_CHECK_EQUAL( name{"abcdehijklmn"_n}.suffix(), name{"abcdehijklmn"_n} );
   BOOST_CHECK_EQUAL( name{"abcdehijklmn1"_n}.suffix(), name{"abcdehijklmn1"_n} );
   BOOST_CHECK_EQUAL( name{"abc.def"_n}.suffix(), name{"def"_n} );
   BOOST_CHECK_EQUAL( name{".abc.def"_n}.suffix(), name{"def"_n} );
   BOOST_CHECK_EQUAL( name{"..abc.def"_n}.suffix(), name{"def"_n} );
   BOOST_CHECK_EQUAL( name{"abc..def"_n}.suffix(), name{"def"_n} );
   BOOST_CHECK_EQUAL( name{"abc.def.ghi"_n}.suffix(), name{"ghi"_n} );
   BOOST_CHECK_EQUAL( name{".abcdefghij"_n}.suffix(), name{"abcdefghij"_n} );
   BOOST_CHECK_EQUAL( name{".abcdefghij.1"_n}.suffix(), name{"1"_n} );
   BOOST_CHECK_EQUAL( name{"a.bcdefghij"_n}.suffix(), name{"bcdefghij"_n} );
   BOOST_CHECK_EQUAL( name{"a.bcdefghij.1"_n}.suffix(), name{"1"_n} );
   BOOST_CHECK_EQUAL( name{"......a.b.c"_n}.suffix(), name{"c"_n} );
   BOOST_CHECK_EQUAL( name{"abcdefhi.123"_n}.suffix(), name{"123"_n} );
   BOOST_CHECK_EQUAL( name{"abcdefhij.123"_n}.suffix(), name{"123"_n} );
}

BOOST_AUTO_TEST_CASE(name_suffix_additional_tests) {
   // ----------------------------
   // constexpr name suffix()const
   BOOST_CHECK_EQUAL( name{".sysioaccounj"}.suffix(), name{"sysioaccounj"} );
   BOOST_CHECK_EQUAL( name{"s.osioaccounj"}.suffix(), name{"osioaccounj"} );
   BOOST_CHECK_EQUAL( name{"sy.sioaccounj"}.suffix(), name{"sioaccounj"} );
   BOOST_CHECK_EQUAL( name{"sys.ioaccounj"}.suffix(), name{"ioaccounj"} );
   BOOST_CHECK_EQUAL( name{"sysi.oaccounj"}.suffix(), name{"oaccounj"} );
   BOOST_CHECK_EQUAL( name{"sysio.accounj"}.suffix(), name{"accounj"} );
   BOOST_CHECK_EQUAL( name{"sysioa.ccounj"}.suffix(), name{"ccounj"} );
   BOOST_CHECK_EQUAL( name{"sysioac.counj"}.suffix(), name{"counj"} );
   BOOST_CHECK_EQUAL( name{"sysioacc.ounj"}.suffix(), name{"ounj"} );
   BOOST_CHECK_EQUAL( name{"sysioacco.unj"}.suffix(), name{"unj"} );
   BOOST_CHECK_EQUAL( name{"sysioaccou.nj"}.suffix(), name{"nj"} );
   BOOST_CHECK_EQUAL( name{"sysioaccoun.j"}.suffix(), name{"j"} );
   BOOST_CHECK_EQUAL( name{"sysioaccounja"}.suffix(), name{"sysioaccounja"} );
   BOOST_CHECK_EQUAL( name{"sysioaccounj"}.suffix(),  name{"sysioaccounj"} );

   BOOST_CHECK_EQUAL( name{"s.y.s.i.o.a.c"}.suffix(), name{"c"} );
   BOOST_CHECK_EQUAL( name{"sys.ioa.cco"}.suffix(), name{"cco"} );
}

BOOST_AUTO_TEST_CASE(name_prefix_tests)
{
   BOOST_CHECK_EQUAL("e"_n.prefix(), "e"_n);
   BOOST_CHECK_EQUAL(""_n.prefix(), ""_n);
   BOOST_CHECK_EQUAL("abcdefghijklm"_n.prefix(), "abcdefghijklm"_n);
   BOOST_CHECK_EQUAL("abcdefghijkl"_n.prefix(), "abcdefghijkl"_n);
   BOOST_CHECK_EQUAL("abc.xyz"_n.prefix(), "abc"_n);
   BOOST_CHECK_EQUAL("abc.xyz.qrt"_n.prefix(), "abc.xyz"_n);
   BOOST_CHECK_EQUAL("."_n.prefix(), ""_n);

   BOOST_CHECK_EQUAL("sysio.any"_n.prefix(), "sysio"_n);
   BOOST_CHECK_EQUAL("sysio"_n.prefix(), "sysio"_n);
   BOOST_CHECK_EQUAL("sysio"_n.prefix(), config::system_account_name);
   BOOST_CHECK_EQUAL("sysio."_n.prefix(), "sysio"_n);
   BOOST_CHECK_EQUAL("sysio.evm"_n.prefix(), "sysio"_n);
   BOOST_CHECK_EQUAL(".sysio"_n.prefix(), ""_n);
   BOOST_CHECK_NE("sysi"_n.prefix(), "sysio"_n);
   BOOST_CHECK_NE("sysiosysio"_n.prefix(), "sysio"_n);
   BOOST_CHECK_NE("sysioe"_n.prefix(), "sysio"_n);
}

BOOST_AUTO_TEST_SUITE_END()
