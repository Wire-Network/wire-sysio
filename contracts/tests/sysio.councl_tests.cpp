/// Integration tests for sysio.councl — the council election contract.
///
/// The fixture mirrors sysio.dispute_tests.cpp: it bootstraps sysio.roa (node owners / tiers) and
/// sysio.system (nodecount), then deploys the CDT-built sysio.councl artifact.
///
/// The escalation tests are deliberately *seed-agnostic*: instead of predicting which tier-2/3
/// account the entropy accumulator selects, they read `state.proposer` back and drive that account.
/// This keeps the suite stable when the seed formula is tweaked (see DESIGN.md §5, §12).
///
/// Coverage includes bounded candidate-paid registration, staged snapshot loading (including ROA
/// churn and the maximum tier-3 size), strict-priority T1/T2/T3 voting, compact duplicate-vote
/// tracking, timeout boundaries and settlement-only stale actions, deterministic no-repeat
/// selection, governance recovery/backstop, and complete cleanup-separated generations.

#include "contracts.hpp"

#include <boost/test/unit_test.hpp>
#include <fc/variant_object.hpp>
#include <set>
#include <string>
#include <string_view>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>
#include <vector>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;

using mvo = fc::mutable_variant_object;

namespace {

// ABI enum spellings generated from sysio.councl.hpp.
constexpr auto PH_AWAIT_REP = "AWAIT_REP";
constexpr auto PH_VOTING = "VOTING";
constexpr auto PH_BACKSTOP = "BACKSTOP";
constexpr auto PH_DONE = "DONE";
constexpr auto IP_REG = "REG";
constexpr auto IP_LOADING = "LOADING";
constexpr auto IP_READY = "READY";
constexpr auto IP_CLEANING = "CLEANING";
constexpr auto TIER_T1 = "T1";
constexpr auto TIER_T2 = "T2";
constexpr auto TIER_T3 = "T3";

/// Build a valid account name of the form `<prefix><suffix-letter>` (lowercase a-z only), e.g.
/// name_idx("own", 0) == "owna". Good for up to 26 distinct names per prefix.
name name_idx(const std::string& prefix, size_t i) {
   BOOST_REQUIRE_LT(i, 26u);
   return name(prefix + static_cast<char>('a' + i));
}

/// Build one of 31^4 deterministic six-character account names for large-bound tests.
name bulk_name(char prefix, size_t i) {
   static constexpr std::string_view alphabet = "12345abcdefghijklmnopqrstuvwxyz";
   BOOST_REQUIRE_LT(i, alphabet.size() * alphabet.size() * alphabet.size() * alphabet.size());
   std::string value(6, '1');
   value[0] = prefix;
   for (size_t pos = value.size() - 1; pos > 1; --pos) {
      value[pos] = alphabet[i % alphabet.size()];
      i /= alphabet.size();
   }
   return name(value);
}

} // anonymous namespace

class sysio_councl_tester : public tester {
public:
   static constexpr auto COUNCL_ACCOUNT = "sysio.councl"_n;
   static constexpr auto ROA_ACCOUNT = "sysio.roa"_n;
   static constexpr uint64_t GEN0 = 0;
   static constexpr uint64_t TIME_SLOT = 60; // seconds per attempt window

   // 21 tier-1 owners, plus pools of tier-2 / tier-3 owners and candidates.
   std::vector<name> t1_owners;   // exactly 21
   std::vector<name> t2_owners;   // optional
   std::vector<name> t3_owners;   // optional
   std::vector<name> candidates_; // >= 23

   abi_serializer councl_abi, roa_abi, system_abi;

   sysio_councl_tester() {
      produce_blocks(2);

      create_accounts({COUNCL_ACCOUNT});

      // Node-owner + candidate accounts. Created without a roa policy so regnodeowner's reslimit
      // creation does not collide (matches sysio.dispute_tests).
      // The tester genesis (init_roa) already forcereg'd NODE_DADDY at tier 1, and startinit
      // requires roa's tier-1 set to be exactly 21 owners — so nodedaddy takes the first roster
      // slot and only 20 fresh accounts are created/registered here.
      t1_owners.push_back(NODE_DADDY);
      for (size_t i = 0; i < 20; ++i)
         t1_owners.push_back(name_idx("own", i));
      for (const auto& o : t1_owners)
         if (o != NODE_DADDY)
            create_account(o, config::system_account_name, false, true, /*include_roa_policy=*/false);
      for (size_t i = 0; i < 26; ++i)
         candidates_.push_back(name_idx("cnd", i));
      for (const auto& c : candidates_)
         create_account(c, config::system_account_name, false, true, /*include_roa_policy=*/false);
      // Candidate rows are intentionally billed to the self-registering account. The system
      // newaccount path grants only enough RAM for the account object, so give each fixture
      // candidate a small finite allowance that can cover its own council registration row.
      auto& resource_limits = control->get_mutable_resource_limits_manager();
      for (const auto& c : candidates_)
         resource_limits.set_account_limits(c, 4096, -1, -1, false);
      produce_blocks(2);

      // sysio.roa is a genesis system account already running this build's code; just load its abi.
      load_abi(ROA_ACCOUNT, roa_abi);

      // Deploy + init sysio.system (setemitcfg + addnodeowner -> nodecount.t{1,2,3}_count).
      set_code(config::system_account_name, contracts::system_wasm());
      set_abi(config::system_account_name, contracts::system_abi().data());
      produce_blocks();
      load_abi(config::system_account_name, system_abi);
      base_tester::push_action(config::system_account_name, "init"_n, config::system_account_name,
                               mvo()("version", 0)("core", std::string("4,SYS")));
      setup_emission_config();

      // Deploy sysio.councl (privileged: rows bill to the sysio RAM pool).
      set_code(COUNCL_ACCOUNT, contracts::councl_wasm());
      set_abi(COUNCL_ACCOUNT, contracts::councl_abi().data());
      set_privileged(COUNCL_ACCOUNT);
      produce_blocks();
      load_abi(COUNCL_ACCOUNT, councl_abi);
   }

