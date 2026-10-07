/// Contract behavior for simultaneous council flights, frozen ballots, and continuation.
#include "contracts.hpp"
#include "sysio.system_tester.hpp"

#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;

using mvo = fc::mutable_variant_object;

namespace {

// ABI enum spellings generated from sysio.councl.hpp.
constexpr auto PH_NOMINATING = "NOMINATING";
constexpr auto PH_GENERATING = "GENERATING";
constexpr auto PH_TABULATING = "TABULATING";
constexpr auto PH_CONTINUING = "CONTINUING";
constexpr auto PH_VOTING = "VOTING";
constexpr auto PH_DONE = "DONE";
constexpr auto IP_REG = "REG";
constexpr auto IP_LOADING = "LOADING";
constexpr auto IP_READY = "READY";
constexpr auto IP_CLEANING = "CLEANING";
constexpr auto TIER_GOVERNANCE = "GOVERNANCE";
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

/// Stable row rendering used to compare persisted state across failed actions and batch sizes.
std::string json_text(const fc::variant& value) {
   return fc::json::to_string(value, fc::time_point::maximum());
}

} // anonymous namespace

class sysio_councl_tester : public tester {
public:
   static constexpr auto COUNCL_ACCOUNT = "sysio.councl"_n;
   static constexpr auto ROA_ACCOUNT = "sysio.roa"_n;
   static constexpr uint64_t GEN0 = 0;
   static constexpr uint64_t TIME_SLOT = 3600; // seconds per attempt window

   // 21 tier-1 owners, plus pools of tier-2 / tier-3 owners and candidates.
   std::vector<name> t1_owners;   // exactly 21
   std::vector<name> t2_owners;   // optional
   std::vector<name> t3_owners;   // optional
   std::vector<name> candidates_; // >= 23

   abi_serializer councl_abi, roa_abi, system_abi;

   explicit sysio_councl_tester(bool configure_emissions = true) {
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

      // Deploy + init sysio.system so tests also exercise the optional emissions membership mirror.
      set_code(config::system_account_name, contracts::system_wasm());
      set_abi(config::system_account_name, contracts::system_abi().data());
      produce_blocks();
      load_abi(config::system_account_name, system_abi);
      base_tester::push_action(config::system_account_name, "init"_n, config::system_account_name,
                               mvo()("version", 0)("core", std::string("4,SYS")));
      if (configure_emissions)
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
      sysio_system::test_support::load_account_abi(*this, account, out_ser);
   }

   void setup_emission_config() {
      push(config::system_account_name, system_abi, config::system_account_name, "setemitcfg"_n,
           mvo()("cfg", sysio_system::test_support::default_emission_config()));
      produce_blocks();
   }

