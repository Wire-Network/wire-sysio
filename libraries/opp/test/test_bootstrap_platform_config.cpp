/**
 * @file test_bootstrap_platform_config.cpp
 * @brief Schema + invariant tests for the launch-day platform bootstrap config
 *        (`sysio.opp.bootstrap.BootstrapPlatformConfig`).
 *
 * Proves the committed example configs under `etc/config/dex/` parse against
 * the generated protobuf schema and satisfy the cross-field invariants the
 * launch bootstrap tool relies on. The same validator is reproduced here so a
 * malformed config is caught in CI rather than at an irreversible bootstrap
 * step. Parsing is STRICT — unknown / misspelled JSON keys are rejected, not
 * silently dropped — because these files are hand-authored.
 *
 * `OPP_DEX_CONFIG_DIR` is injected by CMake (absolute path to the source
 * `etc/config/dex` directory).
 */

#include <boost/test/unit_test.hpp>

#include <google/protobuf/util/json_util.h>
#include <sysio/opp/bootstrap/bootstrap.pb.h>

#include <fc/slug_name.hpp>
#include <fc/crypto/base58.hpp>

#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace gpb = google::protobuf;
using sysio::opp::bootstrap::BootstrapPlatformConfig;
using sysio::opp::types::ChainKind;
using sysio::opp::types::TokenKind;

namespace {

/// Read an entire file into a string. Fails the test if the file is absent.
std::string slurp(const std::string& path) {
   std::ifstream in(path, std::ios::binary);
   BOOST_REQUIRE_MESSAGE(in.good(), "cannot open " + path);
   std::ostringstream ss;
   ss << in.rdbuf();
   return ss.str();
}

/// Parse JSON into `out` with STRICT semantics (unknown fields rejected).
/// Returns false and fills `err` on any parse error.
bool parse_strict(const std::string& json, BootstrapPlatformConfig& out, std::string& err) {
   gpb::util::JsonParseOptions opts; // ignore_unknown_fields defaults to false
   const auto st = gpb::util::JsonStringToMessage(json, &out, opts);
   if (!st.ok()) {
      err = std::string(st.message());
      return false;
   }
   return true;
}

/// True iff `s` is a valid slug_name (alphabet [A-Z0-9_], 1..8 chars). Uses the
/// real `fc::slug_name`, which throws on an out-of-alphabet or over-length code.
bool slug_ok(const std::string& s) {
   if (s.empty()) return false;
   try {
      (void) fc::slug_name{std::string_view{s}};
      return true;
   } catch (...) {
      return false;
   }
}

/// True iff `s` is a 0x-prefixed 20-byte hex string (EVM contract address).
bool evm_addr_ok(const std::string& s) {
   if (s.size() != 42 || s[0] != '0' || s[1] != 'x') return false;
   for (size_t i = 2; i < s.size(); ++i)
      if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
   return true;
}

/// True iff `s` base58-decodes to exactly 32 bytes (SVM mint / address).
bool svm_addr_ok(const std::string& s) {
   try {
      return fc::from_base58(s).size() == 32;
   } catch (...) {
      return false;
   }
}

/// Lightweight Antelope account-name check for the private-reserve `owner`.
/// The contract is the authority; this catches gross authoring mistakes
/// (charset `.a-z1-5`, non-empty, <= 13 chars).
bool account_name_ok(const std::string& s) {
   if (s.empty() || s.size() > 13) return false;
   for (char c : s) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '1' && c <= '5') || c == '.';
      if (!ok) return false;
   }
   return true;
}