   // ── helpers ──────────────────────────────────────────────────────────────

   void load_abi(name account, abi_serializer& out_ser) {
      const auto* accnt = control->find_account_metadata(account);
      BOOST_REQUIRE(accnt != nullptr);
      abi_def parsed;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, parsed), true);
      out_ser.set_abi(std::move(parsed), abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   void setup_emission_config() {
      auto cfg = mvo()("t1_allocation", int64_t(7500000000000000))("t2_allocation", int64_t(1000000000000000))(
         "t3_allocation", int64_t(100000000000000))("t1_duration", uint32_t(12u * 30u * 24u * 3600u))(
         "t2_duration", uint32_t(24u * 30u * 24u * 3600u))("t3_duration", uint32_t(36u * 30u * 24u * 3600u))(
         "min_claimable", int64_t(10000000000))("t5_distributable", int64_t(375000000000000000LL))(
         "t5_floor", int64_t(125000000000000000LL))("target_annual_decay_bps", uint16_t(6940))(
         "annual_initial_emission", int64_t(563150000000000LL * 365))("annual_max_emission",
                                                                      int64_t(3000000000000000LL * 365))(
         "annual_min_emission", int64_t(100000000000000LL * 365))("compute_bps", uint16_t(4000))(
         "capex_bps", uint16_t(2000))("governance_bps", uint16_t(1000))("producer_bps", uint16_t(7000))(
         "batch_op_bps", uint16_t(3000))("standby_end_rank", uint32_t(28))("epoch_log_retention_count", uint32_t(8640))(
         "pay_cadence_epochs", uint16_t(1));
      push(config::system_account_name, system_abi, config::system_account_name, "setemitcfg"_n, mvo()("cfg", cfg));
      produce_blocks();
   }

   /// Register `owner` at `tier` in sysio.roa::nodeowners (forcereg -> regnodeowner also bumps
   /// sysio.system::nodecount.t{tier}_count via addnodeowner).
   void forcereg_owner(name owner, uint8_t tier) {
      BOOST_REQUIRE_EQUAL(success(), push(ROA_ACCOUNT, roa_abi, ROA_ACCOUNT, "forcereg"_n,
                                          mvo()("owner", owner.to_string())("tier", tier)));
   }

   /// Register all 21 tier-1 owners, plus any tier-2 / tier-3 owners requested.
   /// NODE_DADDY is skipped: genesis already forcereg'd it, and regnodeowner rejects re-registration.
   void register_tiers(size_t n_t2 = 0, size_t n_t3 = 0) {
      for (const auto& o : t1_owners)
         if (o != NODE_DADDY)
            forcereg_owner(o, 1);
      for (size_t i = 0; i < n_t2; ++i) {
         auto o = name_idx("two", i);
         mk(o);
         forcereg_owner(o, 2);
         t2_owners.push_back(o);
      }
      for (size_t i = 0; i < n_t3; ++i) {
         auto o = name_idx("thr", i);
         mk(o);
         forcereg_owner(o, 3);
         t3_owners.push_back(o);
      }
   }

   void mk(name a) {
      create_account(a, config::system_account_name, false, true, false);
      produce_block();
   }

   /// Create a candidate account with a finite allowance sufficient for its self-paid row.
   void mk_candidate(name candidate) {
      create_account(candidate, config::system_account_name, false, true, false);
      control->get_mutable_resource_limits_manager().set_account_limits(candidate, 4096, -1, -1, false);
      produce_block();
   }

   // ── generic action push (lands each action in its own block for distinct TaPoS) ─────────────
   action_result push(name contract, abi_serializer& ser, name signer, name action_name,
                      const fc::variant_object& data) {
      try {
         std::string action_type = ser.get_action_type(action_name);
         action act;
         act.account = contract;
         act.name = action_name;
         act.data =
            ser.variant_to_binary(action_type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
         act.authorization = std::vector<permission_level>{
            {signer, config::active_name}
         };
         signed_transaction trx;
         trx.actions.emplace_back(std::move(act));
         set_transaction_headers(trx);
         trx.sign(get_private_key(signer, "active"), control->get_chain_id());
         push_transaction(trx);
         produce_block();
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   // ── councl action wrappers ────────────────────────────────────────────────
   action_result addcandidate(name acct, const std::string& handle) {
      return push(COUNCL_ACCOUNT, councl_abi, acct, "addcandidate"_n,
                  mvo()("account", acct.to_string())("handle", handle));
   }
   action_result rmcandidate(name acct) {
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "rmcandidate"_n, mvo()("account", acct.to_string()));
   }
   action_result startinit(uint64_t slot, const std::vector<name>& owners) {
      fc::variants v;
      for (const auto& o : owners)
         v.emplace_back(o.to_string());
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "startinit"_n,
                  mvo()("time_slot_sec", slot)("ordered_owners", v));
   }
   action_result loadtier(uint8_t tier, uint32_t max_rows) {
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "loadtier"_n, mvo()("tier", tier)("max_rows", max_rows));
   }
   action_result finalizeinit() { return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "finalizeinit"_n, mvo()); }
   action_result reset() { return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "reset"_n, mvo()); }
   action_result purge(uint32_t max_rows) {
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "purge"_n, mvo()("max_rows", max_rows));
   }
   action_result repcandidate(name proposer, name c1, name c2, name c3) {
      return push(
         COUNCL_ACCOUNT, councl_abi, proposer, "repcandidate"_n,
         mvo()("proposer", proposer.to_string())("c1", c1.to_string())("c2", c2.to_string())("c3", c3.to_string()));
   }
   action_result vote(name voter, bool v1, bool v2, bool v3) {
      return push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n,
                  mvo()("voter", voter.to_string())("v1", v1)("v2", v2)("v3", v3));
   }
   action_result settle(name caller = COUNCL_ACCOUNT) {
      return push(COUNCL_ACCOUNT, councl_abi, caller, "settle"_n, mvo()("caller", caller.to_string()));
   }
   action_result stir(name caller) {
      return push(COUNCL_ACCOUNT, councl_abi, caller, "stir"_n, mvo()("caller", caller.to_string()));
   }
   action_result forceback() { return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "forceback"_n, mvo()); }
   action_result forceassign(name member) {
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "forceassign"_n, mvo()("member", member.to_string()));
   }

   // ── convenience: bring the contract to READY with `slot`, `n_t2`/`n_t3` extra tiers ──────────
   void register_candidates(size_t n) {
      for (size_t i = 0; i < n; ++i)
         BOOST_REQUIRE_EQUAL(success(), addcandidate(candidates_[i], "handle"));
   }
   void init_ready(size_t n_candidates = 23, size_t n_t2 = 0, size_t n_t3 = 0) {
      register_candidates(n_candidates);
      register_tiers(n_t2, n_t3);
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000));
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
   }

   // ── state / config readers ────────────────────────────────────────────────
   fc::variant get_state() {
      auto data = get_row_by_account(COUNCL_ACCOUNT, COUNCL_ACCOUNT, "state"_n, "state"_n);
      return data.empty() ? fc::variant()
                          : councl_abi.binary_to_variant(
                               "election_state", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   fc::variant get_config() {
      auto data = get_row_by_account(COUNCL_ACCOUNT, COUNCL_ACCOUNT, "config"_n, "config"_n);
      return data.empty() ? fc::variant()
                          : councl_abi.binary_to_variant(
                               "config_state", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   std::string phase() { return get_state()["phase"].as_string(); }
   std::string tier() { return get_state()["tier"].as_string(); }
   std::string init_phase() { return get_config()["init_phase"].as_string(); }
   uint8_t active_seat() { return get_state()["active_seat"].as<uint8_t>(); }
   uint8_t seats_filled() { return get_state()["seats_filled"].as<uint8_t>(); }
   uint64_t election_gen() { return get_config()["election_gen"].as<uint64_t>(); }
   uint64_t round_id() { return get_state()["round_id"].as<uint64_t>(); }
   uint32_t votes_cast() { return get_state()["votes_cast"].as<uint32_t>(); }
   uint32_t tier3_available() { return get_state()["tier3_available"].as<uint32_t>(); }
   uint64_t yes1() { return get_state()["yes1"].as<uint64_t>(); }
   uint64_t stir_count() { return get_state()["stir_count"].as<uint64_t>(); }
   std::string accumulator() { return get_state()["acc"].as_string(); }
   name proposer() { return name(get_state()["proposer"].as_string()); }

   /// Elected member for a filled seat, or the empty name if the seat row is absent.
   /// Per-election tables are scoped by the generation, so the scope name's value is election_gen.
   name council_member(uint64_t seat, uint64_t generation = GEN0) {
      auto data = get_row_by_id(COUNCL_ACCOUNT, name(generation), "council"_n, seat);
      if (data.empty())
         return name{};
      auto v = councl_abi.binary_to_variant("council_row", data,
                                            abi_serializer::create_yield_function(abi_serializer_max_time));
      return name(v["member"].as_string());
   }

   /// Return whether a generation still retains a candidate row for `candidate`.
   bool candidate_exists(name candidate, uint64_t generation = GEN0) {
      return !get_row_by_id(COUNCL_ACCOUNT, name(generation), "candidates"_n, candidate.value).empty();
   }

   /// Return whether a generation still retains its frozen roster row at `seat`.
   bool roster_exists(uint64_t seat, uint64_t generation = GEN0) {
      return !get_row_by_id(COUNCL_ACCOUNT, name(generation), "roster"_n, seat).empty();
   }

   /// Owner of tier-2 snapshot row `idx`, or the empty name if the row is absent.
   name tier2_owner(uint64_t idx, uint64_t generation = GEN0) {
      auto data = get_row_by_id(COUNCL_ACCOUNT, name(generation), "tier2"_n, idx);
      if (data.empty())
         return name{};
      auto v = councl_abi.binary_to_variant("tier2_row", data,
                                            abi_serializer::create_yield_function(abi_serializer_max_time));
      return name(v["owner"].as_string());
   }

   /// Owner of tier-3 snapshot row `idx`, or the empty name if the row is absent.
   name tier3_owner(uint64_t idx, uint64_t generation = GEN0) {
      auto data = get_row_by_id(COUNCL_ACCOUNT, name(generation), "tier3"_n, idx);
      if (data.empty())
         return name{};
      auto v = councl_abi.binary_to_variant("tier3_row", data,
                                            abi_serializer::create_yield_function(abi_serializer_max_time));
      return name(v["owner"].as_string());
   }

   /// Return whether any lazy Fisher-Yates remap row exists for a generation and seat.
   bool tier3_remap_exists(uint64_t generation, uint8_t seat, uint32_t tier3_size) {
      constexpr uint64_t SEAT_SCOPE_BITS = 40;
      const uint64_t scope = (generation << SEAT_SCOPE_BITS) | seat;
      for (uint64_t idx = 0; idx < tier3_size; ++idx)
         if (!get_row_by_id(COUNCL_ACCOUNT, name(scope), "tier3remap"_n, idx).empty())
            return true;
      return false;
   }

   /// Elapse one full attempt window so a nomination/voting deadline passes, then settle.
   void elapse_and_settle() {
      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), settle());
   }

   /// The 20 tier-1 owners other than the active proposer (tier-1 electorate).
   std::vector<name> tier1_voters_excluding(name p) {
      std::vector<name> v;
      for (const auto& o : t1_owners)
         if (o != p)
            v.push_back(o);
      return v;
   }

   /// Return the members of `owners` other than `excluded`.
   std::vector<name> excluding(const std::vector<name>& owners, name excluded) {
      std::vector<name> result;
      for (const auto owner : owners)
         if (owner != excluded)
            result.push_back(owner);
      return result;
   }
};

// ===========================================================================
BOOST_AUTO_TEST_SUITE(sysio_councl_tests)

// ── registration ──────────────────────────────────────────────────────────
BOOST_FIXTURE_TEST_CASE(registration, sysio_councl_tester) {
   try {
      const int64_t ram_before = control->get_resource_limits_manager().get_account_ram_usage(candidates_[0]);
      BOOST_REQUIRE_EQUAL(success(), addcandidate(candidates_[0], "alice"));
      const int64_t ram_after = control->get_resource_limits_manager().get_account_ram_usage(candidates_[0]);
      BOOST_CHECK_GT(ram_after, ram_before); // the candidate, not governance, paid for the row
      BOOST_REQUIRE_EQUAL(get_config()["cand_count"].as<uint32_t>(), 1u);

      // duplicate
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: already a candidate"),
                          addcandidate(candidates_[0], "alice"));
      // handle too long (> 32 bytes)
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: handle contains invalid characters or length"),
                          addcandidate(candidates_[1], std::string(33, 'x')));
      // wrong auth: data.account = candidates_[3] but the tx is signed by candidates_[2].
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[2], "addcandidate"_n,
                         mvo()("account", candidates_[3].to_string())("handle", "x")) != success());

      // rmcandidate by governance
      BOOST_REQUIRE_EQUAL(success(), rmcandidate(candidates_[0]));
      BOOST_REQUIRE_EQUAL(get_config()["cand_count"].as<uint32_t>(), 0u);
      BOOST_CHECK_EQUAL(control->get_resource_limits_manager().get_account_ram_usage(candidates_[0]), ram_before);
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(registration_is_capped_at_1000, sysio_councl_tester) {
   try {
      for (const auto candidate : candidates_)
         BOOST_REQUIRE_EQUAL(success(), addcandidate(candidate, "handle"));
      for (size_t i = candidates_.size(); i < 1000; ++i) {
         const name candidate = bulk_name('x', i);
         mk_candidate(candidate);
         BOOST_REQUIRE_EQUAL(success(), addcandidate(candidate, "handle"));
      }
      BOOST_REQUIRE_EQUAL(get_config()["cand_count"].as<uint32_t>(), 1000u);

      const name overflow = bulk_name('x', 1000);
      mk_candidate(overflow);
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: candidate registration limit reached"),
                          addcandidate(overflow, "handle"));
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(registration_rejects_unsafe_handle_bytes, sysio_councl_tester) {
   try {
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: handle contains invalid characters or length"),
                          addcandidate(candidates_[0], "bad\nhandle"));
      BOOST_REQUIRE_EQUAL(success(), addcandidate(candidates_[0], "@safe_handle-1.0"));
   }
   FC_LOG_AND_RETHROW()
}