   /// Register `owner` at `tier` in authoritative sysio.roa::nodeowners. When emissions are
   /// configured, regnodeowner also updates sysio.system's distribution mirror.
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
      return sysio_system::test_support::push_contract_action_and_produce_block(*this, contract, ser, signer,
                                                                                action_name, data);
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
   /// Required generation/round binding shared by all signed election requests.
   mvo identity() { return mvo()("election_gen", election_gen())("round_id", round_id()); }
   /// Submit a manual flight with current election identity.
   action_result repcandidate(name proposer, name c1, name c2, name c3) {
      auto data = identity();
      data("proposer", proposer.to_string())("c1", c1.to_string())("c2", c2.to_string())("c3", c3.to_string());
      return push(COUNCL_ACCOUNT, councl_abi, proposer, "repcandidate"_n, data);
   }
   /// Build a complete, ordered ballot with explicit choices for selected seats and NO elsewhere.
   fc::variants ballot(name voter, const std::map<uint8_t, std::array<bool, 3>>& choices = {}) {
      fc::variants votes;
      for (uint8_t seat = 0; seat < t1_owners.size(); ++seat) {
         const auto flight = get_flight(seat);
         if (t1_owners[seat] == voter || !council_member(seat).to_string().empty() || flight.is_null() ||
             flight["round_id"].as<uint64_t>() != round_id() ||
             (get_state()["backstop_mask"].as<uint32_t>() & (uint32_t{1} << seat)))
            continue;
         const auto selected = choices.find(seat);
         const std::array<bool, 3> values = selected == choices.end() ? std::array<bool, 3>{} : selected->second;
         votes.emplace_back(mvo()("seat", seat)("v1", values[0])("v2", values[1])("v3", values[2]));
      }
      return votes;
   }
   /// Signed ballot payload; callers can mutate identity or coverage for negative tests.
   mvo vote_data(name voter, const fc::variants& votes) {
      auto data = identity();
      data("voter", voter.to_string())("flight_hash", get_state()["flight_hash"])("votes", votes);
      return data;
   }
   /// Cast a complete per-round ballot.
   action_result vote(name voter, const std::map<uint8_t, std::array<bool, 3>>& choices = {}) {
      return push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n, vote_data(voter, ballot(voter, choices)));
   }
   /// Drive a bounded public phase transition or cursor step.
   action_result settle(uint32_t steps = 21, name caller = COUNCL_ACCOUNT) {
      auto data = identity();
      data("caller", caller.to_string())("max_steps", steps);
      return push(COUNCL_ACCOUNT, councl_abi, caller, "settle"_n, data);
   }
   /// Contribute entropy without changing frozen flights or an existing generation seed.
   action_result stir(name caller) {
      return push(COUNCL_ACCOUNT, councl_abi, caller, "stir"_n, mvo()("caller", caller.to_string()));
   }
   /// Reserve a specific elapsed seat for governance.
   action_result forceback(uint8_t seat) {
      auto data = identity();
      data("seat", seat);
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "forceback"_n, data);
   }
   /// Assign a specific reserved seat.
   action_result forceassign(uint8_t seat, name member) {
      auto data = identity();
      data("seat", seat)("member", member.to_string());
      return push(COUNCL_ACCOUNT, councl_abi, COUNCL_ACCOUNT, "forceassign"_n, data);
   }
   /// Read a public flight row.
   fc::variant get_flight(uint8_t seat, uint64_t generation = GEN0) {
      auto data = get_row_by_id(COUNCL_ACCOUNT, name(generation), "flights"_n, seat);
      return data.empty() ? fc::variant{}
                          : councl_abi.binary_to_variant(
                               "flight_row", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   /// Read a public ballot row.
   fc::variant get_ballot(name voter, uint64_t generation = GEN0) {
      auto data = get_row_by_id(COUNCL_ACCOUNT, name(generation), "ballots"_n, voter.value);
      return data.empty() ? fc::variant{}
                          : councl_abi.binary_to_variant(
                               "ballot_row", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   /// Finish nominations and automatic generation.
   void open_votes(uint32_t steps = 21) {
      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), settle(steps));
      BOOST_REQUIRE_EQUAL(phase(), PH_GENERATING);
      for (size_t calls = 0; phase() == PH_GENERATING && calls < 21; ++calls)
         BOOST_REQUIRE_EQUAL(success(), settle(steps));
      BOOST_REQUIRE_EQUAL(phase(), PH_VOTING);
   }
   /// Close the common voting window and tabulate every seat without starting continuation.
   void close_votes(uint32_t steps = 21) {
      produce_block(fc::seconds(TIME_SLOT + 1));
      BOOST_REQUIRE_EQUAL(success(), settle(steps));
      BOOST_REQUIRE_EQUAL(phase(), PH_TABULATING);
      for (size_t calls = 0; phase() == PH_TABULATING && calls < 21; ++calls)
         BOOST_REQUIRE_EQUAL(success(), settle(steps));
   }
   /// Finish bounded reset cleanup.
   void finish_cleanup(uint32_t rows = 1000) {
      for (size_t calls = 0; init_phase() == IP_CLEANING && calls < 5000; ++calls)
         BOOST_REQUIRE_EQUAL(success(), purge(rows));
      BOOST_REQUIRE_EQUAL(init_phase(), IP_REG);
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
      load_tier_fully(2);
      load_tier_fully(3);
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
   }

   /// Complete a source-read-bounded tier scan across as many calls as necessary.
   void load_tier_fully(uint8_t tier, uint32_t batch_size = 1000) {
      const char* complete_field = tier == 2 ? "t2_scan_complete" : "t3_scan_complete";
      for (uint32_t calls = 0; !get_config()[complete_field].as_bool() && calls < 10000; ++calls)
         BOOST_REQUIRE_EQUAL(success(), loadtier(tier, batch_size));
      BOOST_REQUIRE(get_config()[complete_field].as_bool());
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
   std::string init_phase() { return get_config()["init_phase"].as_string(); }
   uint8_t seats_filled() { return get_state()["seats_filled"].as<uint8_t>(); }
   uint64_t election_gen() { return get_config()["election_gen"].as<uint64_t>(); }
   uint64_t round_id() { return get_state()["round_id"].as<uint64_t>(); }
   uint64_t stir_count() { return get_state()["stir_count"].as<uint64_t>(); }
   std::string accumulator() { return get_state()["acc"].as_string(); }

   /// ABI-decoded output row for a filled seat, or an empty variant when absent.
   /// Per-election tables are scoped by the generation, so the scope name's value is election_gen.
   fc::variant council_seat(uint64_t seat, uint64_t generation = GEN0) {
      auto data = get_row_by_id(COUNCL_ACCOUNT, name(generation), "council"_n, seat);
      if (data.empty())
         return fc::variant{};
      return councl_abi.binary_to_variant("council_row", data,
                                          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Elected member for a filled seat, or the empty name if the seat row is absent.
   name council_member(uint64_t seat, uint64_t generation = GEN0) {
      const auto row = council_seat(seat, generation);
      return row.is_null() ? name{} : name(row["member"].as_string());
   }

   /// Return whether a generation still retains a candidate row for `candidate`.
   bool candidate_exists(name candidate, uint64_t generation = GEN0) {
      return !get_row_by_id(COUNCL_ACCOUNT, name(generation), "candidates"_n, candidate.value).empty();
   }

   bool state_exists() { return !get_row_by_account(COUNCL_ACCOUNT, COUNCL_ACCOUNT, "state"_n, "state"_n).empty(); }

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

class sysio_councl_without_emissions_tester : public sysio_councl_tester {
public:
   sysio_councl_without_emissions_tester()
      : sysio_councl_tester(false) {}
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
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: not a candidate"), rmcandidate(candidates_[0]));
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
      register_tiers();
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      load_tier_fully(2);
      load_tier_fully(3);
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      open_votes();
      for (uint8_t seat = 0; seat < 21; ++seat)
         BOOST_REQUIRE_EQUAL(get_flight(seat)["candidates"].get_array().size(), 3u);
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

BOOST_FIXTURE_TEST_CASE(startinit_rejects_duplicate_roster_owner, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers();
      std::vector<name> duplicate = t1_owners;
      duplicate.back() = duplicate.front();
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: duplicate owner in ordered_owners"),
                          startinit(TIME_SLOT, duplicate));
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(startinit_requires_exactly_21_roa_tier1_owners, sysio_councl_tester) {
   try {
      register_candidates(23);
      // Genesis supplies NODE_DADDY; register only 19 of the remaining 20 owners.
      for (size_t i = 1; i < t1_owners.size() - 1; ++i)
         forcereg_owner(t1_owners[i], 1);
      BOOST_REQUIRE_EQUAL(
         error("assertion failure with message: roa tier-1 owner count does not match the council seat count"),
         startinit(TIME_SLOT, t1_owners));
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(roa_enforces_authoritative_tier1_cap_when_emissions_counter_lags, sysio_councl_tester) {
   try {
      register_tiers(); // ROA has 21; nodecount has 20 because NODE_DADDY predates setemitcfg.
      const name extra{"extraowner"};
      mk(extra);
      BOOST_REQUIRE_EQUAL(
         error("assertion failure with message: node owner tier cap reached"),
         push(ROA_ACCOUNT, roa_abi, ROA_ACCOUNT, "forcereg"_n, mvo()("owner", extra.to_string())("tier", uint8_t{1})));
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(finalize_uses_roa_without_emissions_configuration, sysio_councl_without_emissions_tester) {
   try {
      register_candidates(23);
      register_tiers(/*n_t2=*/2, /*n_t3=*/1); // every registration predates setemitcfg.
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      load_tier_fully(2);
      load_tier_fully(3);
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(init_phase(), IP_READY);
      BOOST_REQUIRE_EQUAL(get_config()["n2"].as<uint32_t>(), 2u);
      BOOST_REQUIRE_EQUAL(get_config()["n3"].as<uint32_t>(), 1u);
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
      BOOST_REQUIRE(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "reset"_n, mvo()) != success());
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
      // max_rows bounds source rows inspected, including rows from other tiers.
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 3));
      BOOST_REQUIRE(!get_config()["t2_scan_complete"].as_bool());
      BOOST_REQUIRE_NE(get_config()["t2_cursor"].as<uint64_t>(), 0u);
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: tier-2 source scan incomplete"), finalizeinit());
      load_tier_fully(2, 3);
      // finalize before tier-3 loaded -> incomplete
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: tier-3 source scan incomplete"), finalizeinit());
      load_tier_fully(3);
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(get_config()["n2"].as<uint32_t>(), 5u);
      BOOST_REQUIRE_EQUAL(get_config()["n3"].as<uint32_t>(), 9u);
      BOOST_REQUIRE_EQUAL(phase(), PH_NOMINATING);
      BOOST_REQUIRE_EQUAL(round_id(), 1u);
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE(loadtier_validates_phase_tier_and_batch_size, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers(/*n_t2=*/2, /*n_t3=*/1);
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: not in the loading phase"), loadtier(2, 1));
      for (const uint8_t invalid_tier : {uint8_t{0}, uint8_t{1}, uint8_t{4}})
         BOOST_REQUIRE_EQUAL(error("assertion failure with message: tier must be T2 or T3"), loadtier(invalid_tier, 1));
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: max_rows must be positive"), loadtier(2, 0));

      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000));
      const uint32_t loaded = get_config()["t2_loaded"].as<uint32_t>();
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000));
      BOOST_REQUIRE_EQUAL(get_config()["t2_loaded"].as<uint32_t>(), loaded);
      BOOST_REQUIRE_EQUAL(success(), loadtier(3, 1000));
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: not in the loading phase"), loadtier(2, 1));
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
      load_tier_fully(2); // snapshots twob/twoc and marks the first source pass complete

      // Churn mid-load: a new tier-2 owner that sorts BEFORE the already-loaded "twob".
      name twoa{"twoa"};
      mk(twoa);
      forcereg_owner(twoa, 2);

      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1000)); // a new pass absorbs "twoa" through identity dedup
      load_tier_fully(3);
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());

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

/// A failed or obsolete staged snapshot must be recoverable without redeploying the contract.
BOOST_FIXTURE_TEST_CASE(loading_generation_can_be_aborted_purged_and_restarted, sysio_councl_tester) {
   try {
      register_candidates(23);
      register_tiers(/*n_t2=*/2, /*n_t3=*/1);
      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      BOOST_REQUIRE_EQUAL(success(), loadtier(2, 1));
      BOOST_REQUIRE_EQUAL(error("assertion failure with message: tier-2 source scan incomplete"), finalizeinit());

      BOOST_REQUIRE_EQUAL(success(), reset());
      BOOST_REQUIRE_EQUAL(init_phase(), IP_CLEANING);
      for (int calls = 0; init_phase() == IP_CLEANING && calls < 20; ++calls)
         BOOST_REQUIRE_EQUAL(success(), purge(/*max_rows=*/10));
      BOOST_REQUIRE_EQUAL(init_phase(), IP_REG);
      BOOST_REQUIRE_EQUAL(election_gen(), GEN0);
      BOOST_REQUIRE(candidate_exists(candidates_[0], GEN0));
      BOOST_REQUIRE_EQUAL(get_config()["cand_count"].as<uint32_t>(), 23u);
      BOOST_REQUIRE(!roster_exists(0, GEN0));
      BOOST_REQUIRE(tier2_owner(0, GEN0).to_string().empty());
      BOOST_REQUIRE(!state_exists());

      BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
      load_tier_fully(2);
      load_tier_fully(3);
      BOOST_REQUIRE_EQUAL(success(), finalizeinit());
      BOOST_REQUIRE_EQUAL(init_phase(), IP_READY);
   }
   FC_LOG_AND_RETHROW()
}


/// Concurrent replacements reserve candidate positions, not candidates globally.
BOOST_FIXTURE_TEST_CASE(manual_flights_and_atomic_replacement, sysio_councl_tester) {
   init_ready();
   const auto& c = candidates_;
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], c[0], c[1], c[2]));
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[1], c[1], c[0], c[3]));
   const auto original = json_text(get_flight(0));
   BOOST_CHECK(repcandidate(t1_owners[2], c[0], c[4], c[5]) != success());
   BOOST_CHECK(repcandidate(t1_owners[0], c[1], c[4], c[5]) != success());
   BOOST_CHECK_EQUAL(json_text(get_flight(0)), original);
   BOOST_CHECK(repcandidate(t1_owners[2], c[4], c[1], c[5]) != success());       // old claim retained
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], c[0], c[4], c[5])); // own A retained
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[2], c[6], c[1], c[2])); // old B/C released
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], c[4], c[5], c[0])); // reorder
   BOOST_CHECK(repcandidate(t1_owners[0], c[7], c[7], c[8]) != success());
   BOOST_CHECK(repcandidate(t1_owners[0], c[23], c[7], c[8]) != success());
   BOOST_CHECK(repcandidate(c[0], c[7], c[8], c[9]) != success());
   auto unauthorized = identity();
   unauthorized("proposer", t1_owners[0].to_string())("c1", c[7].to_string())("c2", c[8].to_string())("c3",
                                                                                                      c[9].to_string());
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, t1_owners[1], "repcandidate"_n, unauthorized) != success());
   BOOST_CHECK_EQUAL(phase(), PH_NOMINATING);
}

