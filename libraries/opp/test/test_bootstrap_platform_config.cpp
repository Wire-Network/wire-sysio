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

/// True iff `s` is a 0x-prefixed 33-byte hex string whose first byte is 02 or 03
/// (a compressed secp256k1 point, the EVM syndication identity).
bool evm_pubkey_ok(const std::string& s) {
   if (s.size() != 68 || s[0] != '0' || s[1] != 'x') return false;
   if (!(s[2] == '0' && (s[3] == '2' || s[3] == '3'))) return false;
   for (size_t i = 2; i < s.size(); ++i)
      if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
   return true;
}

/// True iff `s` is a symbol code the swap accepts for a pair token: 1..7 upper-case letters.
bool pair_symbol_ok(const std::string& s) {
   if (s.empty() || s.size() > 7) return false;
   for (char c : s)
      if (c < 'A' || c > 'Z') return false;
   return true;
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

/// Validate a parsed config against the launch invariants V1..V13. Returns a
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
   std::set<std::pair<std::string, std::string>> liq_bindings;   // the TOKEN_KIND_LIQ subset
   for (const auto& t : c.tokens()) {
      if (!slug_ok(t.code()))                    e.push_back("V2 token code: " + t.code());
      if (!token_codes.insert(t.code()).second)  e.push_back("V4 duplicate token: " + t.code());
      const auto it = chain_kind.find(t.chain_code());
      if (it == chain_kind.end()) {
         e.push_back("V4 token " + t.code() + " references undeclared chain " + t.chain_code());
         continue;
      }
      if (t.kind() == TokenKind::TOKEN_KIND_LIQ) liq_bindings.insert({t.chain_code(), t.code()});
      // 1..9, NOT 1..18: `TokenSpec.precision` is the DEPOT-FRAME precision, and
      // `sysio.tokens::regtoken` rejects anything above MAX_TOKEN_PRECISION (9).
      // Accepting 10..18 here let a config pass strict validation and then throw
      // partway through the IRREVERSIBLE bootstrap actions. A token whose native
      // precision exceeds the frame (ETH at 18) declares min(native, 9) and is
      // downscaled at the outpost boundary.
      if (t.precision() < 1 || t.precision() > 9) e.push_back("V4 token precision: " + t.code());
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
      // 1..9999 only: 10000 zeroes the token-side weight (dead reserve) and is
      // rejected on-chain by regreserve/oncrtreserve (MAX_CONNECTOR_WEIGHT_BPS).
      if (!(r.connector_weight_bps() > 0 && r.connector_weight_bps() <= 9999))
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

   // liq pools: V11 binding / uniqueness / parameters, V10 earmark
   std::map<std::pair<std::string, std::string>, unsigned __int128> pool_seed;
   unsigned __int128 sum_pool_wire = 0;
   for (const auto& p : c.liq_pools()) {
      const auto binding = std::make_pair(p.chain_code(), p.token_code());
      const auto label   = p.chain_code() + "/" + p.token_code();
      if (!liq_bindings.count(binding))
         e.push_back("V11 liq pool references no declared liq token on its chain: " + label);
      if (!pool_seed.emplace(binding, p.initial_chain_amount()).second)
         e.push_back("V11 duplicate liq pool: " + label);
      if (!pair_symbol_ok(p.pair_symbol()))
         e.push_back("V11 pair_symbol must be 1..7 characters [A-Z]: " + p.pair_symbol());
      if (p.initial_chain_amount() == 0 || p.initial_wire_amount() == 0)
         e.push_back("V11 liq pool seeds must be > 0: " + label);
      // 0..9999: the swap's changefee/inittoken ceiling (MAX_FEE); 10000 is refused on-chain.
      if (p.fee() > 9999)                                      e.push_back("V11 fee > 9999 (100%): " + label);
      if (p.conversion_horizon_sec() == 0)                     e.push_back("V11 conversion_horizon_sec must be > 0: " + label);
      if (p.depth_cap_bps() == 0 || p.depth_cap_bps() > 10000) e.push_back("V11 depth_cap_bps out of 1..10000: " + label);
      if (p.clip_floor() == 0)                                 e.push_back("V11 clip_floor must be > 0: " + label);
      sum_pool_wire += p.initial_wire_amount();
   }

   // V10 — the dex earmark covers the pools' WIRE sides
   if (c.liq_pools_size() > 0 && c.t5_dex_allocation() == 0)
      e.push_back("V10 t5_dex_allocation must be > 0 when liq pools are seeded");
   if (sum_pool_wire > static_cast<unsigned __int128>(c.t5_dex_allocation()))
      e.push_back("V10 sum(liq_pools[].initial_wire_amount) exceeds t5_dex_allocation");

   // syndications: V12 binding / pubkey form / amount
   std::map<std::pair<std::string, std::string>, unsigned __int128> synd_total;
   for (const auto& s : c.syndications()) {
      const auto binding = std::make_pair(s.chain_code(), s.token_code());
      if (!liq_bindings.count(binding)) {
         e.push_back("V12 syndication references no declared liq token on its chain: " + s.chain_code() + "/" + s.token_code());
         continue;
      }
      const ChainKind kind = chain_kind.find(s.chain_code())->second;
      bool ok = false;
      if (kind == ChainKind::CHAIN_KIND_EVM)      ok = evm_pubkey_ok(s.pubkey());
      else if (kind == ChainKind::CHAIN_KIND_SVM) ok = svm_addr_ok(s.pubkey());
      if (!ok)              e.push_back("V12 syndication pubkey does not fit the chain family: " + s.pubkey());
      if (s.amount() == 0)  e.push_back("V12 syndication amount must be > 0: " + s.pubkey());
      synd_total[binding] += s.amount();
   }

   // V13 — a declared custody total is exactly what the depot mints against
   for (const auto& p : c.liq_pools()) {
      if (p.custody_total() == 0) continue;
      const auto binding = std::make_pair(p.chain_code(), p.token_code());
      const unsigned __int128 minted = static_cast<unsigned __int128>(p.initial_chain_amount()) + synd_total[binding];
      if (minted != static_cast<unsigned __int128>(p.custody_total()))
         e.push_back("V13 custody_total != initial_chain_amount + sum(syndications) for " + p.chain_code() + "/" + p.token_code());
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
   BOOST_CHECK_EQUAL(cfg.reserves_size(), 6);      // the two liq tokens are pools, not reserves
   BOOST_CHECK_EQUAL(cfg.liq_pools_size(), 2);
   BOOST_CHECK_EQUAL(cfg.syndications_size(), 3);
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
   // V4 precision is bounded by the DEPOT FRAME (9), not asset's 18. A token
   // declared above the frame is rejected by `sysio.tokens::regtoken`, so
   // accepting it here would pass validation and then abort the IRREVERSIBLE
   // bootstrap. 10 is the first rejected value; 18 (ETH's native precision, which
   // must be declared downscaled as 9) is well past it.
   { auto c = base; c.mutable_tokens(0)->set_precision(10);
     BOOST_CHECK(!validate(c).empty()); }                               // V4 one past the frame
   { auto c = base; c.mutable_tokens(0)->set_precision(18);
     BOOST_CHECK(!validate(c).empty()); }                               // V4 native ETH precision, not downscaled
   { auto c = base; c.mutable_tokens(0)->set_precision(9);
     BOOST_CHECK(validate(c).empty()); }                                // V4 the frame itself stays valid
   { auto c = base; c.mutable_reserves(0)->set_connector_weight_bps(10000);
     BOOST_CHECK(!validate(c).empty()); }                               // V6 weight 10000 rejected (zero token-side weight)
   { auto c = base; c.set_t5_reserve_allocation(1);
     BOOST_CHECK(!validate(c).empty()); }                               // V7 earmark too small
   { auto c = base; c.mutable_reserves(0)->set_is_private(true);        // V8 private without owner
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base; c.mutable_uwrit()->set_fee_bps(10000);
     BOOST_CHECK(!validate(c).empty()); }                               // V9 fee_bps 10000 rejected (100% zeroes post-fee WIRE)
   { auto c = base; c.set_t5_dex_allocation(1);
     BOOST_CHECK(!validate(c).empty()); }                               // V10 dex earmark too small
   { auto c = base; c.mutable_liq_pools(0)->set_token_code("USDC");     // V11 an ERC-20 is not a liq token
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base; *c.add_liq_pools() = c.liq_pools(1);                // V11 one pool per token
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base; c.mutable_liq_pools(0)->set_pair_symbol("TOOLONG9");
     BOOST_CHECK(!validate(c).empty()); }                               // V11 eight characters, a digit
   { auto c = base; c.mutable_liq_pools(0)->set_fee(10000);
     BOOST_CHECK(!validate(c).empty()); }                               // V11 fee 10000 rejected on-chain
   { auto c = base; c.mutable_liq_pools(0)->set_depth_cap_bps(0);
     BOOST_CHECK(!validate(c).empty()); }                               // V11 a zero cap never sells
   { auto c = base; c.mutable_liq_pools(0)->set_clip_floor(0);
     BOOST_CHECK(!validate(c).empty()); }                               // V11 floor
   { auto c = base; c.mutable_liq_pools(0)->set_conversion_horizon_sec(0);
     BOOST_CHECK(!validate(c).empty()); }                               // V11 horizon
   { auto c = base;                                                     // V12 an EVM address is not the pubkey
     c.mutable_syndications(0)->set_pubkey("0x5FbDB2315678afecb367f032d93F642f64180aa3");
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base;                                                     // V12 an EVM pubkey on an SVM token
     c.mutable_syndications(1)->set_pubkey("0x0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
     BOOST_CHECK(!validate(c).empty()); }
   { auto c = base; c.mutable_syndications(1)->set_amount(0);
     BOOST_CHECK(!validate(c).empty()); }                               // V12 amount
   { auto c = base; c.mutable_syndications(1)->set_token_code("USDCSOL");
     BOOST_CHECK(!validate(c).empty()); }                               // V12 not a liq token
   { auto c = base; c.mutable_liq_pools(1)->set_custody_total(1);
     BOOST_CHECK(!validate(c).empty()); }                               // V13 custody does not match
   { auto c = base; c.mutable_liq_pools(1)->set_custody_total(0);
     BOOST_CHECK(validate(c).empty()); }                                // V13 no custody declared: not checked
}

/// The `t5_dex_allocation` earmark backs the liq pools' WIRE sides: required
/// once a pool is declared, free otherwise, and part of the strict schema.
BOOST_AUTO_TEST_CASE(t5_dex_allocation_backs_the_liq_pools) {
   BootstrapPlatformConfig base;
   std::string err;
   BOOST_REQUIRE_MESSAGE(parse_strict(slurp(CONFIG_DIR + "/dex-config.dev.json"), base, err), err);
   BOOST_REQUIRE(validate(base).empty());
   BOOST_CHECK_EQUAL(base.t5_dex_allocation(), 20'000'000'000ull);   // covers both pools exactly

   auto c = base;
   c.set_t5_dex_allocation(0);
   BOOST_CHECK(!validate(c).empty());                        // pools declared: the earmark is required
   c.clear_liq_pools();
   c.clear_syndications();
   BOOST_CHECK(validate(c).empty());                         // no pools: a zero earmark is fine

   BootstrapPlatformConfig parsed;
   std::string perr;
   BOOST_CHECK(parse_strict(
      R"({"schema_version":1,"network":"x","t5_dex_allocation":"5"})", parsed, perr));
   BOOST_CHECK_EQUAL(parsed.t5_dex_allocation(), 5u);        // strict schema knows the key
}

BOOST_AUTO_TEST_SUITE_END()