// ── startinit guards ──────────────────────────────────────────────────────
BOOST_FIXTURE_TEST_CASE(startinit_requires_23_candidates, sysio_councl_tester) {
   try {
      register_candidates(22);
      register_tiers();
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: fewer candidates than required"),
                          startinit(TIME_SLOT, t1_owners));
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(startinit_roster_must_permute_tier1, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers();
      // wrong size
      std::vector<name> short_roster(t1_owners.begin(), t1_owners.end() - 1);
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: ordered_owners must list every council seat owner"),
                          startinit(TIME_SLOT, short_roster));
      // right size but contains a non-tier-1 account (a candidate)
      std::vector<name> bad = t1_owners;
      bad.back() = candidates_[0];
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: ordered_owners contains a non tier-1 owner"),
                          startinit(TIME_SLOT, bad));
      // happy
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(init_phase(), IP_LOADING);
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(startinit_bounds_time_slot, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers();
      constexpr uint64_t MAX_SLOT = 30u * 24u * 60u * 60u;
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: time_slot_sec must be positive"),
                          startinit(0, t1_owners));
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: time_slot_sec exceeds the safety limit"),
                          startinit(MAX_SLOT + 1, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), startinit(MAX_SLOT, t1_owners));
   }
   FC_LOG_AND_RETHROW()
}