/// Minimum pool fills all missing flights while every position remains unique.
BOOST_FIXTURE_TEST_CASE(automatic_flights_with_minimum_pool, sysio_councl_tester) {
   init_ready();
   BOOST_CHECK(vote(t1_owners[0]) != success());
   open_votes(1);
   std::array<std::set<std::string>, 3> positions;
   for (uint8_t seat = 0; seat < 21; ++seat) {
      const auto flight = get_flight(seat);
      BOOST_REQUIRE(flight["automatic"].as_bool());
      const auto names = flight["candidates"].get_array();
      BOOST_REQUIRE_EQUAL(names.size(), 3u);
      std::set<std::string> distinct;
      for (size_t pos = 0; pos < names.size(); ++pos) {
         BOOST_CHECK(distinct.insert(names[pos].as_string()).second);
         BOOST_CHECK(positions[pos].insert(names[pos].as_string()).second);
      }
   }
   BOOST_CHECK(repcandidate(t1_owners[0], candidates_[0], candidates_[1], candidates_[2]) != success());
   BOOST_CHECK_EQUAL(seats_filled(), 0u);
}

/// Ballots are complete, public, immutable, and bound to the frozen election/round/flight set.
BOOST_FIXTURE_TEST_CASE(ballot_guards_and_public_tallies, sysio_councl_tester) {
   init_ready(23, 1, 1);
   open_votes();
   const name voter = t1_owners[0];
   BOOST_REQUIRE_EQUAL(ballot(voter).size(), 20u);
   BOOST_REQUIRE_EQUAL(ballot(t2_owners[0]).size(), 21u);
   BOOST_REQUIRE_EQUAL(ballot(t3_owners[0]).size(), 21u);
   auto decisions = ballot(voter);
   decisions.pop_back();
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n, vote_data(voter, decisions)) != success());
   BOOST_CHECK_EQUAL(get_flight(1)["tallies"].get_array()[0]["votes_cast"].as_uint64(), 0u);
   auto wrong = vote_data(voter, ballot(voter));
   wrong("election_gen", election_gen() + 1);
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n, wrong) != success());
   wrong = vote_data(voter, ballot(voter));
   wrong("round_id", round_id() + 1);
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n, wrong) != success());
   wrong = vote_data(voter, ballot(voter));
   wrong("flight_hash", std::string(64, '0'));
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n, wrong) != success());
   decisions = ballot(voter);
   decisions.insert(decisions.begin(), mvo()("seat", 0)("v1", true)("v2", true)("v3", true));
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, voter, "vote"_n, vote_data(voter, decisions)) != success());
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, t1_owners[1], "vote"_n, vote_data(voter, ballot(voter))) != success());
   BOOST_CHECK(vote(candidates_[0]) != success());
   BOOST_REQUIRE_EQUAL(success(), vote(voter, {
                                                 {1, {true, true, true}}
   }));
   BOOST_CHECK(vote(voter) != success());
   const auto tally = get_flight(1)["tallies"].get_array()[0];
   BOOST_CHECK_EQUAL(tally["yes1"].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(tally["yes2"].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(tally["yes3"].as_uint64(), 1u);
   BOOST_CHECK_EQUAL(get_flight(0)["tallies"].get_array()[0]["votes_cast"].as_uint64(), 0u);
   BOOST_REQUIRE_EQUAL(get_ballot(voter)["votes"].get_array().size(), 20u);
   BOOST_CHECK_EQUAL(get_ballot(voter)["flight_hash"].as_string(), get_state()["flight_hash"].as_string());
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[0], {
                                                        {0, {false, true, false}}
   }));
   BOOST_REQUIRE_EQUAL(success(), vote(t3_owners[0], {
                                                        {0, {true, false, false}}
   }));
   BOOST_CHECK_EQUAL(seats_filled(), 0u); // no early seating even in one-member tiers
   close_votes();
   BOOST_CHECK_EQUAL(council_seat(0)["filled_tier"].as_string(), TIER_T2);
   BOOST_CHECK_EQUAL(council_member(0).to_string(), get_flight(0)["candidates"].get_array()[1].as_string());
}