/// Validate a parsed config against the launch invariants V1..V9. Returns a
/// list of human-readable failures (empty == valid).
std::vector<std::string> validate(const BootstrapPlatformConfig& c) {
   std::vector<std::string> e;

   // V1 — version + label
   if (c.schema_version() != 1) e.push_back("V1 schema_version != 1");
   if (c.network().empty())     e.push_back("V1 network empty");

   // chains: V2 slug, V3 uniqueness + exactly-one-depot
   std::map<std::string, ChainKind> chain_kind;
   std::set<std::string>            chain_codes;
   int wire_chains = 0;
   for (const auto& ch : c.chains()) {
      if (!slug_ok(ch.code()))                  e.push_back("V2 chain code: " + ch.code());
      if (!chain_codes.insert(ch.code()).second) e.push_back("V3 duplicate chain: " + ch.code());
      chain_kind[ch.code()] = ch.kind();
      if (ch.kind() == ChainKind::CHAIN_KIND_WIRE) {
         ++wire_chains;
         if (ch.code() != "WIRE") e.push_back("V3 depot chain code must be WIRE");
      }
   }
   if (wire_chains != 1) e.push_back("V3 expected exactly one CHAIN_KIND_WIRE chain");

   // tokens: V2 slug, V4 binding + precision + address well-formedness
   std::set<std::string>                       token_codes;
   std::map<std::string, int>                  native_per_chain;
   std::set<std::pair<std::string, std::string>> bindings;
   for (const auto& t : c.tokens()) {
      if (!slug_ok(t.code()))                    e.push_back("V2 token code: " + t.code());
      if (!token_codes.insert(t.code()).second)  e.push_back("V4 duplicate token: " + t.code());
      const auto it = chain_kind.find(t.chain_code());
      if (it == chain_kind.end()) {
         e.push_back("V4 token " + t.code() + " references undeclared chain " + t.chain_code());
         continue;
      }
      if (t.precision() < 1 || t.precision() > 18) e.push_back("V4 token precision: " + t.code());
      bindings.insert({t.chain_code(), t.code()});
      if (t.is_native()) {
         ++native_per_chain[t.chain_code()];
         if (t.kind() != TokenKind::TOKEN_KIND_NATIVE || !t.contract_address().empty())
            e.push_back("V4 native token must be TOKEN_KIND_NATIVE with empty address: " + t.code());
      } else {
         bool ok = false;
         if (it->second == ChainKind::CHAIN_KIND_EVM)      ok = evm_addr_ok(t.contract_address());
         else if (it->second == ChainKind::CHAIN_KIND_SVM) ok = svm_addr_ok(t.contract_address());
         if (!ok) e.push_back("V4 malformed address for token: " + t.code());
      }
   }

   // V5 — exactly one native token per non-depot chain
   for (const auto& ch : c.chains())
      if (ch.kind() != ChainKind::CHAIN_KIND_WIRE && native_per_chain[ch.code()] != 1)
         e.push_back("V5 expected exactly one native token on chain " + ch.code());

   // reserves: V6 uniqueness/binding/weight/amount, V7 earmark, V8 owner
   std::set<std::tuple<std::string, std::string, std::string>> triples;
   unsigned __int128 sum_wire = 0;
   for (const auto& r : c.reserves()) {
      const auto key = std::make_tuple(r.chain_code(), r.token_code(), r.code());
      if (!triples.insert(key).second)
         e.push_back("V6 duplicate reserve: " + r.chain_code() + "/" + r.token_code() + "/" + r.code());
      if (!bindings.count({r.chain_code(), r.token_code()}))
         e.push_back("V6 reserve references undeclared binding: " + r.chain_code() + "/" + r.token_code());
      const auto it = chain_kind.find(r.chain_code());
      if (it != chain_kind.end() && it->second == ChainKind::CHAIN_KIND_WIRE)
         e.push_back("V6 reserve cannot live on the depot chain");
      if (!(r.connector_weight_bps() > 0 && r.connector_weight_bps() <= 10000))
         e.push_back("V6 connector_weight_bps out of range");
      if (r.initial_chain_amount() == 0 || r.initial_wire_amount() == 0)
         e.push_back("V6 reserve amounts must be > 0");
      sum_wire += r.initial_wire_amount();
      if (r.is_private() && !account_name_ok(r.owner()))
         e.push_back("V8 private reserve needs a valid owner account name");
      if (!r.is_private() && !r.owner().empty())
         e.push_back("V8 public reserve must not name an owner");
   }

   // V7 — WIRE earmark covers the reserve WIRE sides
   if (c.t5_reserve_allocation() == 0) e.push_back("V7 t5_reserve_allocation must be > 0");
   if (sum_wire > static_cast<unsigned __int128>(c.t5_reserve_allocation()))
      e.push_back("V7 sum(initial_wire_amount) exceeds t5_reserve_allocation");

   // V9 — uwrit config
   if (!c.has_uwrit()) {
      e.push_back("V9 uwrit config missing");
   } else {
      const auto& u = c.uwrit();
      // fee_bps <= 9999: a 10000 (100%) fee zeroes the post-fee WIRE leg and is
      // rejected on-chain by sysio.uwrit::setconfig (MAX_FEE_BPS).
      if (u.fee_bps() > 9999)                        e.push_back("V9 fee_bps > 9999 (100%)");
      if (u.collateral_lock_duration_ms() == 0)      e.push_back("V9 collateral_lock_duration_ms must be > 0");
   }

   return e;
}

/// Absolute path to the source `etc/config/dex` directory (CMake-injected).
const std::string CONFIG_DIR = OPP_DEX_CONFIG_DIR;

} // namespace

BOOST_AUTO_TEST_SUITE(bootstrap_platform_config)