/// Every governance-controlled registration and initialization boundary must reject a non-contract signer.
BOOST_FIXTURE_TEST_CASE(initialization_actions_require_contract_auth, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers(/*n_t2=*/1, /*n_t3=*/1);

      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[1], "rmcandidate"_n,
                         mvo()("account", candidates_[0].to_string())) != success());

      fc::variants owners;
      for (const auto owner : t1_owners)
         owners.emplace_back(owner.to_string());
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "startinit"_n,
                         mvo()("time_slot_sec", TIME_SLOT)("ordered_owners", owners)) != success());

      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "loadtier"_n,
                         mvo()("tier", uint8_t{2})("max_rows", uint32_t{1000})) != success());
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000));
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "finalizeinit"_n, mvo()) != success());
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
   }
   FC_LOG_AND_RETHROW()
}

// ── staged load + finalize ────────────────────────────────────────────────
BOOST_FIXTURE_TEST_CASE(staged_load_and_finalize, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers(/*n_t2=*/5, /*n_t3=*/9);
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      // batch tier-2 in two calls, tier-3 in one
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 3));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 3)); // remaining 2
      // finalize before tier-3 loaded -> incomplete
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: tier-3 snapshot incomplete"), finalizeinit());
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(get_config()["n2"].as<uint32_t>(), 5u);
      BOOST_REQUIRE_EQUAL(get_config()["n3"].as<uint32_t>(), 9u);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
      BOOST_REQUIRE_EQUAL(active_seat(), 0);
      BOOST_REQUIRE_EQUAL(proposer().to_string(), t1_owners[0].to_string());
   }
   FC_LOG_AND_RETHROW()
}