/// Final qualification accepts B with 14 YES even when A has only five explicit NO votes.
BOOST_FIXTURE_TEST_CASE(final_yes_resolution_and_continuation, sysio_councl_tester) {
   init_ready();
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], candidates_[0], candidates_[1], candidates_[2]));
   open_votes();
   const auto stale = vote_data(t1_owners[20], ballot(t1_owners[20]));
   for (size_t i = 1; i <= 14; ++i)
      BOOST_REQUIRE_EQUAL(success(), vote(t1_owners[i], {
                                                           {0, {i <= 9, true, false}}
      }));
   BOOST_CHECK_EQUAL(seats_filled(), 0u);
   close_votes(1);
   BOOST_REQUIRE_EQUAL(council_member(0).to_string(), candidates_[1].to_string());
   BOOST_CHECK_EQUAL(council_seat(0)["filled_tier"].as_string(), TIER_T1);
   BOOST_REQUIRE_EQUAL(seats_filled(), 1u);
   BOOST_REQUIRE_EQUAL(phase(), PH_CONTINUING);
   const auto old_round = round_id();
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(round_id(), old_round + 1);
   BOOST_REQUIRE_EQUAL(phase(), PH_NOMINATING);
   BOOST_CHECK_EQUAL(council_member(0).to_string(), candidates_[1].to_string());
   BOOST_CHECK(repcandidate(t1_owners[0], candidates_[3], candidates_[4], candidates_[5]) != success());
   BOOST_CHECK(repcandidate(t1_owners[1], candidates_[1], candidates_[4], candidates_[5]) != success());
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[1], candidates_[0], candidates_[2], candidates_[3]));
   BOOST_CHECK(addcandidate(candidates_[23], "closed") != success());
   BOOST_CHECK(rmcandidate(candidates_[0]) != success());
   open_votes();
   BOOST_REQUIRE_EQUAL(ballot(t1_owners[0]).size(), 20u); // filled owner votes on every vacancy
   BOOST_REQUIRE_EQUAL(ballot(t1_owners[1]).size(), 19u);
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, t1_owners[20], "vote"_n, stale) != success());
   for (const auto owner : t1_owners) {
      if (owner == t1_owners[1])
         continue;
      BOOST_REQUIRE_EQUAL(success(), vote(owner, {
                                                    {1, {true, false, false}}
      }));
      if (get_flight(1)["tallies"].get_array()[0]["yes1"].as_uint64() == 13)
         break;
   }
   close_votes();
   BOOST_CHECK(council_member(1).to_string().empty()); // denominator stays 20, not vacancies or turnout
   BOOST_CHECK_EQUAL(seats_filled(), 1u);
}