/// The launch example parses strictly and satisfies every invariant.
BOOST_AUTO_TEST_CASE(launch_example_parses_and_validates) {
   BootstrapPlatformConfig cfg;
   std::string err;
   BOOST_REQUIRE_MESSAGE(parse_strict(slurp(CONFIG_DIR + "/dex-config.launch.example.json"), cfg, err), err);
   for (const auto& v : validate(cfg)) BOOST_ERROR(v);
   BOOST_CHECK_EQUAL(cfg.chains_size(), 3);
   BOOST_CHECK_EQUAL(cfg.reserves_size(), 4);
}

/// The dev-cluster mirror parses strictly and satisfies every invariant.
BOOST_AUTO_TEST_CASE(dev_config_parses_and_validates) {
   BootstrapPlatformConfig cfg;
   std::string err;
   BOOST_REQUIRE_MESSAGE(parse_strict(slurp(CONFIG_DIR + "/dex-config.dev.json"), cfg, err), err);
   for (const auto& v : validate(cfg)) BOOST_ERROR(v);
   BOOST_CHECK_EQUAL(cfg.tokens_size(), 9);
   BOOST_CHECK_EQUAL(cfg.reserves_size(), 8);
}

/// A typo'd / unknown JSON key must fail the strict parse, not be dropped.
BOOST_AUTO_TEST_CASE(strict_parse_rejects_unknown_field) {
   BootstrapPlatformConfig cfg;
   std::string err;
   const std::string bad = R"({"schema_version":1,"network":"x","totally_unknown_key":3})";
   BOOST_CHECK(!parse_strict(bad, cfg, err));
}

/// Each single-field mutation of the valid dev config trips at least one
/// invariant (one mutation per targeted check).
BOOST_AUTO_TEST_CASE(validator_rejects_mutations) {
   BootstrapPlatformConfig base;
   std::string err;
   BOOST_REQUIRE_MESSAGE(parse_strict(slurp(CONFIG_DIR + "/dex-config.dev.json"), base, err), err);
   BOOST_REQUIRE(validate(base).empty());

   { auto c = base; c.mutable_chains(1)->set_code("TOOLONG99");         // 9 chars > 8
     BOOST_CHECK(!validate(c).empty()); }                               // V2 over-length slug
   { auto c = base; c.mutable_chains(2)->set_kind(ChainKind::CHAIN_KIND_WIRE);
     BOOST_CHECK(!validate(c).empty()); }                               // V3 two depots
   { auto c = base;                                                     // V5 second native on ETHEREUM
     auto* tok = c.mutable_tokens(2);                                   // LIQETH on ETHEREUM
     tok->set_is_native(true);
     tok->set_kind(TokenKind::TOKEN_KIND_NATIVE);
     tok->clear_contract_address();
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base; c.mutable_reserves(0)->set_connector_weight_bps(10001);
     BOOST_CHECK(!validate(c).empty()); }                               // V6 weight out of range
   { auto c = base; c.set_t5_reserve_allocation(1);
     BOOST_CHECK(!validate(c).empty()); }                               // V7 earmark too small
   { auto c = base; c.mutable_reserves(0)->set_is_private(true);        // V8 private without owner
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base; c.mutable_uwrit()->set_fee_bps(10000);
     BOOST_CHECK(!validate(c).empty()); }                               // V9 fee_bps 10000 rejected (100% zeroes post-fee WIRE)
}

/// The reserved `t5_dex_allocation` earmark is part of the strict schema and is
/// inert today: it defaults to 0, a non-zero value trips no launch invariant
/// (the on-chain DEX-seeding mechanism is not finalized), and strict parsing
/// accepts the key (an unknown key would be rejected — see the test above).
BOOST_AUTO_TEST_CASE(t5_dex_allocation_is_accepted_and_inert) {
   BootstrapPlatformConfig base;
   std::string err;
   BOOST_REQUIRE_MESSAGE(parse_strict(slurp(CONFIG_DIR + "/dex-config.dev.json"), base, err), err);
   BOOST_REQUIRE(validate(base).empty());
   BOOST_CHECK_EQUAL(base.t5_dex_allocation(), 0u);          // default: disabled

   auto c = base;
   c.set_t5_dex_allocation(1'000'000'000ull);                // non-zero earmark
   BOOST_CHECK(validate(c).empty());                         // reserved: trips nothing

   BootstrapPlatformConfig parsed;
   std::string perr;
   BOOST_CHECK(parse_strict(
      R"({"schema_version":1,"network":"x","t5_dex_allocation":"5"})", parsed, perr));
   BOOST_CHECK_EQUAL(parsed.t5_dex_allocation(), 5u);        // strict schema knows the key
}

BOOST_AUTO_TEST_SUITE_END()