// ── staged load: roa tier churn between loadtier batches ──────────────────
/// REGRESSION: loadtier must resume by *identity* (skip owners already snapshotted), not by
/// position. A skip-count cursor mis-resumed when a tier-2 owner forcereg'd between batches
/// sorted before an already-loaded owner: the shifted enumeration re-wrote a snapshotted owner
/// (duplicate) and never wrote the newcomer, while finalizeinit's count cross-check still
/// passed. Identity-based dedup instead absorbs the newcomer in the next batch, keeping the
/// frozen snapshot a faithful, duplicate-free copy of roa's tier-2 set (DESIGN.md §11: the
/// election is immune to roa churn only if the snapshot itself is sound).
BOOST_FIXTURE_TEST_CASE(loadtier_roa_churn_mid_load, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers(); // the 21 tier-1 owners only

      // Two tier-2 owners present at startinit; bytier enumerates them in name order.
      name twob{"twob"}, twoc{"twoc"};
      mk(twob);
      forcereg_owner(twob, 2);
      mk(twoc);
      forcereg_owner(twoc, 2);

      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1)); // snapshots "twob" into idx 0

      // Churn mid-load: a new tier-2 owner that sorts BEFORE the already-loaded "twob".
      name twoa{"twoa"};
      mk(twoa);
      forcereg_owner(twoa, 2);

      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000)); // identity resume absorbs "twoa"
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE_EQUAL(success(), finalizeinit()); // t2_loaded == nodecount either way

      // Faithful snapshot: rows 0..2 hold exactly {twoa, twob, twoc}, no duplicates.
      std::set<name> snapshot;
      for (uint64_t i = 0; i < 3; ++i) {
         name o = tier2_owner(i);
         BOOST_REQUIRE_MESSAGE(snapshot.insert(o).second, "duplicate owner in tier-2 snapshot: " + o.to_string());
      }
      BOOST_CHECK_MESSAGE(snapshot == std::set<name>({twoa, twob, twoc}), "tier-2 snapshot is not the roa tier-2 set");
   }
   FC_LOG_AND_RETHROW()
}

// ── tier-1 happy path: 14 yes on c1 fills seat 0 ──────────────────────────
BOOST_FIXTURE_TEST_CASE(tier1_seat0_win, sysio_councl_tester) {
   try {
      init_ready();
      name p = proposer(); // == t1_owners[0]
      name a = candidates_[0], b = candidates_[1], c = candidates_[2];
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p, a, b, c));
      BOOST_REQUIRE_EQUAL(phase(), PH_VOTING);

      // 14 of the other 20 vote yes on c1 -> win the instant the 14th lands.
      auto voters = tier1_voters_excluding(p);
      for (int i = 0; i < 14; ++i)
         BOOST_REQUIRE_EQUAL(success(), vote(voters[i], true, false, false));

      BOOST_REQUIRE_EQUAL(council_member(0).to_string(), a.to_string());
      BOOST_REQUIRE_EQUAL(seats_filled(), 1);
      BOOST_REQUIRE_EQUAL(active_seat(), 1); // advanced to next seat
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: candidate already elected to a seat"),
                          repcandidate(proposer(), a, b, c));
   }
   FC_LOG_AND_RETHROW()
}

// ── strict priority: c1 eliminated (7 no) then c2 wins ────────────────────
BOOST_FIXTURE_TEST_CASE(strict_priority_promotes_c2, sysio_councl_tester) {
   try {
      init_ready();
      name p = proposer();
      name a = candidates_[0], b = candidates_[1], c = candidates_[2];
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p, a, b, c));
      auto voters = tier1_voters_excluding(p);
      // 7 voters vote NO on c1 (eliminates it) and YES on c2; then 7 more YES on c2 -> c2 at 14.
      for (int i = 0; i < 7; ++i)
         BOOST_REQUIRE_EQUAL(success(), vote(voters[i], false, true, false));
      for (int i = 7; i < 14; ++i)
         BOOST_REQUIRE_EQUAL(success(), vote(voters[i], false, true, false));
      BOOST_REQUIRE_EQUAL(council_member(0).to_string(), b.to_string());
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(strict_priority_promotes_c3, sysio_councl_tester) {
   try {
      init_ready();
      name p = proposer();
      name a = candidates_[0], b = candidates_[1], c = candidates_[2];
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p, a, b, c));
      auto voters = tier1_voters_excluding(p);
      for (int i = 0; i < 14; ++i)
         BOOST_REQUIRE_EQUAL(success(), vote(voters[i], false, false, true));
      BOOST_REQUIRE_EQUAL(council_member(0).to_string(), c.to_string());
   }
   FC_LOG_AND_RETHROW()
}

// ── repcandidate / vote guards ────────────────────────────────────────────
BOOST_FIXTURE_TEST_CASE(repcandidate_and_vote_guards, sysio_councl_tester) {
   try {
      init_ready();
      name p = proposer();
      name a = candidates_[0], b = candidates_[1], c = candidates_[2];

      // not your turn
      name not_p = (t1_owners[1] == p) ? t1_owners[2] : t1_owners[1];
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: not your turn to nominate"),
                          repcandidate(not_p, a, b, c));
      // distinctness
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: slate candidates must be distinct"),
                          repcandidate(p, a, a, b));
      // unregistered candidate
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: candidate not registered"),
                          repcandidate(p, a, b, candidates_[25])); // [25] not registered by init_ready (23)

      // open a valid slate, then vote guards
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p, a, b, c));
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: the proposer cannot vote on their own slate"),
                          vote(p, true, false, false));
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: not eligible to vote in this tier"),
                          vote(candidates_[10], true, false, false));
      auto voters = tier1_voters_excluding(p);
      BOOST_REQUIRE_EQUAL(success(), vote(voters[0], true, false, false));
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: already voted in this round"),
                          vote(voters[0], false, false, false));
   }
   FC_LOG_AND_RETHROW()
}