/// Each seat fully evaluates T1/T2/T3 before the next seat; elected candidates never transfer votes.
BOOST_FIXTURE_TEST_CASE(seat_first_tier_priority_and_duplicate_elimination, sysio_councl_tester) {
   init_ready(23, 3, 1);
   const auto& c = candidates_;
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], c[0], c[1], c[2]));
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[1], c[3], c[0], c[4]));
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[2], c[5], c[6], c[7]));
   open_votes();
   for (size_t i = 0, cast = 0; i < t1_owners.size() && cast < 14; ++i) {
      if (i == 1)
         continue;
      BOOST_REQUIRE_EQUAL(success(), vote(t1_owners[i], {
                                                           {1, {false, true, false}}
      }));
      ++cast;
   }
   for (const auto voter : t2_owners)
      BOOST_REQUIRE_EQUAL(success(), vote(voter, {
                                                    {2, {false, false, true}}
      }));
   BOOST_REQUIRE_EQUAL(success(), vote(t3_owners[0], {
                                                        {0, {true, true, true}  },
                                                        {2, {true, false, false}}
   }));
   close_votes(1);
   BOOST_CHECK_EQUAL(council_member(0).to_string(), c[0].to_string());
   BOOST_CHECK_EQUAL(council_seat(0)["filled_tier"].as_string(), TIER_T3);
   BOOST_CHECK(council_member(1).to_string().empty());
   BOOST_CHECK_EQUAL(council_member(2).to_string(), c[7].to_string());
   BOOST_CHECK_EQUAL(council_seat(2)["filled_tier"].as_string(), TIER_T2);
}

/// No implicit YES credit, empty tiers, and fully rejected ballots all leave seats vacant.
BOOST_FIXTURE_TEST_CASE(no_implicit_votes_and_full_rejection, sysio_councl_tester) {
   init_ready(23, 1, 1);
   open_votes();
   for (const auto voter : t1_owners)
      BOOST_REQUIRE_EQUAL(success(), vote(voter));
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[0]));
   BOOST_REQUIRE_EQUAL(success(), vote(t3_owners[0]));
   BOOST_CHECK_EQUAL(phase(), PH_VOTING);
   close_votes();
   BOOST_CHECK_EQUAL(seats_filled(), 0u);
   BOOST_REQUIRE_EQUAL(success(), settle());
   open_votes();
   close_votes();
   BOOST_CHECK_EQUAL(seats_filled(), 0u);
}

/// Frozen owner identities and denominators survive later ROA registrations and continuation.
BOOST_FIXTURE_TEST_CASE(snapshot_stability_across_owner_churn, sysio_councl_tester) {
   init_ready(23, 2, 1);
   const name newcomer{"newowner"};
   mk(newcomer);
   forcereg_owner(newcomer, 2);
   open_votes();
   BOOST_CHECK(vote(newcomer) != success());
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[0], {
                                                        {0, {true, true, true}}
   }));
   close_votes();
   BOOST_CHECK(council_member(0).to_string().empty());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK_EQUAL(get_config()["n2"].as_uint64(), 2u);
   open_votes();
   BOOST_CHECK(vote(newcomer) != success());
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[0], {
                                                        {0, {true, false, false}}
   }));
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[1], {
                                                        {0, {true, false, false}}
   }));
   close_votes();
   BOOST_CHECK_EQUAL(council_seat(0)["filled_tier"].as_string(), TIER_T2);
}

/// Recovery preserves unique members under public settlement and releases withdrawn reservations.
BOOST_FIXTURE_TEST_CASE(governance_recovery_and_active_abort, sysio_councl_tester) {
   init_ready();
   for (uint8_t seat = 0; seat < 21; ++seat)
      BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[seat], candidates_[seat], candidates_[(seat + 1) % 23],
                                                  candidates_[(seat + 2) % 23]));
   BOOST_CHECK(forceback(0) != success());
   auto unauthorized = identity();
   unauthorized("seat", 0);
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "forceback"_n, unauthorized) != success());
   produce_block(fc::seconds(TIME_SLOT + 1));
   for (uint8_t seat = 0; seat < 20; ++seat) {
      BOOST_REQUIRE_EQUAL(success(), forceback(seat));
      BOOST_REQUIRE_EQUAL(success(), forceassign(seat, candidates_[seat]));
   }
   BOOST_CHECK(forceassign(0, candidates_[20]) != success());
   BOOST_CHECK(forceback(0) != success());
   BOOST_REQUIRE_EQUAL(success(), forceback(20));
   BOOST_CHECK(forceassign(20, candidates_[0]) != success());
   unauthorized = identity();
   unauthorized("seat", 20)("member", candidates_[20].to_string());
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, candidates_[0], "forceassign"_n, unauthorized) != success());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK(forceassign(20, candidates_[20]) != success()); // open shared window
   close_votes();
   BOOST_CHECK_EQUAL(seats_filled(), 20u);
   BOOST_REQUIRE_EQUAL(success(), reset());
   finish_cleanup(5);
   BOOST_CHECK(!state_exists());
   BOOST_CHECK(council_member(0).to_string().empty());
   BOOST_CHECK(!candidate_exists(candidates_[0]));
   BOOST_CHECK(get_flight(0).is_null());
   BOOST_CHECK_EQUAL(election_gen(), 1u);
}

/// Normal voting can fill all seats; completed cleanup retains results and rejects old generation signatures.
BOOST_FIXTURE_TEST_CASE(complete_election_cleanup_and_new_generation, sysio_councl_tester) {
   init_ready(23, 1, 0);
   for (uint8_t seat = 0; seat < 21; ++seat)
      BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[seat], candidates_[seat], candidates_[(seat + 1) % 23],
                                                  candidates_[(seat + 2) % 23]));
   open_votes();
   const auto stale = vote_data(t2_owners[0], ballot(t2_owners[0]));
   std::map<uint8_t, std::array<bool, 3>> choices;
   for (uint8_t seat = 0; seat < 21; ++seat)
      choices[seat] = {true, true, true};
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[0], choices));
   close_votes();
   BOOST_REQUIRE_EQUAL(phase(), PH_DONE);
   BOOST_REQUIRE_EQUAL(seats_filled(), 21u);
   for (uint8_t seat = 0; seat < 21; ++seat) {
      BOOST_CHECK_EQUAL(council_member(seat).to_string(), candidates_[seat].to_string());
      BOOST_CHECK_EQUAL(council_seat(seat)["seat_owner"].as_string(), t1_owners[seat].to_string());
      BOOST_CHECK_EQUAL(council_seat(seat)["proposer"].as_string(), t1_owners[seat].to_string());
   }
   BOOST_REQUIRE_EQUAL(success(), settle()); // completed crank is harmless
   BOOST_REQUIRE_EQUAL(success(), reset());
   finish_cleanup(5);
   BOOST_CHECK_EQUAL(council_member(0).to_string(), candidates_[0].to_string());
   BOOST_CHECK(get_flight(0).is_null());
   BOOST_CHECK(get_ballot(t2_owners[0]).is_null());
   BOOST_CHECK(!candidate_exists(candidates_[0]));
   register_candidates(23);
   BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
   load_tier_fully(2);
   load_tier_fully(3);
   BOOST_REQUIRE_EQUAL(success(), finalizeinit());
   produce_block(fc::seconds(TIME_SLOT + 1));
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK(push(COUNCL_ACCOUNT, councl_abi, t2_owners[0], "vote"_n, stale) != success());
   BOOST_CHECK_EQUAL(election_gen(), 1u);
}