// ── escalation on nomination timeout: seat 0 -> tier 2 ────────────────────
BOOST_FIXTURE_TEST_CASE(escalation_to_tier2_on_timeout, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/5, /*n_t3=*/0);
      BOOST_REQUIRE_EQUAL(tier(), TIER_T1);
      // tier-1 proposer never nominates; window elapses; settle escalates to tier 2.
      elapse_and_settle();
      BOOST_REQUIRE_EQUAL(tier(), TIER_T2);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
      // The selected tier-2 proposer is read back (seed-agnostic) and must be one of the tier-2 owners.
      name p2 = proposer();
      bool is_t2 = false;
      for (const auto& o : t2_owners)
         if (o == p2)
            is_t2 = true;
      BOOST_REQUIRE(is_t2);
   }
   FC_LOG_AND_RETHROW()
}

// ── tier-2 voting: proposer auto-yes plus two voters reaches 3/4 ──────────
BOOST_FIXTURE_TEST_CASE(tier2_auto_yes_and_win, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/4, /*n_t3=*/0);
      elapse_and_settle();
      BOOST_REQUIRE_EQUAL(tier(), TIER_T2);
      name p2 = proposer();
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p2, candidates_[0], candidates_[1], candidates_[2]));
      BOOST_REQUIRE_EQUAL(yes1(), 1u); // proposer auto-yes

      auto voters = excluding(t2_owners, p2);
      BOOST_REQUIRE_EQUAL(success(), vote(voters[0], true, false, false));
      BOOST_REQUIRE_EQUAL(success(), vote(voters[1], true, false, false));
      BOOST_REQUIRE_EQUAL(council_member(0).to_string(), candidates_[0].to_string());
   }
   FC_LOG_AND_RETHROW()
}

// ── tier-2 failure escalates to tier 3 ───────────────────────────────────
BOOST_FIXTURE_TEST_CASE(tier2_failure_escalates_to_tier3, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/3, /*n_t3=*/3);
      elapse_and_settle();
      name p2 = proposer();
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p2, candidates_[0], candidates_[1], candidates_[2]));
      auto voters2 = excluding(t2_owners, p2);
      BOOST_REQUIRE_EQUAL(success(), vote(voters2[0], false, false, false));
      BOOST_REQUIRE_EQUAL(tier(), TIER_T3);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
      BOOST_REQUIRE_EQUAL(tier3_available(), 2u);
   }
   FC_LOG_AND_RETHROW()
}

// ── tier-3 retries are unique within a seat and terminate at BACKSTOP ────
BOOST_FIXTURE_TEST_CASE(tier3_unique_retries_and_exhaustion, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/0, /*n_t3=*/3);
      elapse_and_settle();
      BOOST_REQUIRE_EQUAL(tier(), TIER_T3);

      std::set<name> selected;
      for (int attempt = 0; attempt < 3; ++attempt) {
         name p3 = proposer();
         BOOST_REQUIRE(selected.insert(p3).second);
         BOOST_REQUIRE_EQUAL(tier3_available(), static_cast<uint32_t>(2 - attempt));
         BOOST_REQUIRE_EQUAL(success(), repcandidate(p3, candidates_[0], candidates_[1], candidates_[2]));
         auto voters3 = excluding(t3_owners, p3);
         BOOST_REQUIRE_EQUAL(success(), vote(voters3[0], false, false, false));
      }
      BOOST_REQUIRE_EQUAL(phase(), PH_BACKSTOP);
      BOOST_REQUIRE_EQUAL(selected.size(), 3u);
   }
   FC_LOG_AND_RETHROW()
}

// ── a single tier-3 owner auto-wins and may propose again for another seat ─
BOOST_FIXTURE_TEST_CASE(tier3_single_owner_reusable_on_next_seat, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/0, /*n_t3=*/1);
      elapse_and_settle();
      name p3 = proposer();
      BOOST_REQUIRE_EQUAL(p3.to_string(), t3_owners[0].to_string());
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p3, candidates_[0], candidates_[1], candidates_[2]));
      BOOST_REQUIRE_EQUAL(active_seat(), 1u); // N==1 proposer auto-yes elected c1 immediately

      elapse_and_settle();
      BOOST_REQUIRE_EQUAL(tier(), TIER_T3);
      BOOST_REQUIRE_EQUAL(proposer().to_string(), p3.to_string());
   }
   FC_LOG_AND_RETHROW()
}

// ── a stale ballot commits settlement but is not recorded in the next round
BOOST_FIXTURE_TEST_CASE(late_vote_is_settlement_only, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/2, /*n_t3=*/0);
      name p1 = proposer();
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p1, candidates_[0], candidates_[1], candidates_[2]));
      auto voters1 = tier1_voters_excluding(p1);
      BOOST_REQUIRE_EQUAL(success(), vote(voters1[0], true, false, false));
      const uint64_t old_round = round_id();

      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), vote(voters1[1], true, true, true));
      BOOST_REQUIRE_GT(round_id(), old_round);
      BOOST_REQUIRE_EQUAL(tier(), TIER_T2);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
      BOOST_REQUIRE_EQUAL(votes_cast(), 0u);
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(late_nomination_is_settlement_only, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/2, /*n_t3=*/0);
      const name stale_proposer = proposer();
      const uint64_t old_round = round_id();

      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), repcandidate(stale_proposer, candidates_[0], candidates_[1], candidates_[2]));
      BOOST_REQUIRE_GT(round_id(), old_round);
      BOOST_REQUIRE_EQUAL(tier(), TIER_T2);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
      BOOST_REQUIRE_EQUAL(votes_cast(), 0u);
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(full_turnout_failure_escalates, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/4, /*n_t3=*/1);
      elapse_and_settle();
      const name p2 = proposer();
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p2, candidates_[0], candidates_[1], candidates_[2]));
      const auto voters = excluding(t2_owners, p2);
      BOOST_REQUIRE_EQUAL(voters.size(), 3u);
      BOOST_REQUIRE_EQUAL(success(), vote(voters[0], false, false, true));
      BOOST_REQUIRE_EQUAL(success(), vote(voters[1], false, true, false));
      BOOST_REQUIRE_EQUAL(success(), vote(voters[2], true, false, false));
      BOOST_REQUIRE_EQUAL(tier(), TIER_T3);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
   }
   FC_LOG_AND_RETHROW()
}