/// Draws depend on the frozen seed, never later callers, entropy contributions, or batch size.
BOOST_FIXTURE_TEST_CASE(generation_and_tabulation_batching_equivalence, sysio_councl_tester) {
   sysio_councl_tester replay;
   init_ready(23, 1, 0);
   replay.init_ready(23, 1, 0);
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[17], candidates_[18], candidates_[19], candidates_[20]));
   BOOST_REQUIRE_EQUAL(success(), replay.repcandidate(replay.t1_owners[17], replay.candidates_[18],
                                                      replay.candidates_[19], replay.candidates_[20]));
   produce_block(fc::seconds(TIME_SLOT + 1));
   replay.produce_block(fc::seconds(TIME_SLOT + 1));
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(success(), replay.settle());
   const auto seed = get_state()["round_seed"].as_string();
   BOOST_REQUIRE_EQUAL(seed, replay.get_state()["round_seed"].as_string());
   BOOST_REQUIRE_EQUAL(success(), settle(21));
   for (uint8_t seat = 0; seat < 21; ++seat) {
      BOOST_REQUIRE_EQUAL(success(), replay.stir(replay.candidates_[seat]));
      BOOST_REQUIRE_EQUAL(success(), replay.settle(1, replay.t1_owners[seat]));
      BOOST_REQUIRE_EQUAL(seed, replay.get_state()["round_seed"].as_string());
   }
   BOOST_REQUIRE_EQUAL(get_state()["flight_hash"].as_string(), replay.get_state()["flight_hash"].as_string());
   for (uint8_t seat = 0; seat < 21; ++seat)
      BOOST_REQUIRE_EQUAL(json_text(get_flight(seat)), json_text(replay.get_flight(seat)));
   std::map<uint8_t, std::array<bool, 3>> choices;
   for (uint8_t seat = 0; seat < 21; ++seat)
      choices[seat] = {true, true, true};
   BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[0], choices));
   BOOST_REQUIRE_EQUAL(success(), replay.vote(replay.t2_owners[0], choices));
   close_votes(21);
   replay.close_votes(1);
   BOOST_CHECK_EQUAL(seats_filled(), replay.seats_filled());
   for (uint8_t seat = 0; seat < 21; ++seat)
      BOOST_CHECK_EQUAL(json_text(council_seat(seat)), json_text(replay.council_seat(seat)));
}

/// Submissions at the exact cutoff remain valid; settlement starts strictly afterward.
BOOST_FIXTURE_TEST_CASE(inclusive_nomination_and_voting_deadlines, sysio_councl_tester) {
   init_ready();
   const auto until_exact = fc::milliseconds(TIME_SLOT * 1000 - config::block_interval_ms);
   produce_block(until_exact);
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], candidates_[0], candidates_[1], candidates_[2]));
   BOOST_CHECK_EQUAL(phase(), PH_NOMINATING);
   BOOST_CHECK(repcandidate(t1_owners[1], candidates_[3], candidates_[4], candidates_[5]) != success());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(phase(), PH_GENERATING);
   BOOST_REQUIRE_EQUAL(success(), settle());
   produce_block(until_exact);
   BOOST_REQUIRE_EQUAL(success(), vote(t1_owners[1], {
                                                        {0, {true, true, true}}
   }));
   BOOST_CHECK_EQUAL(phase(), PH_VOTING);
   BOOST_CHECK(vote(t1_owners[2]) != success());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK_EQUAL(phase(), PH_TABULATING);

   sysio_councl_tester exact;
   exact.init_ready();
   exact.produce_block(until_exact);
   BOOST_REQUIRE_EQUAL(success(), exact.settle());
   BOOST_CHECK_EQUAL(exact.phase(), PH_NOMINATING);
   BOOST_REQUIRE_EQUAL(success(), exact.settle());
   BOOST_REQUIRE_EQUAL(success(), exact.settle());
   exact.produce_block(until_exact);
   BOOST_REQUIRE_EQUAL(success(), exact.settle());
   BOOST_CHECK_EQUAL(exact.phase(), PH_VOTING);
   BOOST_REQUIRE_EQUAL(success(), exact.settle());
   BOOST_CHECK_EQUAL(exact.phase(), PH_TABULATING);
}

/// Recovery of twenty seats must leave all three remaining candidates available for the last flight.
BOOST_FIXTURE_TEST_CASE(recovery_releases_claims_before_automatic_generation, sysio_councl_tester) {
   init_ready();
   for (uint8_t seat = 0; seat < 3; ++seat)
      BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[seat], candidates_[20 + seat], candidates_[seat],
                                                  candidates_[(seat + 1) % 3]));
   produce_block(fc::seconds(TIME_SLOT + 1));
   BOOST_REQUIRE_EQUAL(success(), settle());
   for (uint8_t seat = 0; seat < 20; ++seat) {
      BOOST_REQUIRE_EQUAL(success(), forceback(seat));
      BOOST_REQUIRE_EQUAL(success(), forceassign(seat, candidates_[seat]));
   }
   BOOST_REQUIRE_EQUAL(success(), settle());
   const auto names = get_flight(20)["candidates"].get_array();
   BOOST_REQUIRE_EQUAL(names.size(), 3u);
   std::set<std::string> remaining;
   for (const auto& candidate : names)
      remaining.insert(candidate.as_string());
   BOOST_CHECK(remaining == std::set<std::string>({candidates_[20].to_string(), candidates_[21].to_string(),
                                                   candidates_[22].to_string()}));
   for (const auto voter : t1_owners) {
      if (voter == t1_owners[20])
         continue;
      BOOST_REQUIRE_EQUAL(success(), vote(voter, {
                                                    {20, {true, true, true}}
      }));
   }
   close_votes();
   BOOST_CHECK_EQUAL(phase(), PH_DONE);
}