// ── authenticated stir advances entropy and also cranks elapsed state ────
BOOST_FIXTURE_TEST_CASE(stir_uses_authenticated_caller_and_settles, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/2, /*n_t3=*/0);
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "stir"_n,
                         mvo()("caller", candidates_[1].to_string())) != success());
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "settle"_n,
                         mvo()("caller", candidates_[1].to_string())) != success());
      const uint64_t before = stir_count();
      BOOST_REQUIRE_EQUAL(success(), stir(candidates_[0]));
      BOOST_REQUIRE_EQUAL(stir_count(), before + 1);

      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), stir(candidates_[1]));
      BOOST_REQUIRE_EQUAL(tier(), TIER_T2);
   }
   FC_LOG_AND_RETHROW()
}

// ── governance may recover only an elapsed active attempt ────────────────
BOOST_FIXTURE_TEST_CASE(governance_forceback_requires_elapsed_attempt, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/3, /*n_t3=*/3);
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "forceback"_n, mvo()) != success());
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: the active attempt has not elapsed"), forceback());
      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), forceback());
      BOOST_REQUIRE_EQUAL(phase(), PH_BACKSTOP);
      BOOST_REQUIRE_EQUAL(success(), forceassign(candidates_[0]));
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(selection_replays_deterministically, sysio_councl_tester) {
   try {
      sysio_councl_tester replay;
      init_ready(/*n_candidates=*/23, /*n_t2=*/4, /*n_t3=*/4);
      replay.init_ready(/*n_candidates=*/23, /*n_t2=*/4, /*n_t3=*/4);

      elapse_and_settle();
      replay.elapse_and_settle();
      BOOST_REQUIRE_EQUAL(proposer().to_string(), replay.proposer().to_string());
      BOOST_REQUIRE_EQUAL(accumulator(), replay.accumulator());

      const name p2 = proposer();
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p2, candidates_[0], candidates_[1], candidates_[2]));
      BOOST_REQUIRE_EQUAL(success(),
                          replay.repcandidate(p2, replay.candidates_[0], replay.candidates_[1], replay.candidates_[2]));
      const auto voters2 = excluding(t2_owners, p2);
      for (size_t i = 0; i < 2; ++i) {
         BOOST_REQUIRE_EQUAL(success(), vote(voters2[i], false, false, false));
         BOOST_REQUIRE_EQUAL(success(), replay.vote(voters2[i], false, false, false));
      }
      BOOST_REQUIRE_EQUAL(proposer().to_string(), replay.proposer().to_string());
      BOOST_REQUIRE_EQUAL(accumulator(), replay.accumulator());
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(maximum_tier3_snapshot_and_retries, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers();
      for (size_t i = 0; i < 1000; ++i) {
         const name owner = bulk_name('y', i);
         mk(owner);
         forcereg_owner(owner, 3);
         t3_owners.push_back(owner);
      }

      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000));
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(get_config()["n3"].as<uint32_t>(), 1000u);

      elapse_and_settle();
      std::set<name> selected;
      for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
         BOOST_REQUIRE_EQUAL(tier(), TIER_T3);
         BOOST_REQUIRE(selected.insert(proposer()).second);
         BOOST_REQUIRE_EQUAL(tier3_available(), 999u - attempt);
         elapse_and_settle();
      }
      BOOST_REQUIRE_EQUAL(selected.size(), 1000u);
      BOOST_REQUIRE_EQUAL(phase(), PH_BACKSTOP);
   }
   FC_LOG_AND_RETHROW()
}

// ── nomination and voting windows are inclusive at the exact deadline ────
BOOST_FIXTURE_TEST_CASE(deadline_boundary_is_inclusive, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/2, /*n_t3=*/0);
      const auto until_exact_deadline = fc::milliseconds(TIME_SLOT * 1000 - config::block_interval_ms);

      // The next action executes one block interval later, exactly at the nomination deadline.
      produce_block(until_exact_deadline);
      name p1 = proposer();
      BOOST_REQUIRE_EQUAL(success(), repcandidate(p1, candidates_[0], candidates_[1], candidates_[2]));
      BOOST_REQUIRE_EQUAL(phase(), PH_VOTING);

      // Exactly at the voting deadline, settle must leave the round open.
      produce_block(until_exact_deadline);
      BOOST_REQUIRE_EQUAL(success(), settle());
      BOOST_REQUIRE_EQUAL(phase(), PH_VOTING);

      // One block interval later, the same round is elapsed and escalates.
      produce_block();
      BOOST_REQUIRE_EQUAL(success(), settle());
      BOOST_REQUIRE_EQUAL(tier(), TIER_T2);
      BOOST_REQUIRE_EQUAL(phase(), PH_AWAIT_REP);
   }
   FC_LOG_AND_RETHROW()
}

// ── governance backstop when tiers 2 & 3 are empty ────────────────────────
BOOST_FIXTURE_TEST_CASE(backstop_forceassign, sysio_councl_tester) {
   try {
      init_ready(/*n_candidates=*/23, /*n_t2=*/0, /*n_t3=*/0); // no escalation targets
      elapse_and_settle();                                     // tier-1 nomination times out -> no tier2/3 -> BACKSTOP
      BOOST_REQUIRE_EQUAL(phase(), PH_BACKSTOP);
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[1], "forceassign"_n,
                         mvo()("member", candidates_[0].to_string())) != success());
      // governance seats an un-elected candidate
      BOOST_REQUIRE_EQUAL(success(), forceassign(candidates_[0]));
      BOOST_REQUIRE_EQUAL(council_member(0).to_string(), candidates_[0].to_string());
      BOOST_REQUIRE_EQUAL(active_seat(), 1);
   }
   FC_LOG_AND_RETHROW()
}

// ── full two-generation election with staged cleanup and history retention ─
BOOST_FIXTURE_TEST_CASE(full_election_reset_cleanup_and_second_generation, sysio_councl_tester) {
   try {
      auto drive_generation = [&](uint64_t generation) {
         size_t next_cand = 0;
         auto take = [&]() { return candidates_[next_cand++]; };
         for (int seat = 0; seat < 21; ++seat) {
            BOOST_REQUIRE_EQUAL(active_seat(), seat);
            name p = proposer();
            name a = take(), b = take(), c = take();
            next_cand -= 2; // only the winner is consumed; the two losers remain reusable
            BOOST_REQUIRE_EQUAL(success(), repcandidate(p, a, b, c));
            auto voters = tier1_voters_excluding(p);
            for (int i = 0; i < 14; ++i)
               BOOST_REQUIRE_EQUAL(success(), vote(voters[i], true, false, false));
            BOOST_REQUIRE_EQUAL(council_member(seat, generation).to_string(), a.to_string());
         }
         BOOST_REQUIRE_EQUAL(phase(), PH_DONE);
         BOOST_REQUIRE_EQUAL(seats_filled(), 21);
      };

      init_ready(/*n_candidates=*/26);
      const int64_t candidate_ram_with_row =
         control->get_resource_limits_manager().get_account_ram_usage(candidates_[0]);
      drive_generation(/*generation=*/0);

      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "reset"_n, mvo()) != success());
      BOOST_REQUIRE_EQUAL(success(), reset());
      BOOST_REQUIRE_EQUAL(init_phase(), IP_CLEANING);
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "purge"_n, mvo()("max_rows", uint32_t{10})) !=
                    success());
      for (int calls = 0; init_phase() == IP_CLEANING && calls < 20; ++calls)
         BOOST_REQUIRE_EQUAL(success(), purge(/*max_rows=*/10));
      BOOST_REQUIRE_EQUAL(init_phase(), IP_REG);
      BOOST_REQUIRE_EQUAL(election_gen(), 1u);
      BOOST_REQUIRE(!council_member(0, 0).to_string().empty()); // permanent history retained
      BOOST_REQUIRE(!candidate_exists(candidates_[0], 0));
      BOOST_REQUIRE(!roster_exists(0, 0));
      BOOST_CHECK_LT(control->get_resource_limits_manager().get_account_ram_usage(candidates_[0]),
                     candidate_ram_with_row);

      register_candidates(26);
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000));
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(init_phase(), IP_READY);
      drive_generation(/*generation=*/1);
   }
   FC_LOG_AND_RETHROW()
}

/// Cleanup must reclaim non-empty tier snapshots and per-seat tier-3 remaps while retaining history.
BOOST_FIXTURE_TEST_CASE(cleanup_reclaims_all_ephemeral_table_categories, sysio_councl_tester) {
   try {
      constexpr uint32_t TIER2_SIZE = 2;
      constexpr uint32_t TIER3_SIZE = 9;
      init_ready(/*n_candidates=*/26, TIER2_SIZE, TIER3_SIZE);

      // Reach tier 3 for seat zero and leave a real, non-empty lazy Fisher-Yates remap behind.
      elapse_and_settle(); // tier 1 -> tier 2
      elapse_and_settle(); // tier 2 -> tier 3
      while (!tier3_remap_exists(GEN0, 0, TIER3_SIZE) && tier3_available() > 1)
         elapse_and_settle();
      BOOST_REQUIRE(tier3_remap_exists(GEN0, 0, TIER3_SIZE));

      // Governance closes the active attempt, then fills every seat so reset becomes available.
      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), forceback());
      BOOST_REQUIRE_EQUAL(success(), forceassign(candidates_[0]));
      for (uint8_t seat = 1; seat < 21; ++seat) {
         produce_block(fc::seconds(TIME_SLOT + 1));
         BOOST_REQUIRE_EQUAL(success(), forceback());
         BOOST_REQUIRE_EQUAL(success(), forceassign(candidates_[seat]));
      }
      BOOST_REQUIRE_EQUAL(phase(), PH_DONE);

      BOOST_REQUIRE(!tier2_owner(0).to_string().empty());
      BOOST_REQUIRE(!tier3_owner(0).to_string().empty());
      BOOST_REQUIRE(tier3_remap_exists(GEN0, 0, TIER3_SIZE));
      BOOST_REQUIRE_EQUAL(success(), reset());
      for (int calls = 0; init_phase() == IP_CLEANING && calls < 100; ++calls)
         BOOST_REQUIRE_EQUAL(success(), purge(/*max_rows=*/2));

      BOOST_REQUIRE_EQUAL(init_phase(), IP_REG);
      BOOST_REQUIRE(tier2_owner(0).to_string().empty());
      BOOST_REQUIRE(tier3_owner(0).to_string().empty());
      BOOST_REQUIRE(!tier3_remap_exists(GEN0, 0, TIER3_SIZE));
      BOOST_REQUIRE(!council_member(0, GEN0).to_string().empty());
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