/// Maximum frozen tiers, full-size public ballots, cap thresholds, and bounded ballot cleanup.
BOOST_FIXTURE_TEST_CASE(maximum_tiers_and_ballot_storage, sysio_councl_tester) {
   register_candidates(23);
   register_tiers();
   for (size_t i = 0; i < 84; ++i) {
      const name owner = bulk_name('z', i);
      mk(owner);
      forcereg_owner(owner, 2);
      t2_owners.push_back(owner);
   }
   for (size_t i = 0; i < 1000; ++i) {
      const name owner = bulk_name('y', i);
      mk(owner);
      forcereg_owner(owner, 3);
      t3_owners.push_back(owner);
   }
   BOOST_REQUIRE_EQUAL(success(), startinit(TIME_SLOT, t1_owners));
   load_tier_fully(2, 100);
   load_tier_fully(3, 100);
   BOOST_REQUIRE_EQUAL(success(), finalizeinit());
   BOOST_CHECK_EQUAL(get_config()["n2"].as_uint64(), 84u);
   BOOST_CHECK_EQUAL(get_config()["n3"].as_uint64(), 1000u);
   BOOST_CHECK(loadtier(3, 1001) != success());
   BOOST_CHECK(settle(0) != success());
   BOOST_CHECK(settle(22) != success());
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[0], candidates_[0], candidates_[1], candidates_[2]));
   BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[1], candidates_[3], candidates_[4], candidates_[5]));
   open_votes();
   for (size_t i = 0; i < t2_owners.size(); ++i)
      BOOST_REQUIRE_EQUAL(success(), vote(t2_owners[i], {
                                                           {0, {i < 56, false, false}},
                                                           {1, {false, false, i < 57}}
      }));
   for (size_t i = 0; i < t3_owners.size(); ++i)
      BOOST_REQUIRE_EQUAL(success(), vote(t3_owners[i], {
                                                           {0, {i < 667, false, false}}
      }));
   for (const auto owner : t1_owners)
      BOOST_REQUIRE_EQUAL(success(), vote(owner));
   BOOST_CHECK_EQUAL(get_flight(0)["tallies"].get_array()[2]["votes_cast"].as_uint64(), 1000u);
   BOOST_CHECK_EQUAL(seats_filled(), 0u);
   close_votes();
   BOOST_CHECK_EQUAL(council_member(0).to_string(), candidates_[0].to_string());
   BOOST_CHECK_EQUAL(council_seat(0)["filled_tier"].as_string(), TIER_T3);
   BOOST_CHECK_EQUAL(council_member(1).to_string(), candidates_[5].to_string());
   BOOST_CHECK_EQUAL(council_seat(1)["filled_tier"].as_string(), TIER_T2);
   BOOST_REQUIRE_EQUAL(success(), reset());
   BOOST_CHECK(purge(1001) != success());
   finish_cleanup(7);
   BOOST_CHECK(get_ballot(t3_owners.front()).is_null());
   BOOST_CHECK(get_ballot(t3_owners.back()).is_null());
   BOOST_CHECK(get_ballot(t1_owners.back()).is_null());
   BOOST_CHECK(council_member(0).to_string().empty());
}

/// Recovery can interleave with ordered settlement and persists until assigned across continuation.
BOOST_FIXTURE_TEST_CASE(recovery_interleaves_with_tabulation_and_continuation, sysio_councl_tester) {
   init_ready(23, 1, 0);
   for (uint8_t seat = 0; seat < 3; ++seat)
      BOOST_REQUIRE_EQUAL(success(), repcandidate(t1_owners[seat], candidates_[seat * 3], candidates_[seat * 3 + 1],
                                                  candidates_[seat * 3 + 2]));
   open_votes();
   BOOST_REQUIRE_EQUAL(
      success(),
      vote(t2_owners[0], {
                            {0, {true, false, false}},
                            {1, {true, false, false}},
                            {2, {false, true, false}}
   }));
   produce_block(fc::seconds(TIME_SLOT + 1));
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(success(), settle(1));
   BOOST_REQUIRE_EQUAL(council_member(0).to_string(), candidates_[0].to_string());
   BOOST_REQUIRE_EQUAL(success(), forceback(1));
   BOOST_CHECK(forceassign(1, candidates_[0]) != success());
   BOOST_REQUIRE_EQUAL(success(), forceassign(1, candidates_[7]));
   BOOST_CHECK_EQUAL(council_seat(1)["filled_tier"].as_string(), TIER_GOVERNANCE);
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK(council_member(2).to_string().empty());
   BOOST_REQUIRE_EQUAL(success(), forceback(3));
   BOOST_REQUIRE_EQUAL(success(), forceback(3));
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK_EQUAL(get_state()["backstop_mask"].as_uint64(), uint32_t{1} << 3);
   BOOST_CHECK(repcandidate(t1_owners[3], candidates_[3], candidates_[4], candidates_[5]) != success());
   open_votes();
   BOOST_CHECK_EQUAL(ballot(t2_owners[0]).size(), 18u);
   BOOST_CHECK(forceassign(3, candidates_[9]) != success());
   produce_block(fc::seconds(TIME_SLOT + 1));
   BOOST_REQUIRE_EQUAL(success(), forceassign(3, candidates_[9]));
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_REQUIRE_EQUAL(success(), settle());
   BOOST_CHECK_EQUAL(seats_filled(), 3u);
   BOOST_CHECK_EQUAL(get_state()["backstop_mask"].as_uint64(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
