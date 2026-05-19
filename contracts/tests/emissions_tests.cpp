// contracts/tests/emissions_tests.cpp
//
// Focus: emissions logic in sysio.system:
//  - setinittime singleton initialization and immutability
//  - addnodeowner authorization + input validation + row creation per tier
//  - viewnodedist functional behavior (claimable/can_claim) across time states
//  - claimnodedis authorization + gating rules + claimed accounting updates + inline token transfer
//  - sysio.roa::forcereg wiring: inline addnodeowner occurs (guarded on emitcfg.exists())
//  - payepoch (driven by sysio.epoch::advance gate): opreg status filter, batch-op rotation group
//                  pay, slashed/terminated share rollback to treasury, treasury balance/floor
//                  enforcement via the gate's EmissionsBlocked path
//
// This fixture deploys the real sysio.opreg and sysio.epoch contracts (not a mock) so that
// emissions's cross-contract reads (operators_t, epochstate_t) exercise the same code paths
// that production will use. Helpers:
//   - register_operator(acct, type, bootstrapped=true) -> marks as OPERATOR_STATUS_ACTIVE
//   - slash_operator(acct)                            -> marks as OPERATOR_STATUS_SLASHED
//   - init_epoch_state() + advance_epoch_state()      -> drives sysio.epoch::current_epoch_index
//   - setup_producers(N) now auto-registers each producer as an opreg operator so existing
//     producer-pay tests pass through the opreg filter without test-level churn.


#include "contracts.hpp"

// fp_math.hpp is dependency-free __int128 fixed-point math; reused here so
// test expectations match the contract's per-epoch derivations bit-for-bit.
#include <sysio.system/fp_math.hpp>

#include <boost/test/unit_test.hpp>

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>

#include "sysio.system_tester.hpp"

#include <fc/variant_object.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/reflect.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

// sysio.roa is the authority expected by sysio.system::addnodeowner (require_auth("sysio.roa"_n))
static constexpr account_name ROA   = "sysio.roa"_n;
static constexpr account_name OPREG = "sysio.opreg"_n;
static constexpr account_name EPOCH = "sysio.epoch"_n;
static constexpr account_name CHALG = "sysio.chalg"_n;
static constexpr account_name MSGCH = "sysio.msgch"_n;
static constexpr account_name UWRIT = "sysio.uwrit"_n;

// Keep these in sync with contracts/sysio.system/src/emissions.cpp
static constexpr uint32_t SECONDS_PER_MONTH = 30u * 24u * 60u * 60u;
static constexpr uint32_t T1_DURATION       = 12u * SECONDS_PER_MONTH;
static constexpr uint32_t T2_DURATION       = 24u * SECONDS_PER_MONTH;
static constexpr uint32_t T3_DURATION       = 36u * SECONDS_PER_MONTH;

// MIN_CLAIMABLE in emissions.cpp: asset(10000000000, WIRE_SYMBOL)
static constexpr int64_t MIN_CLAIMABLE_AMOUNT = 10'000'000'000;

// In unit tests we use sysio::chain::* types; chain::symbol is not constexpr.
static const symbol WIRE_SYMBOL = symbol(9, "WIRE");

// Node Owner total_claimable amounts (in WIRE subunits)
static const asset T1_ALLOCATION(7500000000000000, WIRE_SYMBOL);
static const asset T2_ALLOCATION(1000000000000000, WIRE_SYMBOL);
static const asset T3_ALLOCATION(100000000000000,   WIRE_SYMBOL);

static constexpr account_name TOKEN = "sysio.token"_n;
static constexpr uint8_t      NETWORK_GEN = 0;

// Keep as a string because sysio.token table helpers in tests use symbol::from_string("p,SYM")
static const std::string WIRE_SYM_STR = "9,WIRE";

// Fund sysio heavily so claims never fail due to insufficient token balance.
static const asset WIRE_MAX_SUPPLY = asset::from_string("1000000000.000000000 WIRE");
static const asset WIRE_ISSUE_TO_SYSIO = asset::from_string("1000000000.000000000 WIRE");

// Mirror the on-chain return struct layout for viewnodedist.
// sysio.system::viewnodedist returns a packed node_claim_result.
struct node_claim_result {
   asset total_allocation;
   asset claimed;
   asset claimable;
   bool  can_claim;
};
FC_REFLECT( node_claim_result, (total_allocation)(claimed)(claimable)(can_claim) )

// T5 return struct mirror. last_epoch_index is a monotonic counter that
// mirrors sysio.epoch's current_epoch_index and is bumped by payepoch on
// each successful gate-passing advance.
struct t5_epoch_info {
   uint64_t       epoch_count;
   uint32_t       last_epoch_index;
   time_point_sec last_epoch_time;
   int64_t        last_epoch_emission;
   int64_t        total_distributed;
   int64_t        treasury_remaining;
   int64_t        next_emission_est;
   uint32_t       seconds_until_next;
};
FC_REFLECT( t5_epoch_info,
   (epoch_count)(last_epoch_index)(last_epoch_time)(last_epoch_emission)
   (total_distributed)(treasury_remaining)(next_emission_est)(seconds_until_next) )

// Mirror emission_config for viewemitcfg return value. Epoch length is
// canonical on sysio.epoch::epochcfg::epoch_duration_sec; mirror updated
// to drop epoch_duration_secs.
struct emit_cfg_result {
   int64_t   t1_allocation;
   int64_t   t2_allocation;
   int64_t   t3_allocation;
   uint32_t  t1_duration;
   uint32_t  t2_duration;
   uint32_t  t3_duration;
   int64_t   min_claimable;
   int64_t   t5_distributable;
   int64_t   t5_floor;
   uint16_t  target_annual_decay_bps;
   int64_t   annual_initial_emission;
   int64_t   annual_max_emission;
   int64_t   annual_min_emission;
   uint16_t  compute_bps;
   uint16_t  capital_bps;
   uint16_t  capex_bps;
   uint16_t  governance_bps;
   uint16_t  producer_bps;
   uint16_t  batch_op_bps;
   uint32_t  standby_end_rank;
   uint32_t  epoch_log_retention_count;
};
FC_REFLECT( emit_cfg_result,
   (t1_allocation)(t2_allocation)(t3_allocation)
   (t1_duration)(t2_duration)(t3_duration)(min_claimable)
   (t5_distributable)(t5_floor)
   (target_annual_decay_bps)
   (annual_initial_emission)(annual_max_emission)(annual_min_emission)
   (compute_bps)(capital_bps)(capex_bps)(governance_bps)
   (producer_bps)(batch_op_bps)
   (standby_end_rank)(epoch_log_retention_count) )

// T5 test helper: compute expected split
static int64_t test_split_bps(int64_t total, uint16_t bps) {
   __int128 p = static_cast<__int128>(total) * static_cast<__int128>(bps);
   return static_cast<int64_t>(p / 10000);
}

static time_point_sec tpsec(uint32_t secs) {
   return time_point_sec{ secs };
}

// Quiet substring checker for action_result strings.
static void require_substr(const std::string& s, const std::string& needle) {
   BOOST_REQUIRE( s.find(needle) != std::string::npos );
}

// ---------------------------------------------------------------------------
// T5 Treasury Emissions mirror constants (keep in sync with emission_config defaults)
// ---------------------------------------------------------------------------
static constexpr int64_t T5_DISTRIBUTABLE        = 375'000'000'000'000'000LL;
static constexpr int64_t T5_FLOOR                = 125'000'000'000'000'000LL;

// Annual emission config (replaces former per-epoch constants).
// 6940 bps surviving / year reproduces the old DECAY_NUMERATOR/DECAY_DENOMINATOR
// (9990/10000) shape when scaled to 1-day epochs (0.999^365 ~= 0.694).
// The annual_* values are the old per-(1-day) epoch values multiplied by 365.
static constexpr uint16_t TARGET_ANNUAL_DECAY_BPS  = 6940;
static constexpr int64_t  ANNUAL_INITIAL_EMISSION  = 563'150'000'000'000LL    * 365;
static constexpr int64_t  ANNUAL_MAX_EMISSION      = 3'000'000'000'000'000LL  * 365;
static constexpr int64_t  ANNUAL_MIN_EMISSION      = 100'000'000'000'000LL    * 365;

static constexpr int64_t SECONDS_PER_YEAR = 31'536'000;

// Mirror of the contract's scale_annual_to_epoch helper (linear scaling).
static int64_t test_scale_annual_to_epoch(int64_t annual, uint32_t epoch_secs) {
   return static_cast<int64_t>(
      (static_cast<__int128>(annual) * epoch_secs) / SECONDS_PER_YEAR);
}

// Mirror of the contract's per-epoch decay computation, using the same
// Q32.32 fp_math used on chain so test expectations are bit-exact.
static sysiosystem::fp_math::fp_t test_per_epoch_decay(
   uint16_t target_bps, uint32_t epoch_secs)
{
   namespace fp = sysiosystem::fp_math;
   const fp::fp_t base = (static_cast<fp::fp_t>(target_bps) << fp::FRAC_BITS) / 10000;
   const fp::fp_t exponent = fp::div(
      static_cast<fp::fp_t>(epoch_secs) << fp::FRAC_BITS,
      static_cast<fp::fp_t>(SECONDS_PER_YEAR) << fp::FRAC_BITS);
   return fp::pow_frac(base, exponent);
}

// Apply per-epoch decay to a previous emission (Q32.32 factor * int64_t).
static int64_t test_apply_decay(int64_t prev_emission,
                                uint16_t target_bps, uint32_t epoch_secs)
{
   namespace fp = sysiosystem::fp_math;
   const fp::fp_t factor = test_per_epoch_decay(target_bps, epoch_secs);
   return static_cast<int64_t>(
      (static_cast<__int128>(prev_emission) * factor) / fp::ONE);
}
static constexpr uint16_t COMPUTE_BPS            = 4000;
static constexpr uint16_t CAPITAL_BPS            = 3000;
static constexpr uint16_t CAPEX_BPS              = 2000;
static constexpr uint16_t PRODUCER_BPS           = 7000;

// Performance-based pay constants (keep in sync with emissions.cpp)
static constexpr uint32_t T_ACTIVE_PRODUCER_COUNT = 21;
static constexpr uint32_t T_STANDBY_START_RANK    = 22;
static constexpr uint32_t T_STANDBY_END_RANK      = 28;

// Helper: compute undistributed producer pool for an emission (when no producers eligible).
static int64_t compute_producer_pool(int64_t emission) {
   int64_t compute = test_split_bps(emission, COMPUTE_BPS);
   return test_split_bps(compute, PRODUCER_BPS);
}

// Helper: compute batch-op pool for an emission (goes to current rotation group of 7).
// When no batch-op operators are registered OR the current group has 0 members, the
// entire batch pool stays in the treasury -- slashed / absent members' shares are not
// redistributed.
static int64_t compute_batch_pool(int64_t emission) {
   int64_t compute = test_split_bps(emission, COMPUTE_BPS);
   int64_t producer = test_split_bps(compute, PRODUCER_BPS);
   return compute - producer; // batch_pool = compute - producer (preserves dust)
}

// Helper: combined producer + batch compute-bucket undistributed amount.
// In tests with no producers AND no batch-op group, both pools are undistributed.
static int64_t compute_undistributed_if_no_operators(int64_t emission) {
   return compute_producer_pool(emission) + compute_batch_pool(emission);
}

class sysio_emissions_tester : public tester {
public:
   sysio_emissions_tester() {
      produce_blocks(2);

      // --- sysio.system (emissions lives here) ---
      set_code( config::system_account_name, contracts::system_wasm() );
      set_abi ( config::system_account_name, contracts::system_abi().data() );

      base_tester::push_action(
         config::system_account_name,
         "init"_n,
         config::system_account_name,
         mvo()("version", 0)
              ("core", symbol(CORE_SYMBOL).to_string())
      );
      produce_blocks(1);

      // sysio.system ABI serializer
      {
         const auto* accnt = control->find_account_metadata( config::system_account_name );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
         sysio_abi_ser.set_abi( abi, abi_serializer::create_yield_function(abi_serializer_max_time) );
      }

      // --- sysio.roa is expected to already be deployed by the harness ---
      {
         const auto* accnt = control->find_account_metadata( ROA );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
         roa_abi_ser.set_abi( abi, abi_serializer::create_yield_function(abi_serializer_max_time) );
      }

      // --- sysio.token setup (only create the account if it doesn't exist) ---
      if (!control->db().find<account_object, by_name>(TOKEN)) {
         create_accounts({ TOKEN }, false, false, false, true); // include_ram_gift = true
         produce_blocks(1);
      }

      // --- RAM policy for sysio.token ---
      if (get_roa_policy(TOKEN, "nodedaddy"_n).is_null()) {
         auto tr = addpolicy_ram_only( "nodedaddy"_n, TOKEN, asset::from_string("500.0000 SYS") );
         BOOST_REQUIRE( tr );
         BOOST_REQUIRE( !tr->except );
         produce_blocks(1);
      }

      set_code( TOKEN, contracts::token_wasm() );
      set_abi ( TOKEN, contracts::token_abi().data() );
      set_privileged( TOKEN );
      produce_blocks(1);

      // sysio.token ABI serializer
      {
         const auto* accnt = control->find_account_metadata( TOKEN );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
         token_abi_ser.set_abi( abi, abi_serializer::create_yield_function(abi_serializer_max_time) );
      }

      // --- Ensure WIRE exists + fund sysio for claim transfers ---
      if (get_token_stats(WIRE_SYM_STR).is_null()) {
         BOOST_REQUIRE_EQUAL( success(), token_create(config::system_account_name, WIRE_MAX_SUPPLY) );
         produce_blocks(1);
      }

      if (get_wire_balance(config::system_account_name) < WIRE_ISSUE_TO_SYSIO) {
         BOOST_REQUIRE_EQUAL(
            success(),
            token_issue_to_self(
               config::system_account_name,
               WIRE_ISSUE_TO_SYSIO,
               "fund sysio for emissions claim tests"
            )
         );
         produce_blocks(1);
      }

      // --- sysio.opreg + sysio.epoch (real, not mocks) for emissions integration ---
      // payepoch reads operator status from sysio.opreg::operators; sysio.epoch's
      // gate reads emitcfg / t5state from sysio.system. Deploying the real contracts
      // here gives us cross-contract exercise of the kv::table reads.
      //
      // Under ROA (active via the base tester), accounts need explicit ROA
      // RAM policies before set_code can succeed for a large contract.
      //
      // UWRIT is created bare (no ROA policy, no code) on purpose: epoch
      // advance fires an unconditional inline sysio.uwrit::chklocks
      // (underwriter lock-expiry sweep). The chain only requires the target
      // account to exist; with no code the inline call is a harmless no-op,
      // and no underwriter locks are staged in these tests. Giving it a
      // 500 SYS policy like the others would overrun nodedaddy's ROA pool.
      create_accounts({ OPREG, EPOCH, CHALG, MSGCH, UWRIT });
      produce_blocks(1);

      for (auto acct : { OPREG, EPOCH, CHALG, MSGCH }) {
         if (get_roa_policy(acct, "nodedaddy"_n).is_null()) {
            auto tr = addpolicy_ram_only("nodedaddy"_n, acct, asset::from_string("500.0000 SYS"));
            BOOST_REQUIRE( tr );
            BOOST_REQUIRE( !tr->except );
         }
      }
      produce_blocks(1);

      set_code( OPREG, contracts::opreg_wasm() );
      set_abi ( OPREG, contracts::opreg_abi().data() );
      set_privileged( OPREG );

      set_code( EPOCH, contracts::epoch_wasm() );
      set_abi ( EPOCH, contracts::epoch_abi().data() );
      set_privileged( EPOCH );

      produce_blocks(1);

      {
         const auto* accnt = control->find_account_metadata( OPREG );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
         opreg_abi_ser.set_abi( abi, abi_serializer::create_yield_function(abi_serializer_max_time) );
      }
      {
         const auto* accnt = control->find_account_metadata( EPOCH );
         BOOST_REQUIRE( accnt != nullptr );
         abi_def abi;
         BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
         epoch_abi_ser.set_abi( abi, abi_serializer::create_yield_function(abi_serializer_max_time) );
      }

      // --- Set default emission configuration ---
      BOOST_REQUIRE_EQUAL( success(), setemitcfg_defaults( config::system_account_name ) );
      produce_blocks(1);

      // --- Bootstrap sysio.epoch (config only, no advance) ---
      // bootstrap_epoch() sets epochcfg but defers genesis advance so each test
      // controls when (and whether) the first advance fires. The first advance
      // after init_epoch_state has next_epoch_start defaulted to 0, so it
      // crosses the wall-clock check immediately; later advances need
      // produce_blocks to cross the configured epoch duration first.
      bootstrap_epoch();
   }


   /// Current head block time in seconds since epoch (used for deterministic time tests).
   uint32_t head_secs() const {
      return time_point_sec(control->head().block_time()).sec_since_epoch();
   }

   /// IMPORTANT:
   /// Create accounts using the same behavior as the other suites in this repo
   /// The last bool parameter `include_ram_gift=true` ensures accounts have RAM,
   void create_user_accounts( std::initializer_list<account_name> accts ) {
      vector<account_name> v(accts.begin(), accts.end());
      create_accounts( v, false, false, false, true );
      produce_blocks(1);
   }

   // -----------------------------
   // T5 holding accounts
   // -----------------------------
   void create_t5_holding_accounts() {
      vector<account_name> accts = {
         "sysio.cap"_n, "sysio.gov"_n, "sysio.batch"_n, "sysio.ops"_n
      };
      for (auto a : accts) {
         if (!control->db().find<account_object, by_name>(a)) {
            create_accounts({ a }, false, false, false, true);
         }
      }
      produce_blocks(1);
   }

   // -----------------------------
   // sysio.system action helpers
   // -----------------------------

   action_result setemitcfg( account_name signer, const variant_object& cfg ) {
      return push_system_action(signer, "setemitcfg"_n, mvo()("cfg", cfg));
   }

   action_result setemitcfg_defaults( account_name signer ) {
      return setemitcfg_with_cadence(signer, uint16_t(1));
   }

   /// Same as setemitcfg_defaults but with a configurable pay_cadence_epochs.
   /// Tests that exercise cadence > 1 behavior call this directly.
   action_result setemitcfg_with_cadence( account_name signer, uint16_t cadence ) {
      return setemitcfg(signer, mvo()
         ("t1_allocation",          T1_ALLOCATION.get_amount())
         ("t2_allocation",          T2_ALLOCATION.get_amount())
         ("t3_allocation",          T3_ALLOCATION.get_amount())
         ("t1_duration",            T1_DURATION)
         ("t2_duration",            T2_DURATION)
         ("t3_duration",            T3_DURATION)
         ("min_claimable",          MIN_CLAIMABLE_AMOUNT)
         ("t5_distributable",       T5_DISTRIBUTABLE)
         ("t5_floor",               125'000'000'000'000'000LL)
         ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
         ("annual_initial_emission", ANNUAL_INITIAL_EMISSION)
         ("annual_max_emission",     ANNUAL_MAX_EMISSION)
         ("annual_min_emission",     ANNUAL_MIN_EMISSION)
         ("compute_bps",            COMPUTE_BPS)
         ("capital_bps",            CAPITAL_BPS)
         ("capex_bps",              CAPEX_BPS)
         ("governance_bps",         uint16_t(1000))
         ("producer_bps",           PRODUCER_BPS)
         ("batch_op_bps",           uint16_t(3000))
         ("standby_end_rank",       T_STANDBY_END_RANK)
         ("epoch_log_retention_count", uint32_t(8640))
         ("pay_cadence_epochs",     cadence)
      );
   }

   action_result setinittime( account_name signer, time_point_sec start ) {
      return push_system_action(
         signer,
         "setinittime"_n,
         mvo()("no_reward_init_time", start)
      );
   }

   action_result addnodeowner( account_name signer, account_name owner, uint8_t tier ) {
      return push_system_action(
         signer,
         "addnodeowner"_n,
         mvo()("account_name", owner)
              ("tier", tier)
      );
   }

   action_result claimnodedis( account_name signer, account_name owner ) {
      return push_system_action(
         signer,
         "claimnodedis"_n,
         mvo()("account_name", owner)
      );
   }

   // -----------------------------
   // T5 action helpers
   // -----------------------------

   action_result initt5( account_name signer, time_point_sec start ) {
      return push_system_action(
         signer,
         "initt5"_n,
         mvo()("start_time", start)
      );
   }

   t5_epoch_info viewepoch() {
      auto trace = push_system_action_trace(
         config::system_account_name,
         "viewepoch"_n,
         mvo()
      );
      BOOST_REQUIRE(trace);
      if (trace->except) BOOST_FAIL( trace->except->to_detail_string() );
      BOOST_REQUIRE(!trace->action_traces.empty());

      const action_trace* found = nullptr;
      for (const auto& at : trace->action_traces) {
         if (at.receiver == config::system_account_name && at.act.name == "viewepoch"_n) {
            found = &at;
            break;
         }
      }
      BOOST_REQUIRE(found != nullptr);
      BOOST_REQUIRE(!found->return_value.empty());
      return fc::raw::unpack<t5_epoch_info>( found->return_value );
   }

   emit_cfg_result viewemitcfg() {
      auto trace = push_system_action_trace(
         config::system_account_name,
         "viewemitcfg"_n,
         mvo()
      );
      BOOST_REQUIRE(trace);
      if (trace->except) BOOST_FAIL( trace->except->to_detail_string() );
      BOOST_REQUIRE(!trace->action_traces.empty());

      const action_trace* found = nullptr;
      for (const auto& at : trace->action_traces) {
         if (at.receiver == config::system_account_name && at.act.name == "viewemitcfg"_n) {
            found = &at;
            break;
         }
      }
      BOOST_REQUIRE(found != nullptr);
      BOOST_REQUIRE(!found->return_value.empty());
      return fc::raw::unpack<emit_cfg_result>( found->return_value );
   }

   // -----------------------------
   // T5 table readers
   // -----------------------------
   //
   // kv::global singletons are stored under the table-name-as-primary-key,
   // mirroring opreg::opconfig / epoch::epochstate. Multi-row kv::table rows
   // use the row's primary_key value.

   fc::variant get_node_count_state() {
      auto data = get_row_by_account(config::system_account_name,
                                     config::system_account_name,
                                     "nodecount"_n,
                                     "nodecount"_n);
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant("node_count_state", data,
          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_blocklog_row(uint64_t epoch_index) {
      auto data = get_row_by_account(EPOCH, EPOCH, "blocklog"_n, account_name(epoch_index));
      if (data.empty()) return fc::variant();
      return epoch_abi_ser.binary_to_variant("blocklog_entry", data,
          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_t5_state() {
      auto data = get_row_by_account(config::system_account_name,
                                     config::system_account_name,
                                     "t5state"_n,
                                     "t5state"_n);
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant("t5_state", data,
          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // Reads the audit-log row keyed by sysio.epoch's current_epoch_index
   // (t5_state::last_epoch_index at write time). Callers pass the sysio.epoch
   // index they want to inspect.
   fc::variant get_epoch_log( uint64_t sysio_epoch_index ) {
      auto data = get_row_by_account(config::system_account_name,
                                     config::system_account_name,
                                     "epochlog"_n,
                                     account_name(sysio_epoch_index));
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant("epoch_log", data,
          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // -----------------------------
   // Producer info reader
   // -----------------------------

   fc::variant get_producer_info( account_name producer ) {
      auto data = get_row_by_account(config::system_account_name,
                                     config::system_account_name,
                                     "producers"_n,
                                     producer);
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant("producer_info", data,
          abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // -----------------------------
   // Producer name helpers
   // -----------------------------

   static name producer_name_at(uint32_t index) {
      std::string name_str;
      if (index < 26) {
         name_str = "producer";
         name_str += static_cast<char>('a' + index);
      } else {
         name_str = "standby";
         name_str += static_cast<char>('a' + (index - 26));
      }
      return name(name_str);
   }

   // -----------------------------
   // Producer setup helper
   // -----------------------------
   //
   // Creates N test accounts, registers them as producers in sysio.system,
   // AND registers each as a bootstrapped opreg operator (-> OPERATOR_STATUS_ACTIVE).
   // Without the opreg registration step, payepoch's opreg status filter would
   // skip them all and no producer would ever be paid.
   //
   // If `register_opreg` is false, the caller is exercising the filter and will
   // handle opreg registration manually (e.g. to test a slashed operator).
   void setup_producers( uint32_t count, bool register_opreg = true ) {
      std::vector<account_name> prod_names;
      for (uint32_t i = 0; i < count; ++i) {
         prod_names.push_back(producer_name_at(i));
      }

      // Create accounts
      create_accounts(prod_names, false, false, false, true);
      produce_blocks(1);

      // Register as producers
      for (auto& pname : prod_names) {
         auto key = get_public_key(pname, "active");
         push_system_action(pname, "regproducer"_n, mvo()
            ("producer", pname)
            ("producer_key", key)
            ("url", "")
            ("location", 0)
         );
      }
      produce_blocks(1);

      // Register as bootstrapped opreg operators so emissions's opreg filter
      // treats them as ACTIVE / eligible for distribution.
      if (register_opreg) {
         for (auto& pname : prod_names) {
            BOOST_REQUIRE_EQUAL(
               success(),
               register_operator(pname, OperatorType::OPERATOR_TYPE_PRODUCER, /*bootstrapped=*/true)
            );
         }
         produce_blocks(1);
      }

      // Build schedule and call setprodkeys
      set_producer_schedule(prod_names);
      produce_blocks(1);
   }

   // Set the active producer schedule via setprodkeys
   action_result set_producer_schedule( const std::vector<account_name>& prod_names ) {
      std::vector<fc::variant> schedule;
      for (auto& pname : prod_names) {
         auto key = get_public_key(pname, "active");
         schedule.push_back(mvo()
            ("producer_name", pname)
            ("block_signing_key", key)
         );
      }
      return push_system_action(config::system_account_name, "setprodkeys"_n, mvo()
         ("schedule", schedule)
      );
   }

   // Wait for the producer schedule to activate (new producers producing blocks)
   void wait_for_producer_schedule() {
      int max_attempts = 500;
      while (control->head().header().producer == config::system_account_name && max_attempts-- > 0) {
         produce_blocks(1);
      }
   }

   // Produce exact number of complete round-robin cycles for N producers
   void produce_complete_cycles(uint32_t num_producers, uint32_t num_cycles) {
      produce_blocks(num_producers * 12 * num_cycles);
   }

   // -----------------------------
   // viewnodedist return decoding
   // -----------------------------

   /// Calls sysio.system::viewnodedist and decodes the return_value into node_claim_result.
   /// We search action_traces for the sysio.system receiver trace for this action to avoid
   /// decoding the wrong trace in a nested/inline scenario.
   node_claim_result viewnodedist( account_name owner ) {
      auto trace = push_system_action_trace(
         config::system_account_name,
         "viewnodedist"_n,
         mvo()("account_name", owner)
      );

      BOOST_REQUIRE(trace);
      if (trace->except) {
         BOOST_FAIL( trace->except->to_detail_string() );
      }
      BOOST_REQUIRE(!trace->action_traces.empty());

      const action_trace* found = nullptr;
      for (const auto& at : trace->action_traces) {
         if (at.receiver == config::system_account_name && at.act.name == "viewnodedist"_n) {
            found = &at;
            break;
         }
      }

      BOOST_REQUIRE(found != nullptr);
      BOOST_REQUIRE(!found->return_value.empty());

      return fc::raw::unpack<node_claim_result>( found->return_value );
   }

   // -----------------------------
   // sysio.roa wiring (forcereg)
   // -----------------------------

   /// Executes sysio.roa::forcereg and returns a trace so we can assert that
   /// an inline sysio.system::addnodeowner occurred.
   transaction_trace_ptr forcereg_trace( account_name signer, account_name owner, uint8_t tier ) {
      return push_roa_action_trace(
         signer,
         "forcereg"_n,
         mvo()("owner", owner)
              ("tier", tier)
      );
   }

   // -----------------------------
   // Table readers (ABI decoding)
   // -----------------------------

   fc::variant get_emission_state() {
      // kv::global stores under the table-name-as-primary-key.
      auto data = get_row_by_account(
         config::system_account_name,
         config::system_account_name,
         "emissionmngr"_n,
         "emissionmngr"_n
      );
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant(
         "emission_state",
         data,
         abi_serializer::create_yield_function(abi_serializer_max_time)
      );
   }

   /// Reads a row from the node owner distribution table:
   ///   typedef sysio::multi_index<"nodedist"_n, node_owner_distribution> nodedist_t;
   fc::variant get_nodedist_row( account_name owner ) {
      vector<char> data = get_row_by_account(
         config::system_account_name,
         config::system_account_name,
         "nodedist"_n,
         owner
      );

      return data.empty()
         ? fc::variant()
         : sysio_abi_ser.binary_to_variant(
              "node_owner_distribution",
              data,
              abi_serializer::create_yield_function(abi_serializer_max_time)
           );
   }

   asset get_wire_balance( account_name acc ) {
      auto row = get_token_account_row(acc, WIRE_SYM_STR);
      if (row.is_null())
         return asset(0, WIRE_SYMBOL);
      return row["balance"].as<asset>();
   }

   fc::variant get_token_stats( const std::string& symbolname ) {
      auto symb = sysio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;

      std::vector<char> data = get_row_by_account(
         TOKEN,
         name(symbol_code),     // scope = symbol_code
         "stat"_n,
         account_name(symbol_code)
      );

      return data.empty()
         ? fc::variant()
         : token_abi_ser.binary_to_variant(
              "currency_stats",
              data,
              abi_serializer::create_yield_function(abi_serializer_max_time)
           );
   }

   fc::variant get_token_account_row( account_name acc, const std::string& symbolname ) {
      auto symb = sysio::chain::symbol::from_string(symbolname);
      auto symbol_code = symb.to_symbol_code().value;

      std::vector<char> data = get_row_by_account(
         TOKEN,
         acc,
         "accounts"_n,
         account_name(symbol_code)
      );

      return data.empty()
         ? fc::variant()
         : token_abi_ser.binary_to_variant(
              "account",
              data,
              abi_serializer::create_yield_function(abi_serializer_max_time)
           );
   }

   transaction_trace_ptr addpolicy_ram_only( account_name issuer, account_name owner, asset ram_weight ) {
      // NOTE: owner == sysio.token (sysio.*), so NET/CPU MUST be zero per ROA rules.
      return base_tester::push_action(
         ROA,
         "addpolicy"_n,
         vector<permission_level>{{ issuer, "active"_n }},
         mvo()
           ("owner", owner)
           ("issuer", issuer)
           ("net_weight", asset::from_string("0.0000 SYS"))
           ("cpu_weight", asset::from_string("0.0000 SYS"))
           ("ram_weight", ram_weight)
           ("time_block", control->head().block_num())
           ("network_gen", NETWORK_GEN)
      );
   }

private:
   // -----------------------------
   // Internal push helpers
   // -----------------------------

   action_result push_system_action( const account_name& signer,
                                    const action_name& name,
                                    const variant_object& data ) {
      const string action_type_name = sysio_abi_ser.get_action_type(name);

      action act;
      act.account = config::system_account_name;
      act.name    = name;
      act.data    = sysio_abi_ser.variant_to_binary(
                      action_type_name,
                      data,
                      abi_serializer::create_yield_function(abi_serializer_max_time)
                    );

      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   transaction_trace_ptr push_system_action_trace( const account_name& signer,
                                                  const action_name& name,
                                                  const variant_object& data ) {
      return base_tester::push_action(
         config::system_account_name,
         name,
         vector<permission_level>{{ signer, "active"_n }},
         data
      );
   }

   transaction_trace_ptr push_roa_action_trace( const account_name& signer,
                                                const action_name& name,
                                                const variant_object& data ) {
      return base_tester::push_action(
         ROA,
         name,
         vector<permission_level>{{ signer, "active"_n }},
         data
      );
   }

   action_result push_token_action( const account_name& signer,
                                 const action_name& name,
                                 const variant_object& data ) {
      const std::string action_type_name = token_abi_ser.get_action_type(name);

      action act;
      act.account = TOKEN;
      act.name    = name;
      act.data    = token_abi_ser.variant_to_binary(
                      action_type_name, data,
                      abi_serializer::create_yield_function(abi_serializer_max_time)
                    );

      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   action_result token_create( account_name issuer, asset maximum_supply ) {
      // signer is sysio.token (matches sysio.token_tests.cpp)
      return push_token_action( TOKEN, "create"_n, mvo()
         ("issuer", issuer)
         ("maximum_supply", maximum_supply)
      );
   }

   action_result token_issue_to_self( account_name issuer, asset quantity, const std::string& memo ) {
      // signer is issuer, and token contract enforces "to == issuer" (matches sysio.token_tests.cpp)
      return push_token_action( issuer, "issue"_n, mvo()
         ("to", issuer)
         ("quantity", quantity)
         ("memo", memo)
      );
   }

   fc::variant get_roa_policy( account_name policy_owner, account_name issuer ) {
      // policies table: code=sysio.roa, scope=issuer, table=policies, primary_key=policy_owner
      auto data = get_row_by_account(ROA, issuer, "policies"_n, policy_owner);
      if (data.empty()) return fc::variant();
      return roa_abi_ser.binary_to_variant(
         "policies", data,
         abi_serializer::create_yield_function(abi_serializer_max_time)
      );
   }

public:
   // =============================================================================
   // sysio.opreg action helpers
   // =============================================================================

   action_result register_operator(account_name account,
                                    OperatorType type,
                                    bool is_bootstrapped) {
      return push_opreg_action(OPREG, "regoperator"_n, mvo()
         ("account", account)
         ("type", type)
         ("is_bootstrapped", is_bootstrapped)
      );
   }

   // Permanently slash an operator. Callable only by sysio.chalg per opreg auth.
   action_result slash_operator(account_name account, const std::string& reason = "test slash") {
      return push_opreg_action(CHALG, "slash"_n, mvo()
         ("account", account)
         ("reason", reason)
      );
   }

   // =============================================================================
   // sysio.epoch action helpers
   // =============================================================================

   // Initialize sysio.epoch with a minimum viable configuration. Values are chosen
   // so emissions tests can advance the epoch index without having to populate
   // 21 batch operator accounts -- batch_op_groups is empty until initgroups is
   // called, and payepoch tolerates an empty rotation group (the batch-op share
   // simply rolls to treasury, which is what we want in producer-focused tests).
   //
   // Default epoch_duration_sec is the contract minimum (MIN_EPOCH_DURATION_SEC =
   // 60s). Tests cross the wall-clock boundary by produce_blocks(120). Lower
   // values are rejected by sysio.epoch::setconfig.
   action_result init_epoch_state(uint32_t epoch_duration_sec = 60,
                                   uint32_t operators_per_epoch = 7,
                                   uint32_t batch_op_groups_count = 3) {
      return push_epoch_action(EPOCH, "setconfig"_n, mvo()
         ("epoch_duration_sec", epoch_duration_sec)
         ("operators_per_epoch", operators_per_epoch)
         ("batch_operator_minimum_active", operators_per_epoch * batch_op_groups_count)
         ("batch_op_groups", batch_op_groups_count)
         ("epoch_retention_envelope_log_count", 1000u)
      );
   }

   // Advance sysio.epoch's current_epoch_index. At genesis (index 0) advance is
   // permissionless; post-genesis it requires sysio.msgch authorization. The
   // epoch contract also clamps on wall-clock (next_epoch_start), so tests
   // must produce_blocks enough to cross the epoch duration boundary before
   // calling this for a second+ advance.
   action_result advance_epoch_state(account_name signer = EPOCH) {
      return push_epoch_action(signer, "advance"_n, mvo());
   }

   // Convenience: set epoch config only. Genesis advance is deferred so each
   // test can decide whether to initt5 (and thus pass the emissions gate) or
   // exercise gate-block behavior. Under the new model, the first
   // advance_epoch_state ALSO fires payepoch inline -- which requires t5state
   // to exist, so initt5 must precede any successful advance.
   void bootstrap_epoch() {
      BOOST_REQUIRE_EQUAL( success(), init_epoch_state() );
      produce_blocks(1);
   }

   // =============================================================================
   // opreg / epoch table readers
   // =============================================================================

   fc::variant get_opreg_operator(account_name account) {
      auto data = get_row_by_account(OPREG, OPREG, "operators"_n, account);
      if (data.empty()) return fc::variant();
      return opreg_abi_ser.binary_to_variant("operator_entry", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   fc::variant get_epoch_state_row() {
      auto data = get_row_by_account(EPOCH, EPOCH, "epochstate"_n, "epochstate"_n);
      if (data.empty()) return fc::variant();
      return epoch_abi_ser.binary_to_variant("epoch_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

private:
   action_result push_opreg_action(account_name signer, action_name act, const variant_object& data) {
      try {
         base_tester::push_action(OPREG, act, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   action_result push_epoch_action(account_name signer, action_name act, const variant_object& data) {
      try {
         base_tester::push_action(EPOCH, act, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
   }

   abi_serializer sysio_abi_ser;
   abi_serializer roa_abi_ser;
   abi_serializer token_abi_ser;
   abi_serializer opreg_abi_ser;
   abi_serializer epoch_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_emissions_tests)

// -----------------------------------------------------------------------------
// setinittime
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( setinittime_requires_sysio_auth, sysio_emissions_tester ) try {
   // setinittime requires sysio.system's authority (require_auth(get_self()))
   create_user_accounts({ "alice"_n });

   auto r = setinittime( "alice"_n, tpsec(head_secs()) );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setinittime_singleton_write_and_reprotect, sysio_emissions_tester ) try {
   // First call should initialize the singleton, second call should be blocked.
   auto before = get_emission_state();
   BOOST_REQUIRE( before.is_null() );

   const uint32_t start = head_secs(); // must be > 0 to pass compute checks elsewhere
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );

   auto after = get_emission_state();
   BOOST_REQUIRE( !after.is_null() );
   BOOST_REQUIRE( after.is_object() );
   BOOST_REQUIRE_EQUAL( after["node_rewards_start"].as<time_point_sec>().sec_since_epoch(), start );

   auto r = setinittime( config::system_account_name, tpsec(start) );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "emission state already initialized" );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// addnodeowner
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( addnodeowner_requires_sysio_roa_auth, sysio_emissions_tester ) try {
   create_user_accounts({ "alice"_n, "nodeowner2"_n });

   auto r = addnodeowner( "alice"_n, "nodeowner2"_n, 1 );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio.roa" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( addnodeowner_rejects_invalid_tier, sysio_emissions_tester ) try {
   // Tier must be 1..3
   create_user_accounts({ "nodeowner3"_n });

   auto r1 = addnodeowner( ROA, "nodeowner3"_n, 0 );
   BOOST_REQUIRE( r1 != success() );
   require_substr( r1, "invalid tier" );

   auto r2 = addnodeowner( ROA, "nodeowner3"_n, 4 );
   BOOST_REQUIRE( r2 != success() );
   require_substr( r2, "invalid tier" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( addnodeowner_writes_expected_rows_for_each_tier, sysio_emissions_tester ) try {
   // Valid tiers should create a row in nodedist with expected allocations/durations.
   // Also verifies uniqueness constraint (account already exists).
   create_user_accounts({ "t1"_n, "t2"_n, "t3"_n });

   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "t1"_n, 1 ) );
   auto r1 = get_nodedist_row("t1"_n);
   BOOST_REQUIRE( !r1.is_null() );
   BOOST_REQUIRE_EQUAL( r1["account_name"].as<name>(), "t1"_n );
   BOOST_REQUIRE_EQUAL( r1["total_allocation"].as<asset>(), T1_ALLOCATION );
   BOOST_REQUIRE_EQUAL( r1["claimed"].as<asset>(), asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE_EQUAL( r1["total_duration"].as<uint32_t>(), T1_DURATION );

   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "t2"_n, 2 ) );
   auto r2 = get_nodedist_row("t2"_n);
   BOOST_REQUIRE( !r2.is_null() );
   BOOST_REQUIRE_EQUAL( r2["total_allocation"].as<asset>(), T2_ALLOCATION );
   BOOST_REQUIRE_EQUAL( r2["claimed"].as<asset>(), asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE_EQUAL( r2["total_duration"].as<uint32_t>(), T2_DURATION );

   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "t3"_n, 3 ) );
   auto r3 = get_nodedist_row("t3"_n);
   BOOST_REQUIRE( !r3.is_null() );
   BOOST_REQUIRE_EQUAL( r3["total_allocation"].as<asset>(), T3_ALLOCATION );
   BOOST_REQUIRE_EQUAL( r3["claimed"].as<asset>(), asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE_EQUAL( r3["total_duration"].as<uint32_t>(), T3_DURATION );

   auto dup = addnodeowner( ROA, "t3"_n, 3 );
   BOOST_REQUIRE( dup != success() );
   require_substr( dup, "account already registered" );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Per-tier registration cap (T1=21, T2=84, T3=1000)
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( addnodeowner_t1_cap_rejects_22nd, sysio_emissions_tester ) try {
   // T1_MAX_NODE_OWNERS = 21. Register 21 successfully; 22nd must be rejected
   // with "t1 node owner cap reached". node_count_state singleton tracks the
   // running count.
   std::vector<account_name> names;
   const char digits[] = "abcdefghijklmnopqrstuvwxyz"; // sysio names: a-z
   for (uint32_t i = 0; i < 22; ++i) {
      std::string s = "nt1x";
      s += digits[i / 26];
      s += digits[i % 26];
      names.push_back(account_name(s));
   }
   create_accounts(names, false, false, false, true);
   produce_blocks(1);

   for (uint32_t i = 0; i < 21; ++i) {
      BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, names[i], 1 ) );
   }

   auto r = addnodeowner( ROA, names[21], 1 );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "t1 node owner cap reached" );

   // T2 / T3 still register: tier counts are independent.
   create_user_accounts({ "nt2caps"_n });
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nt2caps"_n, 2 ) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( addnodeowner_invalid_tier_above_max, sysio_emissions_tester ) try {
   // Coverage for the upper-bound side of the tier check: tier 100 / 255
   // (uint8_t max) must fail "invalid tier" the same as 0 / 4 / 5.
   create_user_accounts({ "nthi100"_n, "nthi255"_n });

   auto r100 = addnodeowner( ROA, "nthi100"_n, 100 );
   BOOST_REQUIRE( r100 != success() );
   require_substr( r100, "invalid tier" );

   auto r255 = addnodeowner( ROA, "nthi255"_n, 255 );
   BOOST_REQUIRE( r255 != success() );
   require_substr( r255, "invalid tier" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( addnodeowner_increments_node_count_per_tier, sysio_emissions_tester ) try {
   // Verify the nodecount singleton updates correctly across mixed tiers and
   // that all three counters are independent.
   create_user_accounts({ "ncntt1a"_n, "ncntt1b"_n, "ncntt2a"_n, "ncntt3a"_n, "ncntt3b"_n });

   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "ncntt1a"_n, 1 ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "ncntt2a"_n, 2 ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "ncntt3a"_n, 3 ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "ncntt1b"_n, 1 ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "ncntt3b"_n, 3 ) );

   auto row = get_node_count_state();
   BOOST_REQUIRE( !row.is_null() );
   BOOST_REQUIRE_EQUAL( row["t1_count"].as<uint32_t>(), 2u );
   BOOST_REQUIRE_EQUAL( row["t2_count"].as<uint32_t>(), 1u );
   BOOST_REQUIRE_EQUAL( row["t3_count"].as<uint32_t>(), 2u );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// viewnodedist / claimnodedis
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( no_vesting_yet_start_in_future_blocks_claim, sysio_emissions_tester ) try {
   // If start time is in the future, elapsed==0 => claimable==0 and can_claim==false.
   create_user_accounts({ "nodefuture"_n });

   const uint32_t start_future = head_secs() + 10'000;
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start_future) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodefuture"_n, 1 ) );

   auto info = viewnodedist( "nodefuture"_n );
   BOOST_REQUIRE_EQUAL(info.total_allocation, T1_ALLOCATION);
   BOOST_REQUIRE_EQUAL( info.claimed, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE_EQUAL( info.claimable, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE( !info.can_claim );

   const asset sys_before  = get_wire_balance(config::system_account_name);
   const asset user_before = get_wire_balance("nodefuture"_n);

   auto r = claimnodedis( "nodefuture"_n, "nodefuture"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "claim amount below minimum threshold" );

   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name), sys_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance("nodefuture"_n), user_before );

   // Ensure claim did not mutate table
   auto row = get_nodedist_row("nodefuture"_n);
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), asset(0, WIRE_SYMBOL) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( mid_vesting_claimable_grows_but_gate_blocks_until_min_threshold, sysio_emissions_tester ) try {
   create_user_accounts({ "nodemid"_n });

   // Use Tier 3 so MIN_CLAIMABLE is not reached quickly.
   // Only ~60s vested => claimable > 0 but still < MIN.
   const uint32_t now   = head_secs();
   const uint32_t start = now - 60;

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodemid"_n, 3 ) );

   auto info1 = viewnodedist( "nodemid"_n );
   BOOST_REQUIRE_EQUAL(info1.total_allocation, T3_ALLOCATION);
   BOOST_REQUIRE_EQUAL( info1.claimed, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE( info1.claimable.get_amount() > 0 );
   BOOST_REQUIRE( info1.claimable.get_amount() < MIN_CLAIMABLE_AMOUNT );
   BOOST_REQUIRE( !info1.can_claim );

   // Move time forward a bit; claimable should increase but still stay below MIN.
   produce_blocks(200);

   auto info2 = viewnodedist( "nodemid"_n );
   BOOST_REQUIRE_EQUAL( info2.claimed, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE( info2.claimable.get_amount() > info1.claimable.get_amount() );
   BOOST_REQUIRE( info2.claimable.get_amount() < MIN_CLAIMABLE_AMOUNT );
   BOOST_REQUIRE( !info2.can_claim );

   auto r = claimnodedis( "nodemid"_n, "nodemid"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "claim amount below minimum threshold" );

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( full_vesting_allows_claim_then_blocks_second_claim, sysio_emissions_tester ) try {
   // If elapsed >= duration, compute_node_claim clamps elapsed to duration and makes all remaining claimable.
   // Then claimnodedis should update claimed to equal total_allocation and block subsequent claims.
   create_user_accounts({ "nodefull"_n });

   const uint32_t start = head_secs() - (T1_DURATION + 10);
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodefull"_n, 1 ) );

   auto before = viewnodedist( "nodefull"_n );
   BOOST_REQUIRE_EQUAL(before.total_allocation, T1_ALLOCATION);
   BOOST_REQUIRE_EQUAL( before.claimed, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE_EQUAL( before.claimable, before.total_allocation );
   BOOST_REQUIRE( before.can_claim );

   // Get initial $WIRE balance
   const asset sys_before  = get_wire_balance(config::system_account_name);
   const asset user_before = get_wire_balance("nodefull"_n);
   const asset expected    = before.claimable;

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodefull"_n, "nodefull"_n ) );
   produce_blocks(1);

   // Ensure sysio.token transfer succeeds.
   BOOST_REQUIRE_EQUAL( get_wire_balance("nodefull"_n), user_before + expected );
   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name), sys_before - expected );

   auto row = get_nodedist_row("nodefull"_n);
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), row["total_allocation"].as<asset>() );

   auto after = viewnodedist( "nodefull"_n );
   BOOST_REQUIRE_EQUAL( after.claimable, asset(0, WIRE_SYMBOL) );

   // Snapshot of balance before expected failure to claim.
   const asset sys_before2  = get_wire_balance(config::system_account_name);
   const asset user_before2 = get_wire_balance("nodefull"_n);

   auto r = claimnodedis( "nodefull"_n, "nodefull"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "all node owner rewards already claimed" );

   // Ensure $WIRE balance didn't change after failed claim
   BOOST_REQUIRE_EQUAL( get_wire_balance("nodefull"_n), user_before2 );
   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name), sys_before2 );

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( claimnodedis_requires_self_auth, sysio_emissions_tester ) try {
   // claimnodedis requires_auth(account_name)
   create_user_accounts({ "alice"_n, "bob"_n });

   const uint32_t start = head_secs() - (T1_DURATION + 10);
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "alice"_n, 1 ) );

   auto r = claimnodedis( "bob"_n, "alice"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of alice" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( final_vesting_allows_small_final_remainder_below_min_threshold, sysio_emissions_tester ) try {
   create_user_accounts({ "nodesmall"_n });

   // Make vesting almost complete, but not quite:
   // elapsed = duration - 10 seconds (so first claim leaves a small remainder)
   const uint32_t now   = head_secs();
   const uint32_t start = now - (T1_DURATION - 10);

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodesmall"_n, 1 ) );

   // First claim: should be large and allowed (>= MIN)
   auto info_pre = viewnodedist( "nodesmall"_n );
   BOOST_REQUIRE_EQUAL( info_pre.claimed, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE( info_pre.can_claim );
   BOOST_REQUIRE( info_pre.claimable.get_amount() >= MIN_CLAIMABLE_AMOUNT );

   const asset sys_before1  = get_wire_balance( config::system_account_name );
   const asset user_before1 = get_wire_balance( "nodesmall"_n );
   const asset claimed1     = info_pre.claimable;

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodesmall"_n, "nodesmall"_n ) );
   produce_blocks(1);

   auto row_after_first = get_nodedist_row( "nodesmall"_n );
   BOOST_REQUIRE( !row_after_first.is_null() );

   const auto claimed_after_first = row_after_first["claimed"].as<asset>();
   const auto total_after_first   = row_after_first["total_allocation"].as<asset>();

   BOOST_REQUIRE( claimed_after_first > asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE( claimed_after_first < total_after_first );

   BOOST_REQUIRE_EQUAL( get_wire_balance("nodesmall"_n), user_before1 + claimed1 );
   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name), sys_before1 - claimed1 );

   // Advance past the remaining ~10 seconds to reach full vesting.
   // (Assuming 0.5s block interval; 25 blocks ~ 12.5s)
   produce_blocks(25);

   // Final remainder: should be >0 but < MIN, and allowed because elapsed == duration.
   auto info_final = viewnodedist( "nodesmall"_n );
   BOOST_REQUIRE( info_final.can_claim );
   BOOST_REQUIRE( info_final.claimable.get_amount() > 0 );
   BOOST_REQUIRE( info_final.claimable.get_amount() < MIN_CLAIMABLE_AMOUNT );

   const asset sys_before2  = get_wire_balance( config::system_account_name );
   const asset user_before2 = get_wire_balance( "nodesmall"_n );
   const asset remainder    = info_final.claimable;

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodesmall"_n, "nodesmall"_n ) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( get_wire_balance("nodesmall"_n), user_before2 + remainder );
   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name), sys_before2 - remainder );

   // Table shows fully claimed.
   auto row = get_nodedist_row( "nodesmall"_n );
   BOOST_REQUIRE( !row.is_null() );
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), row["total_allocation"].as<asset>() );

} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// sysio.roa::forcereg wiring -> inline sysio.system::addnodeowner
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( forcereg_inlines_addnodeowner_and_writes_nodedist, sysio_emissions_tester ) try {
   // forcereg should inline sysio.system::addnodeowner under sysio.roa authority,
   // resulting in a nodedist row for the registered owner.
   create_user_accounts({ "emissinline"_n });

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

   auto trace = forcereg_trace( ROA, "emissinline"_n, 1 );
   BOOST_REQUIRE(trace);
   BOOST_REQUIRE(!trace->except);

   bool saw_inline = false;
   for (const auto& at : trace->action_traces) {
      if (at.receiver == config::system_account_name &&
          at.act.account == config::system_account_name &&
          at.act.name == "addnodeowner"_n) {
         saw_inline = true;
         break;
      }
   }
   BOOST_REQUIRE( saw_inline );

   auto row = get_nodedist_row("emissinline"_n);
   BOOST_REQUIRE( !row.is_null() );
   BOOST_REQUIRE_EQUAL( row["account_name"].as<name>(), "emissinline"_n );
   BOOST_REQUIRE_EQUAL( row["total_allocation"].as<asset>(), T1_ALLOCATION );
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE_EQUAL( row["total_duration"].as<uint32_t>(), T1_DURATION );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( forcereg_duplicate_is_blocked_by_roa, sysio_emissions_tester ) try {
   // ROA maintains its own registration table; calling forcereg twice should fail in ROA.
   // We also advance blocks to avoid duplicate-trx-id issues due to identical TAPOS.
   create_user_accounts({ "emissdup"_n });

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

   auto t1 = forcereg_trace( ROA, "emissdup"_n, 1 );
   BOOST_REQUIRE(t1);
   BOOST_REQUIRE(!t1->except);

   // Avoid "Duplicate transaction" by moving TAPOS window forward.
   produce_blocks(2);

   // Expected ROA error message (per sysio.roa contract).
   BOOST_REQUIRE_EXCEPTION(
      forcereg_trace( ROA, "emissdup"_n, 1 ),
      sysio_assert_message_exception,
      sysio_assert_message_is("This account is already registered.")
   );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Error paths: missing prerequisites
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( claimnodedis_fails_before_setinittime, sysio_emissions_tester ) try {
   create_user_accounts({ "nodenostart"_n });
   // Add node owner without calling setinittime first
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodenostart"_n, 1 ) );

   auto r = claimnodedis( "nodenostart"_n, "nodenostart"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "emission state not initialized" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewnodedist_fails_before_setinittime, sysio_emissions_tester ) try {
   create_user_accounts({ "nodenoview"_n });
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodenoview"_n, 2 ) );

   // viewnodedist should throw — we catch the assert
   BOOST_REQUIRE_EXCEPTION(
      viewnodedist( "nodenoview"_n ),
      sysio_assert_message_exception,
      sysio_assert_message_is("emission state not initialized")
   );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( claimnodedis_fails_for_nonexistent_account, sysio_emissions_tester ) try {
   create_user_accounts({ "nobody"_n });
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

   auto r = claimnodedis( "nobody"_n, "nobody"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "account is not a node owner" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewnodedist_fails_for_nonexistent_account, sysio_emissions_tester ) try {
   create_user_accounts({ "nobody2"_n });
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

   BOOST_REQUIRE_EXCEPTION(
      viewnodedist( "nobody2"_n ),
      sysio_assert_message_exception,
      sysio_assert_message_is("account is not a node owner")
   );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Mid-vesting partial claim (above MIN threshold)
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( mid_vesting_claim_above_min_succeeds, sysio_emissions_tester ) try {
   // T1: 7.5M WIRE over 12 months. After ~50% elapsed, claimable ~3.75M WIRE >> MIN (10 WIRE).
   create_user_accounts({ "nodehalf"_n });

   const uint32_t half_elapsed = T1_DURATION / 2;
   const uint32_t start = head_secs() - half_elapsed;

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodehalf"_n, 1 ) );

   auto info = viewnodedist( "nodehalf"_n );
   BOOST_REQUIRE( info.can_claim );
   BOOST_REQUIRE( info.claimable.get_amount() >= MIN_CLAIMABLE_AMOUNT );
   // Approximately half of T1_ALLOCATION
   BOOST_REQUIRE( info.claimable.get_amount() > T1_ALLOCATION.get_amount() / 3 );
   BOOST_REQUIRE( info.claimable.get_amount() < T1_ALLOCATION.get_amount() * 2 / 3 );

   const asset sys_before  = get_wire_balance(config::system_account_name);
   const asset user_before = get_wire_balance("nodehalf"_n);
   const asset expected    = info.claimable;

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodehalf"_n, "nodehalf"_n ) );
   produce_blocks(1);

   // Token transfer succeeded
   BOOST_REQUIRE_EQUAL( get_wire_balance("nodehalf"_n), user_before + expected );
   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name), sys_before - expected );

   // Table updated correctly
   auto row = get_nodedist_row("nodehalf"_n);
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), expected );
   BOOST_REQUIRE( row["claimed"].as<asset>() < row["total_allocation"].as<asset>() );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Multiple sequential partial claims
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( multiple_sequential_partial_claims, sysio_emissions_tester ) try {
   // Claim at 25%, 50%, 75%, and 100% of vesting — all should succeed,
   // and cumulative claimed should equal total_allocation at the end.
   create_user_accounts({ "nodeseq"_n });

   // Start T1 at 25% elapsed
   const uint32_t quarter = T1_DURATION / 4;
   const uint32_t start = head_secs() - quarter;

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodeseq"_n, 1 ) );

   int64_t total_claimed = 0;

   // Claim 1: at ~25% vesting
   auto info1 = viewnodedist( "nodeseq"_n );
   BOOST_REQUIRE( info1.can_claim );
   BOOST_REQUIRE( info1.claimable.get_amount() > 0 );

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodeseq"_n, "nodeseq"_n ) );
   produce_blocks(1);
   total_claimed += info1.claimable.get_amount();

   auto row1 = get_nodedist_row("nodeseq"_n);
   BOOST_REQUIRE_EQUAL( row1["claimed"].as<asset>().get_amount(), total_claimed );

   // Advance another ~25% of duration
   produce_block( fc::seconds(quarter) );

   // Claim 2: at ~50% vesting
   auto info2 = viewnodedist( "nodeseq"_n );
   BOOST_REQUIRE( info2.can_claim );
   BOOST_REQUIRE( info2.claimable.get_amount() > 0 );

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodeseq"_n, "nodeseq"_n ) );
   produce_blocks(1);
   total_claimed += info2.claimable.get_amount();

   auto row2 = get_nodedist_row("nodeseq"_n);
   BOOST_REQUIRE_EQUAL( row2["claimed"].as<asset>().get_amount(), total_claimed );

   // Advance past full duration
   produce_block( fc::seconds(T1_DURATION) );

   // Claim 3: final claim at 100% vesting
   auto info3 = viewnodedist( "nodeseq"_n );
   BOOST_REQUIRE( info3.can_claim );
   BOOST_REQUIRE( info3.claimable.get_amount() > 0 );

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodeseq"_n, "nodeseq"_n ) );
   produce_blocks(1);
   total_claimed += info3.claimable.get_amount();

   // After full vesting + final claim, total claimed == total allocation
   auto row3 = get_nodedist_row("nodeseq"_n);
   BOOST_REQUIRE_EQUAL( row3["claimed"].as<asset>(), row3["total_allocation"].as<asset>() );
   BOOST_REQUIRE_EQUAL( total_claimed, T1_ALLOCATION.get_amount() );

   // Subsequent claim should fail
   auto r = claimnodedis( "nodeseq"_n, "nodeseq"_n );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "all node owner rewards already claimed" );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Tier-specific full claim flows (T2 and T3)
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( tier2_full_vesting_claim, sysio_emissions_tester ) try {
   create_user_accounts({ "nodet2"_n });

   const uint32_t start = head_secs() - (T2_DURATION + 10);
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodet2"_n, 2 ) );

   auto info = viewnodedist( "nodet2"_n );
   BOOST_REQUIRE_EQUAL( info.total_allocation, T2_ALLOCATION );
   BOOST_REQUIRE_EQUAL( info.claimable, T2_ALLOCATION );
   BOOST_REQUIRE( info.can_claim );

   const asset user_before = get_wire_balance("nodet2"_n);
   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodet2"_n, "nodet2"_n ) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( get_wire_balance("nodet2"_n), user_before + T2_ALLOCATION );

   auto row = get_nodedist_row("nodet2"_n);
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), T2_ALLOCATION );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( tier3_full_vesting_claim, sysio_emissions_tester ) try {
   create_user_accounts({ "nodet3"_n });

   const uint32_t start = head_secs() - (T3_DURATION + 10);
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodet3"_n, 3 ) );

   auto info = viewnodedist( "nodet3"_n );
   BOOST_REQUIRE_EQUAL( info.total_allocation, T3_ALLOCATION );
   BOOST_REQUIRE_EQUAL( info.claimable, T3_ALLOCATION );
   BOOST_REQUIRE( info.can_claim );

   const asset user_before = get_wire_balance("nodet3"_n);
   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodet3"_n, "nodet3"_n ) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( get_wire_balance("nodet3"_n), user_before + T3_ALLOCATION );

   auto row = get_nodedist_row("nodet3"_n);
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), T3_ALLOCATION );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Linear vesting precision
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( linear_vesting_precision_at_known_fraction, sysio_emissions_tester ) try {
   // Verify exact vested amount at 1/4 of T1 duration.
   // Expected: T1_ALLOCATION * (T1_DURATION/4) / T1_DURATION = T1_ALLOCATION / 4
   create_user_accounts({ "nodeprec"_n });

   const uint32_t quarter = T1_DURATION / 4;
   const uint32_t start = head_secs() - quarter;

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodeprec"_n, 1 ) );

   auto info = viewnodedist( "nodeprec"_n );

   // Expected vested: total_amount * quarter / T1_DURATION
   // Use __int128 for precision matching
   __int128 expected_vested = static_cast<__int128>(T1_ALLOCATION.get_amount()) *
                              static_cast<__int128>(quarter) / T1_DURATION;
   int64_t expected = static_cast<int64_t>(expected_vested);

   // claimable == expected (within 1 subunit tolerance for block time rounding)
   int64_t diff = info.claimable.get_amount() - expected;
   // Block time can advance slightly beyond quarter, so claimable >= expected
   BOOST_REQUIRE( diff >= 0 );
   // But not by more than a few seconds' worth of vesting
   // T1: 7.5M WIRE over ~31M seconds ≈ 0.24 WIRE/sec ≈ 24000000 subunits/sec
   // Allow up to 5 seconds of drift
   int64_t per_sec = T1_ALLOCATION.get_amount() / T1_DURATION;
   BOOST_REQUIRE( diff <= per_sec * 5 );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// Token balance conservation across claims
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( token_conservation_across_claims, sysio_emissions_tester ) try {
   // After any claim, sysio_loss == user_gain == claimable amount
   create_user_accounts({ "nodecons"_n });

   const uint32_t third = T1_DURATION / 3;
   const uint32_t start = head_secs() - third;

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "nodecons"_n, 1 ) );

   // Claim 1
   auto info1 = viewnodedist( "nodecons"_n );
   BOOST_REQUIRE( info1.can_claim );

   asset sys_before  = get_wire_balance(config::system_account_name);
   asset user_before = get_wire_balance("nodecons"_n);

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodecons"_n, "nodecons"_n ) );
   produce_blocks(1);

   asset sys_after  = get_wire_balance(config::system_account_name);
   asset user_after = get_wire_balance("nodecons"_n);

   // Conservation: sysio lost == user gained == claimable
   asset sysio_loss = sys_before - sys_after;
   asset user_gain  = user_after - user_before;
   BOOST_REQUIRE_EQUAL( sysio_loss, user_gain );
   BOOST_REQUIRE_EQUAL( sysio_loss, info1.claimable );

   // Advance to full vesting and claim remainder
   produce_block( fc::seconds(T1_DURATION) );

   auto info2 = viewnodedist( "nodecons"_n );
   BOOST_REQUIRE( info2.can_claim );

   sys_before  = get_wire_balance(config::system_account_name);
   user_before = get_wire_balance("nodecons"_n);

   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "nodecons"_n, "nodecons"_n ) );
   produce_blocks(1);

   sys_after  = get_wire_balance(config::system_account_name);
   user_after = get_wire_balance("nodecons"_n);

   sysio_loss = sys_before - sys_after;
   user_gain  = user_after - user_before;
   BOOST_REQUIRE_EQUAL( sysio_loss, user_gain );
   BOOST_REQUIRE_EQUAL( sysio_loss, info2.claimable );

   // Total user balance should be exactly T1_ALLOCATION
   auto row = get_nodedist_row("nodecons"_n);
   BOOST_REQUIRE_EQUAL( row["claimed"].as<asset>(), T1_ALLOCATION );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// setemitcfg
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( setemitcfg_requires_sysio_auth, sysio_emissions_tester ) try {
   create_user_accounts({ "alice"_n });

   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(10000)) ("capital_bps", uint16_t(0)) ("capex_bps", uint16_t(0)) ("governance_bps", uint16_t(0))
      ("producer_bps", uint16_t(5000)) ("batch_op_bps", uint16_t(5000))
      ("standby_end_rank", uint32_t(28))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg("alice"_n, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_bad_category_bps, sysio_emissions_tester ) try {
   // Category BPS must sum to 10000
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(5000)) ("capital_bps", uint16_t(3000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(500))
      ("producer_bps", uint16_t(5000)) ("batch_op_bps", uint16_t(5000))
      ("standby_end_rank", uint32_t(28))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "category BPS must sum to 10000" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_bad_compute_subsplit, sysio_emissions_tester ) try {
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(4000)) ("capital_bps", uint16_t(3000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(6000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(28))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "compute sub-split BPS must sum to 10000" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_zero_duration, sysio_emissions_tester ) try {
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(0))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(4000)) ("capital_bps", uint16_t(3000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(28))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "t1_duration must be positive" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_invalid_decay_target, sysio_emissions_tester ) try {
   // target_annual_decay_bps must be in (0, 10000]; both extremes rejected.
   auto build_cfg = [&](uint16_t target_bps) {
      return mvo()
         ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
         ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
         ("min_claimable", int64_t(0))
         ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
         ("target_annual_decay_bps", target_bps)
         ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
         ("compute_bps", uint16_t(4000)) ("capital_bps", uint16_t(3000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
         ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
         ("standby_end_rank", uint32_t(28))
         ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));
   };

   auto r0 = setemitcfg(config::system_account_name, build_cfg(0));
   BOOST_REQUIRE( r0 != success() );
   require_substr( r0, "target_annual_decay_bps must be in (0, 10000]" );

   auto r_high = setemitcfg(config::system_account_name, build_cfg(10001));
   BOOST_REQUIRE( r_high != success() );
   require_substr( r_high, "target_annual_decay_bps must be in (0, 10000]" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_round_to_zero_per_epoch, sysio_emissions_tester ) try {
   // Annual values that scale to 0 at the canonical epoch_duration_sec would
   // silently disable emissions (gate sees emission_amount = 0 and blocks
   // every advance). With the test fixture's 60-sec epoch, annual values
   // below SECONDS_PER_YEAR/60 = 525600 round down to 0; the contract must
   // reject such configs at setemitcfg time.
   auto build_cfg = [&](int64_t annual_initial) {
      return mvo()
         ("t1_allocation", T1_ALLOCATION.get_amount())
         ("t2_allocation", T2_ALLOCATION.get_amount())
         ("t3_allocation", T3_ALLOCATION.get_amount())
         ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
         ("min_claimable", MIN_CLAIMABLE_AMOUNT)
         ("t5_distributable", T5_DISTRIBUTABLE) ("t5_floor", int64_t(125000000000000000LL))
         ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
         ("annual_initial_emission", annual_initial)
         ("annual_max_emission", ANNUAL_MAX_EMISSION)
         ("annual_min_emission", int64_t(0))
         ("compute_bps", COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
         ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
         ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
         ("standby_end_rank", T_STANDBY_END_RANK)
         ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));
   };

   // annual_initial = 1 scales to 0 at 60s -> reject.
   auto r_tiny = setemitcfg(config::system_account_name, build_cfg(int64_t(1)));
   BOOST_REQUIRE( r_tiny != success() );
   require_substr( r_tiny, "annual_initial_emission per-epoch share rounds to 0" );

   // annual_initial = SECONDS_PER_YEAR/60 - 1 still rounds to 0.
   auto r_just_under = setemitcfg(config::system_account_name,
                                   build_cfg(int64_t(SECONDS_PER_YEAR / 60 - 1)));
   BOOST_REQUIRE( r_just_under != success() );
   require_substr( r_just_under, "annual_initial_emission per-epoch share rounds to 0" );

   // SECONDS_PER_YEAR/60 scales to exactly 1 -- accepted.
   BOOST_REQUIRE_EQUAL( success(),
      setemitcfg(config::system_account_name, build_cfg(int64_t(SECONDS_PER_YEAR / 60))) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_bad_standby_rank, sysio_emissions_tester ) try {
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(4000)) ("capital_bps", uint16_t(3000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(21))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "standby_end_rank must be >= standby_start_rank" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_standby_rank_over_cap, sysio_emissions_tester ) try {
   // Upper cap on standby_end_rank bounds inline-action count in payepoch.
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(4000)) ("capital_bps", uint16_t(3000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(101))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "standby_end_rank exceeds safety cap" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setinittime_rejects_epoch_zero, sysio_emissions_tester ) try {
   // time_point_sec{} default-constructs to epoch 0; accepting it would brick
   // claim paths permanently via compute_node_claim's start_secs > 0 guard.
   auto r = setinittime( config::system_account_name, tpsec(0) );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "node_rewards_start must be non-zero" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_reconfigurable, sysio_emissions_tester ) try {
   // Change allocation mid-stream; new node owner gets new amount
   auto cfg = mvo()
      ("t1_allocation", int64_t(999000000000))  // much smaller than default
      ("t2_allocation", int64_t(1000000000000000))
      ("t3_allocation", int64_t(100000000000000))
      ("t1_duration", uint32_t(12u * 30u * 24u * 3600u))
      ("t2_duration", uint32_t(24u * 30u * 24u * 3600u))
      ("t3_duration", uint32_t(36u * 30u * 24u * 3600u))
      ("min_claimable", int64_t(10000000000))
      ("t5_distributable", int64_t(375000000000000000LL))
      ("t5_floor", int64_t(125000000000000000LL))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(563150000000000LL * 365))
      ("annual_max_emission", int64_t(3000000000000000LL * 365))
      ("annual_min_emission", int64_t(100000000000000LL * 365))
      ("compute_bps", uint16_t(4000))
      ("capital_bps", uint16_t(3000))
      ("capex_bps", uint16_t(2000))
      ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000))
      ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(28))
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   BOOST_REQUIRE_EQUAL( success(), setemitcfg(config::system_account_name, cfg) );

   // Register a tier-1 node owner after reconfiguration
   create_user_accounts({ "newt1"_n });
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "newt1"_n, 1 ) );

   auto row = get_nodedist_row("newt1"_n);
   BOOST_REQUIRE( !row.is_null() );
   BOOST_REQUIRE_EQUAL( row["total_allocation"].as<asset>(), asset(999000000000, WIRE_SYMBOL) );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// viewemitcfg
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( viewemitcfg_returns_current_config, sysio_emissions_tester ) try {
   // Default config was set in constructor; verify viewemitcfg returns it
   auto cfg = viewemitcfg();
   BOOST_REQUIRE_EQUAL( cfg.t1_allocation, T1_ALLOCATION.get_amount() );
   BOOST_REQUIRE_EQUAL( cfg.t2_allocation, T2_ALLOCATION.get_amount() );
   BOOST_REQUIRE_EQUAL( cfg.t3_allocation, T3_ALLOCATION.get_amount() );
   BOOST_REQUIRE_EQUAL( cfg.t1_duration, T1_DURATION );
   BOOST_REQUIRE_EQUAL( cfg.t2_duration, T2_DURATION );
   BOOST_REQUIRE_EQUAL( cfg.t3_duration, T3_DURATION );
   BOOST_REQUIRE_EQUAL( cfg.min_claimable, MIN_CLAIMABLE_AMOUNT );
   BOOST_REQUIRE_EQUAL( cfg.t5_distributable, T5_DISTRIBUTABLE );
   BOOST_REQUIRE_EQUAL( cfg.target_annual_decay_bps, TARGET_ANNUAL_DECAY_BPS );
   BOOST_REQUIRE_EQUAL( cfg.annual_initial_emission, ANNUAL_INITIAL_EMISSION );
   BOOST_REQUIRE_EQUAL( cfg.annual_max_emission, ANNUAL_MAX_EMISSION );
   BOOST_REQUIRE_EQUAL( cfg.annual_min_emission, ANNUAL_MIN_EMISSION );
   BOOST_REQUIRE_EQUAL( cfg.compute_bps, COMPUTE_BPS );
   BOOST_REQUIRE_EQUAL( cfg.capital_bps, CAPITAL_BPS );
   BOOST_REQUIRE_EQUAL( cfg.capex_bps, CAPEX_BPS );
   BOOST_REQUIRE_EQUAL( cfg.governance_bps, uint16_t(1000) );
   BOOST_REQUIRE_EQUAL( cfg.producer_bps, PRODUCER_BPS );
   BOOST_REQUIRE_EQUAL( cfg.batch_op_bps, uint16_t(3000) );
   BOOST_REQUIRE_EQUAL( cfg.standby_end_rank, T_STANDBY_END_RANK );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewemitcfg_reflects_update, sysio_emissions_tester ) try {
   // Update config and verify viewemitcfg returns new values
   auto cfg = mvo()
      ("t1_allocation", int64_t(42))
      ("t2_allocation", int64_t(43))
      ("t3_allocation", int64_t(44))
      ("t1_duration", uint32_t(100))
      ("t2_duration", uint32_t(200))
      ("t3_duration", uint32_t(300))
      ("min_claimable", int64_t(5))
      ("t5_distributable", int64_t(999))
      ("t5_floor", int64_t(111))
      ("target_annual_decay_bps", uint16_t(5000))
      // Annual values must be large enough that scale_annual_to_epoch at the
      // fixture's 60s epoch (= annual * 60 / 31'536'000) is non-zero.
      ("annual_initial_emission", int64_t(500'000'000))
      ("annual_max_emission", int64_t(1'000'000'000))
      ("annual_min_emission", int64_t(10'000'000))
      ("compute_bps", uint16_t(2500))
      ("capital_bps", uint16_t(2500))
      ("capex_bps", uint16_t(2500))
      ("governance_bps", uint16_t(2500))
      ("producer_bps", uint16_t(5000))
      ("batch_op_bps", uint16_t(5000))
      ("standby_end_rank", uint32_t(30))
      ("epoch_log_retention_count", uint32_t(2880))("pay_cadence_epochs", uint16_t(1));

   BOOST_REQUIRE_EQUAL( success(), setemitcfg(config::system_account_name, cfg) );

   auto result = viewemitcfg();
   BOOST_REQUIRE_EQUAL( result.t1_allocation, int64_t(42) );
   BOOST_REQUIRE_EQUAL( result.t2_allocation, int64_t(43) );
   BOOST_REQUIRE_EQUAL( result.t3_allocation, int64_t(44) );
   BOOST_REQUIRE_EQUAL( result.t1_duration, uint32_t(100) );
   BOOST_REQUIRE_EQUAL( result.target_annual_decay_bps, uint16_t(5000) );
   BOOST_REQUIRE_EQUAL( result.compute_bps, uint16_t(2500) );
   BOOST_REQUIRE_EQUAL( result.producer_bps, uint16_t(5000) );
   BOOST_REQUIRE_EQUAL( result.standby_end_rank, uint32_t(30) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END() // sysio_emissions_tests

// =============================================================================
// T5 Treasury Emissions Tests
// =============================================================================

BOOST_AUTO_TEST_SUITE(t5_emissions_tests)

// Helper: number of seconds in one epoch (24 hours)
static constexpr uint32_t ONE_EPOCH = 86400u;

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( initt5_requires_sysio_auth, sysio_emissions_tester ) try {
   create_user_accounts({ "alice"_n });
   auto r = initt5( "alice"_n, tpsec(head_secs()) );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( initt5_writes_state_and_blocks_reinit, sysio_emissions_tester ) try {
   auto before = get_t5_state();
   BOOST_REQUIRE( before.is_null() );

   const uint32_t start = head_secs();
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   auto after = get_t5_state();
   BOOST_REQUIRE( !after.is_null() );
   BOOST_REQUIRE_EQUAL( after["epoch_count"].as<uint64_t>(), 0u );
   BOOST_REQUIRE_EQUAL( after["total_distributed"].as<int64_t>(), 0 );

   auto r = initt5( config::system_account_name, tpsec(start) );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "t5 state already initialized" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_post_initt5_rejects_brick_reduce, sysio_emissions_tester ) try {
   // After t5_state exists and epochs have run, setemitcfg must reject a
   // t5_distributable reduction that would make remaining (= distributable -
   // floor - total_distributed) negative. Otherwise the treasury silently
   // bricks with EmissionsBlocked on the next advance (gate sees treasury exhausted).
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto cfg = mvo()
      ("t1_allocation", T1_ALLOCATION.get_amount())
      ("t2_allocation", T2_ALLOCATION.get_amount())
      ("t3_allocation", T3_ALLOCATION.get_amount())
      ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable", MIN_CLAIMABLE_AMOUNT)
      // Shrink distributable below already-distributed + floor.
      ("t5_distributable", int64_t(1))
      ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", ANNUAL_INITIAL_EMISSION)
      ("annual_max_emission", ANNUAL_MAX_EMISSION) ("annual_min_emission", ANNUAL_MIN_EMISSION)
      ("compute_bps", COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "t5_distributable must cover floor + already-distributed" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_post_initt5_rejects_unreachable_min_emission, sysio_emissions_tester ) try {
   // After t5_state exists, setemitcfg must also reject an annual_min_emission
   // whose per-epoch share exceeds the remaining distributable budget -- the
   // floor would otherwise drain the treasury faster than the decay curve
   // suggests.
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto state = get_t5_state();
   const int64_t total_distributed = state["total_distributed"].as<int64_t>();

   // Make remaining_distributable a small handful of subunits, then pick an
   // annual_min whose per-epoch share at the fixture's 60-sec epoch duration
   // exceeds it. annual = SECONDS_PER_YEAR scales to exactly epoch_secs (=60)
   // subunits per epoch, comfortably above small_remaining (=10).
   const int64_t small_remaining = 10;
   const int64_t annual_floor = SECONDS_PER_YEAR; // per-epoch at 60s = 60 subunits

   auto cfg = mvo()
      ("t1_allocation", T1_ALLOCATION.get_amount())
      ("t2_allocation", T2_ALLOCATION.get_amount())
      ("t3_allocation", T3_ALLOCATION.get_amount())
      ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable", MIN_CLAIMABLE_AMOUNT)
      ("t5_distributable", int64_t(T5_FLOOR + total_distributed + small_remaining))
      ("t5_floor", T5_FLOOR)
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", int64_t(0))
      ("annual_max_emission", annual_floor)
      ("annual_min_emission", annual_floor)
      ("compute_bps", COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "annual_min_emission per-epoch share exceeds remaining distributable" );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( advance_gate_blocks_before_initt5, sysio_emissions_tester ) try {
   // initt5 has not been called yet -- the readiness gate sees t5state missing
   // and refuses to advance the epoch. advance returns success (no throw),
   // but state.current_epoch_index stays at 0 (or epochstate row not written
   // at all if nothing has ever called set on it) and no payepoch fires.
   create_t5_holding_accounts();

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto est = get_epoch_state_row();
   if (!est.is_null()) {
      BOOST_REQUIRE_EQUAL( est["current_epoch_index"].as<uint32_t>(), 0u );
   }
   // t5state should also still be absent.
   BOOST_REQUIRE( get_t5_state().is_null() );

   // blocklog row records the gate failure with the expected reason.
   // Reason 2 = EMISSIONS_BLOCK_REASON_STATE_UNINITIALIZED.
   auto bl = get_blocklog_row(1u);
   BOOST_REQUIRE( !bl.is_null() );
   BOOST_REQUIRE_EQUAL( bl["epoch_index"].as<uint32_t>(),  1u );
   BOOST_REQUIRE_EQUAL( bl["reason"].as_string(), "EMISSIONS_BLOCK_REASON_STATE_UNINITIALIZED" );
   BOOST_REQUIRE_EQUAL( bl["retry_count"].as<uint32_t>(),  1u );
   // attempted_emission is 0 here -- gate never reached emission compute (no t5state).
   BOOST_REQUIRE_EQUAL( bl["attempted_emission"].as<int64_t>(), 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( gate_block_dedup_same_reason, sysio_emissions_tester ) try {
   // Two consecutive gate-block attempts with the same reason: blocklog row
   // exists, retry_count increments, last_retry_at advances. No outbound
   // queueout is attempted on the second call (reason unchanged), so the
   // trx still succeeds even without sysio.msgch deployed.
   create_t5_holding_accounts();

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto bl1 = get_blocklog_row(1u);
   BOOST_REQUIRE( !bl1.is_null() );
   const uint32_t first_retry_at = bl1["last_retry_at"].as<uint32_t>();

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto bl2 = get_blocklog_row(1u);
   BOOST_REQUIRE( !bl2.is_null() );
   BOOST_REQUIRE_EQUAL( bl2["retry_count"].as<uint32_t>(), 2u );
   BOOST_REQUIRE_GE( bl2["last_retry_at"].as<uint32_t>(), first_retry_at );
   // first_blocked_at MUST NOT change across retries with same reason.
   BOOST_REQUIRE_EQUAL( bl2["first_blocked_at"].as<uint32_t>(),
                        bl1["first_blocked_at"].as<uint32_t>() );
   BOOST_REQUIRE_EQUAL( bl2["reason"].as_string(), "EMISSIONS_BLOCK_REASON_STATE_UNINITIALIZED" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( gate_block_reason_change_updates_row, sysio_emissions_tester ) try {
   // First block: STATE_UNINITIALIZED (no initt5).
   // Second block: switch to TREASURY_EXHAUSTED by initt5'ing AND configuring
   // a tight cfg whose remaining is zero. Reason changes; row is updated and
   // first_blocked_at is preserved (still records the original block time).
   create_t5_holding_accounts();

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto bl1 = get_blocklog_row(1u);
   BOOST_REQUIRE( !bl1.is_null() );
   BOOST_REQUIRE_EQUAL( bl1["reason"].as_string(), "EMISSIONS_BLOCK_REASON_STATE_UNINITIALIZED" );
   const uint32_t orig_blocked_at = bl1["first_blocked_at"].as<uint32_t>();

   // Reconfigure with t5_distributable == t5_floor so remaining is zero.
   auto cfg = mvo()
      ("t1_allocation",          T1_ALLOCATION.get_amount())
      ("t2_allocation",          T2_ALLOCATION.get_amount())
      ("t3_allocation",          T3_ALLOCATION.get_amount())
      ("t1_duration",            T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable",          MIN_CLAIMABLE_AMOUNT)
      ("t5_distributable",       int64_t(125000000000000000LL))
      ("t5_floor",               int64_t(125000000000000000LL))
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", int64_t(0))
      ("annual_max_emission",     ANNUAL_MAX_EMISSION) ("annual_min_emission", int64_t(0))
      ("compute_bps",            COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
      ("capex_bps",              CAPEX_BPS)   ("governance_bps", uint16_t(1000))
      ("producer_bps",           PRODUCER_BPS)("batch_op_bps", uint16_t(3000))
      ("standby_end_rank",       T_STANDBY_END_RANK)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));
   BOOST_REQUIRE_EQUAL( success(), setemitcfg(config::system_account_name, cfg) );
   BOOST_REQUIRE_EQUAL( success(), initt5(config::system_account_name, tpsec(head_secs())) );

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto bl2 = get_blocklog_row(1u);
   BOOST_REQUIRE( !bl2.is_null() );
   BOOST_REQUIRE_EQUAL( bl2["reason"].as_string(), "EMISSIONS_BLOCK_REASON_TREASURY_EXHAUSTED" );
   BOOST_REQUIRE_EQUAL( bl2["first_blocked_at"].as<uint32_t>(), orig_blocked_at );
   BOOST_REQUIRE_EQUAL( bl2["retry_count"].as<uint32_t>(), 2u );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( gate_block_clears_on_unblock, sysio_emissions_tester ) try {
   // Gate fails (no initt5), blocklog row appears, then gate passes after
   // initt5 -- the row for that epoch_index must be erased on the success
   // path so it no longer shows up to ops as "currently blocked".
   create_t5_holding_accounts();

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );
   BOOST_REQUIRE( !get_blocklog_row(1u).is_null() );

   // Unblock by initialising t5 state. Wall clock already past initial
   // next_epoch_start (default 0); next advance crosses the gate.
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5(config::system_account_name, tpsec(start)) );

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   // Epoch advanced to 1 and blocklog[1] was pruned.
   auto est = get_epoch_state_row();
   BOOST_REQUIRE( !est.is_null() );
   BOOST_REQUIRE_EQUAL( est["current_epoch_index"].as<uint32_t>(), 1u );
   BOOST_REQUIRE( get_blocklog_row(1u).is_null() );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( advance_pays_after_epoch_duration, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   // Set start_time in the past so epoch has already elapsed
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto state = get_t5_state();
   BOOST_REQUIRE( !state.is_null() );
   BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );
   BOOST_REQUIRE( state["total_distributed"].as<int64_t>() > 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Emission curve
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( first_epoch_uses_initial_emission, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Test fixture's epoch_duration_sec = 60. Initial per-epoch is the annual
   // value scaled to 60 seconds.
   const int64_t initial_per_epoch =
      test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, 60);

   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["last_epoch_emission"].as<int64_t>(), initial_per_epoch );

   // With no producers registered AND no batch-op rotation group populated,
   // both the producer_pool and the batch_pool stay in the treasury.
   int64_t undist = compute_undistributed_if_no_operators(initial_per_epoch);
   BOOST_REQUIRE_EQUAL( state["total_distributed"].as<int64_t>(), initial_per_epoch - undist );

   auto log = get_epoch_log(1);
   BOOST_REQUIRE( !log.is_null() );
   BOOST_REQUIRE_EQUAL( log["total_emission"].as<int64_t>(), initial_per_epoch );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( subsequent_epochs_apply_decay, sysio_emissions_tester ) try {
   // Use a longer epoch so the per-epoch decay factor is large enough to
   // observe across two epochs. At 60s epochs the factor is ~0.99999930
   // (annual decay applied once per minute), invisible at int64 precision.
   // 86400s (1 day) gives factor ~= 0.999, matching the legacy curve shape.
   constexpr uint32_t EPOCH_SECS = 86400;
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(EPOCH_SECS) );

   create_t5_holding_accounts();
   const uint32_t start = head_secs() - (2 * ONE_EPOCH) - 10;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Epoch 1 (bootstrap_epoch already advanced sysio.epoch to index 1)
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Epoch 2: cross the wall-clock boundary (86400s = 172800 blocks at 0.5s);
   // produce a bit more for safety margin.
   produce_blocks(2 * EPOCH_SECS + 10);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 2u );

   const int64_t initial_per_epoch =
      test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, EPOCH_SECS);
   const int64_t expected_e2 =
      test_apply_decay(initial_per_epoch, TARGET_ANNUAL_DECAY_BPS, EPOCH_SECS);

   BOOST_REQUIRE_EQUAL( state["last_epoch_emission"].as<int64_t>(), expected_e2 );

   // Without producers OR a populated batch-op rotation group, both pools are
   // undistributed in each epoch.
   int64_t undist1 = compute_undistributed_if_no_operators(initial_per_epoch);
   int64_t undist2 = compute_undistributed_if_no_operators(expected_e2);
   BOOST_REQUIRE_EQUAL( state["total_distributed"].as<int64_t>(),
      (initial_per_epoch - undist1) + (expected_e2 - undist2) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( emission_clamped_to_max, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t per_epoch_max = test_scale_annual_to_epoch(ANNUAL_MAX_EMISSION, 60);
   const int64_t per_epoch_min = test_scale_annual_to_epoch(ANNUAL_MIN_EMISSION, 60);

   auto state = get_t5_state();
   BOOST_REQUIRE( state["last_epoch_emission"].as<int64_t>() <= per_epoch_max );
   BOOST_REQUIRE( state["last_epoch_emission"].as<int64_t>() >= per_epoch_min );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( emission_capped_at_distributable_ceiling, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto state = get_t5_state();
   BOOST_REQUIRE( state["total_distributed"].as<int64_t>() <= T5_DISTRIBUTABLE );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Category distribution
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( category_split_matches_basis_points, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   BOOST_REQUIRE( !log.is_null() );

   int64_t total     = log["total_emission"].as<int64_t>();
   int64_t compute   = log["compute_amount"].as<int64_t>();
   int64_t capital   = log["capital_amount"].as<int64_t>();
   int64_t capex     = log["capex_amount"].as<int64_t>();

   BOOST_REQUIRE_EQUAL( compute, test_split_bps(total, COMPUTE_BPS) );
   BOOST_REQUIRE_EQUAL( capital, test_split_bps(total, CAPITAL_BPS) );
   // capex gets only its base split (no producer dust redirect)
   BOOST_REQUIRE_EQUAL( capex, test_split_bps(total, CAPEX_BPS) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( governance_gets_remainder_no_dust_loss, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t total   = log["total_emission"].as<int64_t>();
   int64_t compute = log["compute_amount"].as<int64_t>();
   int64_t capital = log["capital_amount"].as<int64_t>();
   int64_t gov     = log["governance_amount"].as<int64_t>();

   int64_t capex_base = test_split_bps(total, CAPEX_BPS);
   int64_t expected_gov = total - compute - capital - capex_base;
   BOOST_REQUIRE_EQUAL( gov, expected_gov );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Compute distribution (producers)
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( no_producers_undistributed_stays_in_sysio, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const asset sysio_before = get_wire_balance(config::system_account_name);
   const asset capex_before = get_wire_balance("sysio.ops"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t capex_base = test_split_bps(emission, CAPEX_BPS);

   // Capex gets only its base split (no producer dust redirect)
   const asset capex_after = get_wire_balance("sysio.ops"_n);
   int64_t capex_received = capex_after.get_amount() - capex_before.get_amount();
   BOOST_REQUIRE_EQUAL( capex_received, capex_base );

   // sysio's balance decreases by (emission - producer_pool - batch_pool) since
   // both producer and batch-op pools are undistributed when no operators are
   // registered and no batch-op rotation group has been populated.
   const asset sysio_after = get_wire_balance(config::system_account_name);
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   int64_t undist = compute_undistributed_if_no_operators(emission);
   BOOST_REQUIRE_EQUAL( sysio_decrease, emission - undist );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( active_producers_get_equal_share, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);

   // Wait for schedule to activate, then produce complete cycles
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2); // 2 cycles sufficient for eligible_rounds

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);
   asset bal_b_before = get_wire_balance("producerb"_n);
   asset bal_c_before = get_wire_balance("producerc"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance("producera"_n).get_amount() - bal_a_before.get_amount();
   int64_t got_b = get_wire_balance("producerb"_n).get_amount() - bal_b_before.get_amount();
   int64_t got_c = get_wire_balance("producerc"_n).get_amount() - bal_c_before.get_amount();

   // All producers should receive equal payment (same eligible_rounds)
   BOOST_REQUIRE_EQUAL( got_a, got_b );
   BOOST_REQUIRE_EQUAL( got_b, got_c );
   BOOST_REQUIRE( got_a > 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Holding account stub transfers
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( holding_accounts_receive_correct_amounts, sysio_emissions_tester ) try {
   // Category bucket recipients are fixed accounts (sysio.cap / sysio.gov / sysio.ops).
   // The batch-op share is NOT sent to a holding account -- it is split across the
   // members of the current sysio.epoch batch_op_groups rotation slot. When no
   // operators are registered, the entire batch_pool stays in the treasury.
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset cap_before   = get_wire_balance("sysio.cap"_n);
   asset gov_before   = get_wire_balance("sysio.gov"_n);
   asset batch_before = get_wire_balance("sysio.batch"_n);
   asset ops_before   = get_wire_balance("sysio.ops"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t capital  = log["capital_amount"].as<int64_t>();
   int64_t gov      = log["governance_amount"].as<int64_t>();
   int64_t capex_base = test_split_bps(emission, CAPEX_BPS);

   int64_t cap_received   = get_wire_balance("sysio.cap"_n).get_amount()   - cap_before.get_amount();
   int64_t gov_received   = get_wire_balance("sysio.gov"_n).get_amount()   - gov_before.get_amount();
   int64_t batch_received = get_wire_balance("sysio.batch"_n).get_amount() - batch_before.get_amount();
   int64_t ops_received   = get_wire_balance("sysio.ops"_n).get_amount()   - ops_before.get_amount();

   BOOST_REQUIRE_EQUAL( cap_received, capital );
   BOOST_REQUIRE_EQUAL( gov_received, gov );
   // sysio.batch is not an emissions recipient anymore -- batch pay goes to the
   // current rotation group, not a holding account.
   BOOST_REQUIRE_EQUAL( batch_received, 0 );
   BOOST_REQUIRE_EQUAL( ops_received, capex_base );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Read-only
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( viewepoch_returns_correct_state, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t initial_per_epoch =
      test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, 60);
   int64_t undist = compute_undistributed_if_no_operators(initial_per_epoch);
   int64_t expected_distributed = initial_per_epoch - undist;

   auto info = viewepoch();
   BOOST_REQUIRE_EQUAL( info.epoch_count, 1u );
   BOOST_REQUIRE_EQUAL( info.last_epoch_emission, initial_per_epoch );
   BOOST_REQUIRE_EQUAL( info.total_distributed, expected_distributed );
   BOOST_REQUIRE( info.treasury_remaining > 0 );
   BOOST_REQUIRE_EQUAL( info.treasury_remaining, T5_DISTRIBUTABLE - T5_FLOOR - expected_distributed );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewepoch_estimates_next_emission, sysio_emissions_tester ) try {
   // Use a longer epoch so the per-epoch decay factor is visible.
   constexpr uint32_t EPOCH_SECS = 86400;
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(EPOCH_SECS) );

   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto info = viewepoch();

   const int64_t initial_per_epoch =
      test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, EPOCH_SECS);
   const int64_t expected_next =
      test_apply_decay(initial_per_epoch, TARGET_ANNUAL_DECAY_BPS, EPOCH_SECS);

   BOOST_REQUIRE_EQUAL( info.next_emission_est, expected_next );
   BOOST_REQUIRE( info.seconds_until_next > 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Integration
// ---------------------------------------------------------------------------

// payepoch's auth (require_auth(sysio.epoch)) is exercised implicitly by every
// other emissions test that drives advance_epoch_state -- if the auth check
// were absent, those tests would either spuriously pay or spuriously fail.
// A direct-push-as-non-epoch test would need sysio.system's payepoch helper
// in the test fixture's public surface; not worth the boilerplate.

// ---------------------------------------------------------------------------
// Performance-based producer pay
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( non_producing_active_excluded, sysio_emissions_tester ) try {
   // Producers with rank 1-21 but 0 eligible_rounds get no pay
   create_t5_holding_accounts();
   setup_producers(3);
   // Do NOT produce extra blocks — schedule hasn't activated, so producers have 0 eligible_rounds

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);
   asset bal_b_before = get_wire_balance("producerb"_n);
   asset bal_c_before = get_wire_balance("producerc"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Producers should receive nothing (0 eligible_rounds → excluded)
   BOOST_REQUIRE_EQUAL( get_wire_balance("producera"_n), bal_a_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance("producerb"_n), bal_b_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance("producerc"_n), bal_c_before );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( partial_uptime_proportional_pay, sysio_emissions_tester ) try {
   // Producers with eligible_rounds < expected_rounds get proportional share.
   // Override epoch_duration_sec so expected_rounds (=epoch_secs*2/252) is
   // well above the eligible_rounds the test produces (~2 from 2 cycles), so
   // the proportional path is exercised rather than the elig>=expected cap.
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(7200) );
   create_t5_holding_accounts();
   setup_producers(3);

   // Produce blocks so producers accumulate eligible_rounds
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2); // 2 cycles sufficient

   // Read eligible_rounds for producera before advance
   auto pa_info = get_producer_info("producera"_n);
   BOOST_REQUIRE( !pa_info.is_null() );
   uint16_t elig_a = pa_info["eligible_rounds"].as<uint16_t>();
   BOOST_REQUIRE( elig_a > 0 );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance("producera"_n).get_amount() - bal_a_before.get_amount();
   BOOST_REQUIRE( got_a > 0 );

   // Verify proportional: got_a < full_share (since elig < expected)
   auto log = get_epoch_log(1);
   int64_t compute = log["compute_amount"].as<int64_t>();
   int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   // Full share for one of 3 equal-weight producers
   int64_t full_share = producer_pool / 3;
   BOOST_REQUIRE( got_a < full_share );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( standby_paid_without_block_check, sysio_emissions_tester ) try {
   // Standby producers (rank 22-28) get full weight share without performance check
   create_t5_holding_accounts();

   // Set up 24 producers: 21 active + 3 standby (ranks 22-24)
   setup_producers(24);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // 1 cycle sufficient for eligible_rounds

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Standby producer (rank 22) is "producerw" (index 22)
   name standby_name = producer_name_at(21); // index 21 = 'v', rank 22
   asset standby_before = get_wire_balance(standby_name);

   // Verify the standby producer has rank 22
   auto standby_info = get_producer_info(standby_name);
   BOOST_REQUIRE( !standby_info.is_null() );
   uint32_t standby_rank = standby_info["rank"].as<uint32_t>();
   BOOST_REQUIRE( standby_rank >= T_STANDBY_START_RANK && standby_rank <= T_STANDBY_END_RANK );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Standby should receive payment even with 0 blocks produced
   int64_t standby_got = get_wire_balance(standby_name).get_amount() - standby_before.get_amount();
   BOOST_REQUIRE( standby_got > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( round_tracking_reset_after_epoch, sysio_emissions_tester ) try {
   // After advance, all round-tracking fields should be reset
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   // Verify fields are non-zero before advance
   auto pa_before = get_producer_info("producera"_n);
   BOOST_REQUIRE( pa_before["eligible_rounds"].as<uint16_t>() > 0 );
   BOOST_REQUIRE( pa_before["unpaid_blocks"].as<uint32_t>() > 0 );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // After advance, fields are reset; however, the block that commits the
   // advance transaction is itself produced by one of the test producers,
   // so onblock runs once after the reset and that producer's per-block tracking
   // (current_round_blocks + unpaid_blocks + last_block_num) gets re-bumped by 1.
   // eligible_rounds should still be 0 because a single block cannot satisfy
   // the per-round threshold.
   auto pa_after = get_producer_info("producera"_n);
   BOOST_REQUIRE_EQUAL( pa_after["eligible_rounds"].as<uint16_t>(), 0u );
   BOOST_REQUIRE( pa_after["current_round_blocks"].as<uint16_t>() <= 1 );
   BOOST_REQUIRE( pa_after["unpaid_blocks"].as<uint32_t>() <= 1 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( total_distributed_excludes_undistributed, sysio_emissions_tester ) try {
   // When some producers are excluded, total_distributed < emission
   create_t5_holding_accounts();
   setup_producers(3);
   // Producers have 0 eligible_rounds - all excluded - producer_pool undistributed.
   // Batch-op pool is also undistributed (no members in the rotation group).

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto state = get_t5_state();
   int64_t emission = state["last_epoch_emission"].as<int64_t>();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   // Both pools undistributed: total_distributed = emission minus (producer + batch)
   int64_t undist = compute_undistributed_if_no_operators(emission);
   BOOST_REQUIRE_EQUAL( distributed, emission - undist );
   BOOST_REQUIRE( distributed < emission );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( inprogress_round_finalized, sysio_emissions_tester ) try {
   // A producer with current_round_blocks >= 6 (but < 12) gets credit at epoch end
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();

   // Produce complete cycles so producers have some eligible_rounds
   produce_complete_cycles(3, 1);

   // Now produce blocks one-at-a-time until producera has a partial round with >= 6 blocks
   for (int i = 0; i < 200; ++i) {
      produce_blocks(1);
      auto info = get_producer_info("producera"_n);
      uint16_t cur = info["current_round_blocks"].as<uint16_t>();
      if (cur >= 6 && cur < 12) break;
   }

   // Check producera has in-progress round
   auto pa_info = get_producer_info("producera"_n);
   BOOST_REQUIRE( !pa_info.is_null() );
   uint16_t current_blocks = pa_info["current_round_blocks"].as<uint16_t>();
   uint16_t elig_before = pa_info["eligible_rounds"].as<uint16_t>();

   // producera should have in-progress blocks >= 6 and accumulated eligible rounds
   BOOST_REQUIRE( current_blocks >= 6 );
   BOOST_REQUIRE( current_blocks < 12 );
   BOOST_REQUIRE( elig_before >= 0 );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance("producera"_n).get_amount() - bal_a_before.get_amount();

   // payepoch finalizes in-progress round (>= 6 blocks) -> adds 1 to eligible_rounds
   // Pay should be based on (elig_before + 1) rounds > 0
   BOOST_REQUIRE( got_a > 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Mid-epoch schedule changes
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( producer_promoted_mid_epoch, sysio_emissions_tester ) try {
   // Producer starts as standby, gets promoted to active mid-epoch
   // Should receive proportional active pay based on eligible_rounds after promotion
   create_t5_holding_accounts();

   // Start with 22 producers: 21 active + 1 standby
   setup_producers(22);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // 1 cycle sufficient

   // Promote the standby (rank 22) to active by replacing producera in the schedule
   // New schedule: producers b..v + standby producer (index 21)
   std::vector<account_name> new_schedule;
   for (uint32_t i = 1; i <= 21; ++i) {
      new_schedule.push_back(producer_name_at(i));
   }
   BOOST_REQUIRE_EQUAL( success(), set_producer_schedule(new_schedule) );
   produce_blocks(1);

   // Produce more blocks with new schedule
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // The promoted producer (producerv, was standby) should now have active rank
   name promoted = producer_name_at(21); // "producerv"
   auto promoted_info = get_producer_info(promoted);
   BOOST_REQUIRE( !promoted_info.is_null() );
   uint32_t promoted_rank = promoted_info["rank"].as<uint32_t>();
   BOOST_REQUIRE( promoted_rank >= 1 && promoted_rank <= T_ACTIVE_PRODUCER_COUNT );

   asset promoted_before = get_wire_balance(promoted);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t promoted_got = get_wire_balance(promoted).get_amount() - promoted_before.get_amount();
   // Should get proportional active pay (they produced blocks after promotion)
   BOOST_REQUIRE( promoted_got > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( producer_demoted_mid_epoch, sysio_emissions_tester ) try {
   // Producer starts as active, accumulates eligible_rounds, then gets demoted to standby
   // At epoch end, treated as standby → full standby weight (no performance check)
   create_t5_holding_accounts();

   // Start with 22 producers: 21 active + 1 standby
   setup_producers(22);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // producera accumulates eligible_rounds

   // Demote producera: new schedule replaces producera with the standby
   std::vector<account_name> new_schedule;
   for (uint32_t i = 1; i <= 21; ++i) {
      new_schedule.push_back(producer_name_at(i));
   }
   BOOST_REQUIRE_EQUAL( success(), set_producer_schedule(new_schedule) );
   produce_blocks(1);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // producera should now be demoted (rank 22+)
   auto pa_info = get_producer_info("producera"_n);
   BOOST_REQUIRE( !pa_info.is_null() );
   uint32_t pa_rank = pa_info["rank"].as<uint32_t>();
   BOOST_REQUIRE( pa_rank >= T_STANDBY_START_RANK );

   asset demoted_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   if (pa_rank <= T_STANDBY_END_RANK) {
      // Treated as standby → gets standby weight share (no performance check)
      int64_t demoted_got = get_wire_balance("producera"_n).get_amount() - demoted_before.get_amount();
      BOOST_REQUIRE( demoted_got > 0 );
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( producer_replaced_mid_epoch, sysio_emissions_tester ) try {
   // One active producer replaced by another. Old goes to standby, new gets active.
   // Both receive partial payment. Total distributed < full emission.
   create_t5_holding_accounts();

   // Start with 22 producers
   setup_producers(22);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // 1 cycle sufficient

   // Replace producera with the standby producer (index 21)
   std::vector<account_name> new_schedule;
   for (uint32_t i = 1; i <= 21; ++i) {
      new_schedule.push_back(producer_name_at(i));
   }
   BOOST_REQUIRE_EQUAL( success(), set_producer_schedule(new_schedule) );
   produce_blocks(1);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset old_before = get_wire_balance("producera"_n);
   name new_producer = producer_name_at(21);
   asset new_before = get_wire_balance(new_producer);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();

   // Verify: old producer (now standby) gets standby pay if in range
   auto pa_info = get_producer_info("producera"_n);
   uint32_t pa_rank = pa_info["rank"].as<uint32_t>();
   if (pa_rank <= T_STANDBY_END_RANK) {
      int64_t old_got = get_wire_balance("producera"_n).get_amount() - old_before.get_amount();
      BOOST_REQUIRE( old_got > 0 );
   }

   // Verify: new active producer gets proportional active pay
   int64_t new_got = get_wire_balance(new_producer).get_amount() - new_before.get_amount();
   BOOST_REQUIRE( new_got > 0 );

   // Total distributed should be less than full emission (both had partial epochs)
   auto state = get_t5_state();
   BOOST_REQUIRE( state["total_distributed"].as<int64_t>() < emission );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: timing & epoch boundaries
// ---------------------------------------------------------------------------

// `delayed_epoch_processes_only_once` removed: tested the cranker-era
// idempotency guard ("emissions already caught up to sysio.epoch"). Under the
// gate-based model, every successful advance pays exactly once -- there is no
// idempotency to test. Wall-clock gating in advance() (next_epoch_start) is
// covered by other tests that drive multiple advances within one trx flow.

BOOST_FIXTURE_TEST_CASE( multi_epoch_cumulative_accounting, sysio_emissions_tester ) try {
   // Run 3 epochs and verify total_distributed equals sum of per-epoch effective distributions
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   int64_t cumulative = 0;

   // Epoch 1 (sysio.epoch bootstrapped to index 1 in fixture).
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   auto log1 = get_epoch_log(1);
   int64_t e1 = log1["total_emission"].as<int64_t>();
   int64_t undist1 = compute_undistributed_if_no_operators(e1);
   cumulative += (e1 - undist1);

   auto state1 = get_t5_state();
   BOOST_REQUIRE_EQUAL( state1["epoch_count"].as<uint64_t>(), 1u );
   BOOST_REQUIRE_EQUAL( state1["total_distributed"].as<int64_t>(), cumulative );

   // Advance sysio.epoch to index 2 and process.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   auto log2 = get_epoch_log(2);
   int64_t e2 = log2["total_emission"].as<int64_t>();
   int64_t undist2 = compute_undistributed_if_no_operators(e2);
   cumulative += (e2 - undist2);

   auto state2 = get_t5_state();
   BOOST_REQUIRE_EQUAL( state2["epoch_count"].as<uint64_t>(), 2u );
   BOOST_REQUIRE_EQUAL( state2["total_distributed"].as<int64_t>(), cumulative );

   // Verify decay applied
   BOOST_REQUIRE( e2 < e1 );

   // Advance sysio.epoch to index 3 and process.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   auto log3 = get_epoch_log(3);
   int64_t e3 = log3["total_emission"].as<int64_t>();
   int64_t undist3 = compute_undistributed_if_no_operators(e3);
   cumulative += (e3 - undist3);

   auto state3 = get_t5_state();
   BOOST_REQUIRE_EQUAL( state3["epoch_count"].as<uint64_t>(), 3u );
   BOOST_REQUIRE_EQUAL( state3["total_distributed"].as<int64_t>(), cumulative );

   // Verify monotonic decay
   BOOST_REQUIRE( e3 < e2 );
   BOOST_REQUIRE( e2 < e1 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: category split arithmetic
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( category_splits_sum_to_emission, sysio_emissions_tester ) try {
   // Verify compute + capital + capex + governance == total_emission exactly
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission    = log["total_emission"].as<int64_t>();
   int64_t compute     = log["compute_amount"].as<int64_t>();
   int64_t capital     = log["capital_amount"].as<int64_t>();
   int64_t capex       = log["capex_amount"].as<int64_t>();
   int64_t governance  = log["governance_amount"].as<int64_t>();

   // Exact sum: governance absorbs remainder, so this must hold exactly
   BOOST_REQUIRE_EQUAL( compute + capital + capex + governance, emission );

   // Compute sub-split: producer_pool + batch_pool == compute_amount
   int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   int64_t batch_pool    = compute - producer_pool;
   BOOST_REQUIRE_EQUAL( producer_pool + batch_pool, compute );
   BOOST_REQUIRE( producer_pool > 0 );
   BOOST_REQUIRE( batch_pool > 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: epoch log field verification
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( epoch_log_records_all_fields, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   BOOST_REQUIRE( !log.is_null() );

   // sysio_epoch_index aligns with sysio.epoch's current_epoch_index at write time.
   // epoch_count is sysio.system's internal invocation counter; after the first
   // advance call both are 1.
   BOOST_REQUIRE_EQUAL( log["sysio_epoch_index"].as<uint32_t>(), 1u );
   BOOST_REQUIRE_EQUAL( log["epoch_count"].as<uint64_t>(), 1u );

   // timestamp is set (non-zero)
   BOOST_REQUIRE( log["timestamp"].as<time_point_sec>().sec_since_epoch() > 0 );

   // total_emission matches expected first epoch
   BOOST_REQUIRE_EQUAL( log["total_emission"].as<int64_t>(),
      test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, 60) );

   // All category amounts are positive
   BOOST_REQUIRE( log["compute_amount"].as<int64_t>() > 0 );
   BOOST_REQUIRE( log["capital_amount"].as<int64_t>() > 0 );
   BOOST_REQUIRE( log["capex_amount"].as<int64_t>() > 0 );
   BOOST_REQUIRE( log["governance_amount"].as<int64_t>() > 0 );

   // Category amounts match expected BPS splits
   int64_t emission = log["total_emission"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( log["compute_amount"].as<int64_t>(), test_split_bps(emission, COMPUTE_BPS) );
   BOOST_REQUIRE_EQUAL( log["capital_amount"].as<int64_t>(), test_split_bps(emission, CAPITAL_BPS) );
   BOOST_REQUIRE_EQUAL( log["capex_amount"].as<int64_t>(), test_split_bps(emission, CAPEX_BPS) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: producer pay edge cases
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( all_actives_excluded_standbys_still_paid, sysio_emissions_tester ) try {
   // When all 21 active producers have 0 eligible_rounds, only standbys receive payment.
   create_t5_holding_accounts();

   // Set up 24 producers: 21 active + 3 standby
   // Do NOT wait for schedule or produce blocks — actives have 0 eligible_rounds
   setup_producers(24);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Active producer should have 0 eligible_rounds
   name active = producer_name_at(0);
   name standby = producer_name_at(21);

   asset active_before  = get_wire_balance(active);
   asset standby_before = get_wire_balance(standby);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Active should get nothing (0 eligible_rounds)
   BOOST_REQUIRE_EQUAL( get_wire_balance(active), active_before );

   // Standby should get paid (no block production check for standbys)
   auto standby_info = get_producer_info(standby);
   uint32_t standby_rank = standby_info["rank"].as<uint32_t>();
   if (standby_rank >= T_STANDBY_START_RANK && standby_rank <= T_STANDBY_END_RANK) {
      int64_t standby_got = get_wire_balance(standby).get_amount() - standby_before.get_amount();
      BOOST_REQUIRE( standby_got > 0 );
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( single_active_producer_full_active_share, sysio_emissions_tester ) try {
   // A single active producer who produces blocks should get the entire active-weight share
   create_t5_holding_accounts();
   setup_producers(1);
   wait_for_producer_schedule();
   produce_complete_cycles(1, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got = get_wire_balance("producera"_n).get_amount() - bal_before.get_amount();
   BOOST_REQUIRE( got > 0 );

   // With only 1 active (weight=15, total_weight=15), full_share = producer_pool
   // Payment is proportional: pool * min(elig, expected) / expected
   auto log = get_epoch_log(1);
   int64_t compute = log["compute_amount"].as<int64_t>();
   int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);

   // They must get something > 0 and <= producer_pool
   BOOST_REQUIRE( got <= producer_pool );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( standby_weight_decreases_by_rank, sysio_emissions_tester ) try {
   // Rank 22 should receive more than rank 23, which should receive more than rank 24, etc.
   // Weight formula: w = 29 - rank (22→7, 23→6, 24→5)
   create_t5_holding_accounts();

   // Set up 25 producers: 21 active + 4 standby (ranks 22-25)
   setup_producers(25);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // active producers produce blocks

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   name sb1 = producer_name_at(21); // rank 22, weight 7
   name sb2 = producer_name_at(22); // rank 23, weight 6
   name sb3 = producer_name_at(23); // rank 24, weight 5

   asset sb1_before = get_wire_balance(sb1);
   asset sb2_before = get_wire_balance(sb2);
   asset sb3_before = get_wire_balance(sb3);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got1 = get_wire_balance(sb1).get_amount() - sb1_before.get_amount();
   int64_t got2 = get_wire_balance(sb2).get_amount() - sb2_before.get_amount();
   int64_t got3 = get_wire_balance(sb3).get_amount() - sb3_before.get_amount();

   // Higher rank (lower number) should get more: got1 > got2 > got3
   BOOST_REQUIRE( got1 > got2 );
   BOOST_REQUIRE( got2 > got3 );
   BOOST_REQUIRE( got3 > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( inprogress_round_below_threshold_no_credit, sysio_emissions_tester ) try {
   // A producer with current_round_blocks < 6 should NOT get credit from in-progress finalization.
   // If that means 0 total eligible_rounds, they get excluded from payment.
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();

   // Produce blocks one-at-a-time until producera has < 6 current_round_blocks
   // We need: eligible_rounds == 0 AND 0 < current_round_blocks < 6
   // Strategy: produce less than one full cycle so producera doesn't complete a round
   for (int i = 0; i < 5; ++i) {
      produce_blocks(1);
   }

   auto pa_info = get_producer_info("producera"_n);
   if (pa_info.is_null()) {
      // producera hasn't produced yet, eligible_rounds=0
   } else {
      uint16_t current_blocks = pa_info["current_round_blocks"].as<uint16_t>();
      uint16_t elig_rounds    = pa_info["eligible_rounds"].as<uint16_t>();

      // If producera has produced, it should have < 6 blocks in current round and 0 eligible
      if (current_blocks > 0 && current_blocks < 6 && elig_rounds == 0) {
         const uint32_t start = head_secs() - ONE_EPOCH - 1;
         BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

         asset bal_before = get_wire_balance("producera"_n);
         BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

         // Finalization should NOT credit this round (< 6 blocks)
         // So eligible_rounds stays 0 → excluded from payment
         BOOST_REQUIRE_EQUAL( get_wire_balance("producera"_n), bal_before );
      }
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( active_capped_at_expected_rounds, sysio_emissions_tester ) try {
   // expected_rounds = (epoch_duration_sec * 2) / TOTAL_BLOCKS_PER_ROUND.
   // Use 7200s so expected_rounds = 57, well above the elig_rounds the test
   // produces (~2 from 2 cycles). Pay then = elig/expected * full_share which
   // is strictly less than full_share -- exercising the proportional path.
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(7200) );
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2); // some eligible_rounds

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // All 3 producers have same eligible_rounds and same weight
   asset bal_a_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance("producera"_n).get_amount() - bal_a_before.get_amount();

   auto log = get_epoch_log(1);
   int64_t compute = log["compute_amount"].as<int64_t>();
   int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);

   // Each has weight 15 out of total 45, so full_share = pool * 15 / 45 = pool / 3
   int64_t full_share = static_cast<int64_t>(
      static_cast<__int128>(producer_pool) * 15 / 45);

   // Payment is min(elig_rounds, expected_rounds) / expected_rounds * full_share
   // Since elig_rounds << expected_rounds (2 cycles vs 685), pay < full_share
   BOOST_REQUIRE( got_a > 0 );
   BOOST_REQUIRE( got_a < full_share );

} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: sysio balance accounting
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( sysio_balance_decreases_by_distributed_amount, sysio_emissions_tester ) try {
   // The sysio account should lose exactly (emission - undistributed_producer) in WIRE balance
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset sysio_before = get_wire_balance(config::system_account_name);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   asset sysio_after = get_wire_balance(config::system_account_name);

   auto state = get_t5_state();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   // sysio loses exactly total_distributed
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   BOOST_REQUIRE_EQUAL( sysio_decrease, distributed );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( sysio_balance_with_producers, sysio_emissions_tester ) try {
   // When producers ARE paid, sysio loses emission minus only the rounding dust
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset sysio_before = get_wire_balance(config::system_account_name);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   asset sysio_after = get_wire_balance(config::system_account_name);

   auto state = get_t5_state();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   BOOST_REQUIRE_EQUAL( sysio_decrease, distributed );

   // With producers paid, distributed must exceed the "no operators" baseline
   // (producer_pool + batch_pool both undistributed), meaning at least some
   // producer pool was consumed by the eligible producers.
   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t baseline_no_ops = emission - compute_undistributed_if_no_operators(emission);
   BOOST_REQUIRE( distributed > baseline_no_ops );
   BOOST_REQUIRE( distributed <= emission );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: viewepoch edge cases
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( viewepoch_before_first_epoch, sysio_emissions_tester ) try {
   // viewepoch should work before any epoch is processed
   create_t5_holding_accounts();
   const uint32_t start = head_secs();
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   auto info = viewepoch();
   BOOST_REQUIRE_EQUAL( info.epoch_count, 0u );
   BOOST_REQUIRE_EQUAL( info.total_distributed, 0 );
   BOOST_REQUIRE_EQUAL( info.treasury_remaining, T5_DISTRIBUTABLE - T5_FLOOR );
   BOOST_REQUIRE_EQUAL( info.next_emission_est,
      test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, 60) );
   BOOST_REQUIRE( info.seconds_until_next > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewepoch_after_multiple_epochs, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Epoch 1 (sysio.epoch already at index 1 from fixture's bootstrap_epoch).
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Advance sysio.epoch to index 2 across the wall-clock boundary.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto info = viewepoch();
   BOOST_REQUIRE_EQUAL( info.epoch_count, 2u );
   BOOST_REQUIRE( info.total_distributed > 0 );
   BOOST_REQUIRE_EQUAL( info.treasury_remaining, T5_DISTRIBUTABLE - T5_FLOOR - info.total_distributed );

   // next_emission_est should reflect decay from epoch 2's emission
   const int64_t expected_next =
      test_apply_decay(info.last_epoch_emission, TARGET_ANNUAL_DECAY_BPS, 60);
   BOOST_REQUIRE_EQUAL( info.next_emission_est, expected_next );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: rank boundaries
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( rank_29_and_above_get_nothing, sysio_emissions_tester ) try {
   // Producers with rank > STANDBY_END_RANK (28) should receive nothing
   create_t5_holding_accounts();

   // Set up 30 producers: 21 active + 7 standby (22-28) + 2 beyond (29-30)
   setup_producers(30);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Producers at index 28 and 29 should be rank 29 and 30
   name beyond1 = producer_name_at(28);
   name beyond2 = producer_name_at(29);

   asset beyond1_before = get_wire_balance(beyond1);
   asset beyond2_before = get_wire_balance(beyond2);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto b1_info = get_producer_info(beyond1);
   auto b2_info = get_producer_info(beyond2);

   // Only check if their rank is actually > 28
   if (!b1_info.is_null() && b1_info["rank"].as<uint32_t>() > T_STANDBY_END_RANK) {
      BOOST_REQUIRE_EQUAL( get_wire_balance(beyond1), beyond1_before );
   }
   if (!b2_info.is_null() && b2_info["rank"].as<uint32_t>() > T_STANDBY_END_RANK) {
      BOOST_REQUIRE_EQUAL( get_wire_balance(beyond2), beyond2_before );
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( rank_28_standby_gets_minimum_weight, sysio_emissions_tester ) try {
   // Rank 28 = weight 1 (29 - 28). Should be the smallest standby payment.
   create_t5_holding_accounts();

   // Set up 28 producers: 21 active + 7 standby (ranks 22-28)
   setup_producers(28);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   name last_standby = producer_name_at(27); // index 27 = rank 28
   asset last_before = get_wire_balance(last_standby);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto info = get_producer_info(last_standby);
   if (!info.is_null() && info["rank"].as<uint32_t>() == T_STANDBY_END_RANK) {
      int64_t got = get_wire_balance(last_standby).get_amount() - last_before.get_amount();
      BOOST_REQUIRE( got > 0 ); // weight = 1, should still get paid
   }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: inactive producer handling
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( inactive_producer_excluded_from_distribution, sysio_emissions_tester ) try {
   // A producer marked is_active=false should be skipped even if they have valid rank
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   // Deactivate producera by unregistering
   base_tester::push_action(
      config::system_account_name,
      "unregprod"_n,
      vector<permission_level>{{"producera"_n, "active"_n}},
      mvo()("producer", "producera"_n)
   );
   produce_blocks(1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Inactive producer should receive nothing
   auto pa_info = get_producer_info("producera"_n);
   if (!pa_info.is_null() && !pa_info["is_active"].as<bool>()) {
      BOOST_REQUIRE_EQUAL( get_wire_balance("producera"_n), bal_a_before );
   }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: round tracking correctness
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( eligible_rounds_increment_per_complete_cycle, sysio_emissions_tester ) try {
   // Verify that eligible_rounds increments correctly as complete rounds are produced
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();

   // Produce 1 complete cycle: each of 3 producers does 12 blocks = 1 eligible round each
   produce_complete_cycles(3, 1);

   auto pa_info = get_producer_info("producera"_n);
   BOOST_REQUIRE( !pa_info.is_null() );
   uint16_t elig_1 = pa_info["eligible_rounds"].as<uint16_t>();
   BOOST_REQUIRE( elig_1 >= 1 );

   // Produce another cycle
   produce_complete_cycles(3, 1);

   auto pa_info2 = get_producer_info("producera"_n);
   uint16_t elig_2 = pa_info2["eligible_rounds"].as<uint16_t>();
   BOOST_REQUIRE( elig_2 > elig_1 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( unpaid_blocks_track_actual_production, sysio_emissions_tester ) try {
   // Verify unpaid_blocks counts actual blocks produced
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();

   // Produce 2 complete cycles: 3 producers × 12 blocks × 2 = 72 blocks total
   // Each producer should have ~24 unpaid_blocks
   produce_complete_cycles(3, 2);

   auto pa_info = get_producer_info("producera"_n);
   BOOST_REQUIRE( !pa_info.is_null() );
   uint32_t unpaid = pa_info["unpaid_blocks"].as<uint32_t>();
   BOOST_REQUIRE( unpaid >= 20 ); // at least ~24 blocks produced (allowing for schedule transition)
   BOOST_REQUIRE( unpaid <= 30 ); // bounded above
} FC_LOG_AND_RETHROW()

// =============================================================================
// OPP integration tests
// -----------------------------------------------------------------------------
// These tests exercise emissions's cross-contract reads against the real
// sysio.opreg and sysio.epoch contracts deployed by the fixture, covering:
//
//   - opreg status filter (slashed / terminated producers excluded from pay)
//   - treasury balance floor (gate blocks advance when sysio has insufficient WIRE)
//   - sysio.roa::regnodeowner inline-addnodeowner guard when emitcfg is unset
// =============================================================================

BOOST_FIXTURE_TEST_CASE( opreg_slashed_producer_excluded_from_pay, sysio_emissions_tester ) try {
   // A producer whose opreg status flips from ACTIVE to SLASHED must stop
   // receiving emission pay; their share stays in the treasury rather than
   // being redistributed to surviving producers.
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   // Slash producerb. Emissions should skip it but keep paying the other two.
   BOOST_REQUIRE_EQUAL( success(), slash_operator("producerb"_n) );
   produce_blocks(1);

   auto op_row = get_opreg_operator("producerb"_n);
   BOOST_REQUIRE( !op_row.is_null() );
   // Proto enum fields come through the abi_serializer as their string name.
   BOOST_REQUIRE_EQUAL( op_row["status"].as_string(), "OPERATOR_STATUS_SLASHED" );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);
   asset bal_b_before = get_wire_balance("producerb"_n);
   asset bal_c_before = get_wire_balance("producerc"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance("producera"_n).get_amount() - bal_a_before.get_amount();
   int64_t got_b = get_wire_balance("producerb"_n).get_amount() - bal_b_before.get_amount();
   int64_t got_c = get_wire_balance("producerc"_n).get_amount() - bal_c_before.get_amount();

   BOOST_REQUIRE_EQUAL( got_b, 0 );
   BOOST_REQUIRE( got_a > 0 );
   BOOST_REQUIRE( got_c > 0 );
   // producera / producerc keep their original 1/3 share (weighted by rank * eligible_rounds);
   // producerb's share does not flow to them.
   BOOST_REQUIRE_EQUAL( got_a, got_c );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( opreg_unregistered_producer_excluded_from_pay, sysio_emissions_tester ) try {
   // A producer that is registered in sysio.system but not in sysio.opreg at
   // all must also be filtered -- they're treated as "status unknown" by the
   // opreg filter.
   create_t5_holding_accounts();
   // setup_producers with register_opreg=false: producers exist on sysio.system
   // but have no opreg registration.
   setup_producers(3, /*register_opreg=*/false);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset sysio_before = get_wire_balance(config::system_account_name);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   asset sysio_after = get_wire_balance(config::system_account_name);

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();

   // With all producers unregistered in opreg, the producer_pool is fully
   // undistributed (same baseline as "no producers at all"). Batch pool also
   // stays in treasury.
   int64_t expected_baseline = emission - compute_undistributed_if_no_operators(emission);
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   BOOST_REQUIRE_EQUAL( sysio_decrease, expected_baseline );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( advance_gate_blocks_on_insufficient_treasury_balance, sysio_emissions_tester ) try {
   // The fixture funds sysio with 1_000_000_000 WIRE which easily covers the
   // default emission schedule. If sysio's balance is drained below the next
   // epoch's emission, sysio.epoch's readiness gate refuses to advance: the
   // epoch index stays at 0, a blocklog row is written, and an EmissionsBlocked
   // attestation is queued per outpost.
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Drain sysio's WIRE balance into a dummy account. Transfer amount chosen
   // so sysio keeps a small balance (much less than epoch_initial_emission).
   create_user_accounts({ "drainacct"_n });
   asset drain_amount = asset::from_string("999999999.000000000 WIRE");
   base_tester::push_action(
      TOKEN, "transfer"_n,
      vector<permission_level>{{ config::system_account_name, "active"_n }},
      mvo()("from", config::system_account_name)
           ("to", "drainacct"_n)
           ("quantity", drain_amount)
           ("memo", "drain for balance-floor test")
   );

   // advance must succeed (no throw -- gate emits error attestation cleanly).
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   // Epoch index unchanged (still 0). If epochstate row hasn't been written,
   // that also means current_epoch_index is effectively 0.
   auto est = get_epoch_state_row();
   if (!est.is_null()) {
      BOOST_REQUIRE_EQUAL( est["current_epoch_index"].as<uint32_t>(), 0u );
   }

   // t5state.last_epoch_index unchanged (no payepoch ran).
   BOOST_REQUIRE_EQUAL( get_t5_state()["last_epoch_index"].as<uint32_t>(), 0u );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( roa_forcereg_inlines_addnodeowner_happy_path, sysio_emissions_tester ) try {
   // Happy path: with emitcfg present (fixture default), sysio.roa::forcereg
   // fires an inline sysio.system::addnodeowner which writes a nodedist row.
   //
   // The complementary "emitcfg absent -> skip" path is exercised by the
   // Python TestHarness bootstrap in Cluster.py when loadSystemContract=False
   // (not covered by this Boost suite; see sysio.roa_tests.cpp notes). The
   // guard itself (`if (emitcfg.exists())` in sysio.roa::regnodeowner) is
   // load-bearing for bootstrap flows without sysio.system deployed.
   create_user_accounts({ "forceregt1"_n });

   auto trace = forcereg_trace( ROA, "forceregt1"_n, 1 );
   BOOST_REQUIRE( trace );
   BOOST_REQUIRE( !trace->except );

   // Walk the action traces looking for the inline sysio::addnodeowner notice.
   bool saw_addnodeowner = false;
   for (const auto& at : trace->action_traces) {
      if (at.receiver == config::system_account_name && at.act.name == "addnodeowner"_n) {
         saw_addnodeowner = true;
         break;
      }
   }
   BOOST_REQUIRE( saw_addnodeowner );

   // nodedist row created on sysio.system
   auto row = get_nodedist_row("forceregt1"_n);
   BOOST_REQUIRE( !row.is_null() );
   BOOST_REQUIRE_EQUAL( row["account_name"].as<name>(), "forceregt1"_n );
   BOOST_REQUIRE_EQUAL( row["total_allocation"].as<asset>(), T1_ALLOCATION );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// epochlog retention
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_zero_retention, sysio_emissions_tester ) try {
   // epoch_log_retention_count = 0 would never prune; reject so the audit log
   // can never grow unbounded by misconfig.
   auto cfg = mvo()
      ("t1_allocation", T1_ALLOCATION.get_amount())
      ("t2_allocation", T2_ALLOCATION.get_amount())
      ("t3_allocation", T3_ALLOCATION.get_amount())
      ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable", MIN_CLAIMABLE_AMOUNT)
      ("t5_distributable", T5_DISTRIBUTABLE) ("t5_floor", int64_t(125000000000000000LL))
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", ANNUAL_INITIAL_EMISSION)
      ("annual_max_emission", ANNUAL_MAX_EMISSION) ("annual_min_emission", ANNUAL_MIN_EMISSION)
      ("compute_bps", COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)
      ("epoch_log_retention_count", uint32_t(0))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "epoch_log_retention_count must be positive" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( epochlog_prunes_past_retention_cap, sysio_emissions_tester ) try {
   // Set retention cap to 3, advance through 5 epochs, verify only the last
   // 3 epoch_log rows survive.
   create_t5_holding_accounts();

   auto cfg = mvo()
      ("t1_allocation", T1_ALLOCATION.get_amount())
      ("t2_allocation", T2_ALLOCATION.get_amount())
      ("t3_allocation", T3_ALLOCATION.get_amount())
      ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable", MIN_CLAIMABLE_AMOUNT)
      ("t5_distributable", T5_DISTRIBUTABLE) ("t5_floor", int64_t(125000000000000000LL))
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", ANNUAL_INITIAL_EMISSION)
      ("annual_max_emission", ANNUAL_MAX_EMISSION) ("annual_min_emission", ANNUAL_MIN_EMISSION)
      ("compute_bps", COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)
      ("epoch_log_retention_count", uint32_t(3))("pay_cadence_epochs", uint16_t(1));
   BOOST_REQUIRE_EQUAL( success(), setemitcfg(config::system_account_name, cfg) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Epoch 1: genesis advance fires immediately (next_epoch_start = 0).
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   // Epochs 2-5: cross the wall-clock boundary each time.
   for (int i = 0; i < 4; ++i) {
      produce_blocks(130);
      BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   }

   // Cap is 3; only epochs 3, 4, 5 should remain. Epochs 1 and 2 are pruned.
   BOOST_REQUIRE( get_epoch_log(1).is_null() );
   BOOST_REQUIRE( get_epoch_log(2).is_null() );
   BOOST_REQUIRE( !get_epoch_log(3).is_null() );
   BOOST_REQUIRE( !get_epoch_log(4).is_null() );
   BOOST_REQUIRE( !get_epoch_log(5).is_null() );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// pay_cadence_epochs > 1 (period-based pay)
// ---------------------------------------------------------------------------
//
// The cases below exercise the cadence-aware path: accrueepoch fires every
// epoch, payepoch fires only on the period boundary (target_epoch >=
// period_start_epoch + cadence - 1). Per-epoch state (last_epoch_emission,
// last_epoch_index) is owned by accrueepoch; the period accumulator
// (pending_emission_amount, batch_group_epochs, period_start_epoch) is
// drained by payepoch.

BOOST_FIXTURE_TEST_CASE( pay_cadence_2_pays_every_other_epoch, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(2) ) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // First advance: target_epoch=1. With period_start_epoch=0 and cadence=2
   // the pay condition (target >= 0 + 2 - 1) is target >= 1, so this IS a
   // pay-epoch. But pending_emission_amount starts at 0, so the period
   // emission equals one per-epoch share -- effectively the same payout the
   // cadence=1 case would produce on epoch 1, modulo the period_start_epoch
   // moving to 2 instead of 2 (no difference for genesis).
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );
      BOOST_REQUIRE_EQUAL( state["pending_emission_amount"].as<int64_t>(), 0 );
      BOOST_REQUIRE_EQUAL( state["period_start_epoch"].as<uint32_t>(), 2u );
   }

   // Second advance: target_epoch=2, period_start=2. Condition (target >= 2 +
   // 2 - 1 = 3) is FALSE, so this is a NON-pay epoch. accrueepoch fires
   // alone; epoch_count stays at 1, pending grows by one per-epoch share.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );  // payepoch did NOT fire
      BOOST_REQUIRE( state["pending_emission_amount"].as<int64_t>() > 0 );  // accumulating
      BOOST_REQUIRE_EQUAL( state["last_epoch_index"].as<uint32_t>(), 2u );  // accrueepoch advanced index
   }

   // Third advance: target_epoch=3, period_start=2. Condition (target >= 3)
   // is TRUE; pay-epoch fires. pending drains, epoch_count==2,
   // period_start_epoch advances to 4 for the next period.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 2u );  // payepoch fired
      BOOST_REQUIRE_EQUAL( state["pending_emission_amount"].as<int64_t>(), 0 );  // drained
      BOOST_REQUIRE_EQUAL( state["period_start_epoch"].as<uint32_t>(), 4u );  // next period anchor
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( pay_cadence_pending_accumulates_then_drains, sysio_emissions_tester ) try {
   // Cadence=3 under period_start_epoch=0: pay-epoch condition is target >= 2,
   // so target=1 (genesis) is NON-pay, target=2 is pay, target=3..4 non-pay,
   // target=5 pay, etc. (First period covers two real epochs because epoch 0
   // is genesis -- documented quirk of the period_start_epoch=0 default.)
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(3) ) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // First advance is target=1 -- non-pay under cadence=3. accrueepoch fires
   // alone; pending grows from 0 by one per-epoch share.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   const int64_t pending_after_genesis  = get_t5_state()["pending_emission_amount"].as<int64_t>();
   const int64_t per_epoch_after_genesis = get_t5_state()["last_epoch_emission"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( get_t5_state()["epoch_count"].as<uint64_t>(), 0u );  // payepoch has not fired yet
   BOOST_REQUIRE( pending_after_genesis > 0 );
   BOOST_REQUIRE_EQUAL( pending_after_genesis, per_epoch_after_genesis );

   // Second advance: target=2, pay-epoch fires. pending drains, epoch_count
   // becomes 1, period_start_epoch advances to 3 for the next period.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );
      BOOST_REQUIRE_EQUAL( state["pending_emission_amount"].as<int64_t>(), 0 );
      BOOST_REQUIRE_EQUAL( state["period_start_epoch"].as<uint32_t>(), 3u );
   }

   // Third + fourth advances: target=3, target=4. Both non-pay (3 < 5 and
   // 4 < 5 under the new period_start=3, cadence=3). pending grows by
   // per-epoch share each advance.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );  // target=3 non-pay
   const int64_t pending_after_one = get_t5_state()["pending_emission_amount"].as<int64_t>();
   BOOST_REQUIRE( pending_after_one > 0 );

   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );  // target=4 non-pay
   const int64_t pending_after_two = get_t5_state()["pending_emission_amount"].as<int64_t>();
   BOOST_REQUIRE( pending_after_two > pending_after_one );

   // Fifth advance: target=5, pay-epoch (5 >= 3 + 3 - 1 = 5). pending drains,
   // epoch_count==2, period_start_epoch advances to 6.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 2u );
      BOOST_REQUIRE_EQUAL( state["pending_emission_amount"].as<int64_t>(), 0 );
      BOOST_REQUIRE_EQUAL( state["period_start_epoch"].as<uint32_t>(), 6u );
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( pay_cadence_epochlog_only_on_pay_epoch, sysio_emissions_tester ) try {
   // Audit log (`epochlog` table) gains one row per payepoch invocation; non-
   // pay epochs add nothing. With cadence=2, advancing through epochs 1..4
   // produces exactly two epochlog rows (epochs 1 and 3 -- the genesis pay-
   // epoch under cadence=2 and the next period boundary).
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(2) ) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );  // target=1 pay (genesis)
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );  // target=2 non-pay
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );  // target=3 pay

   BOOST_REQUIRE( !get_epoch_log(1).is_null() );  // pay-epoch -> row written
   BOOST_REQUIRE(  get_epoch_log(2).is_null() );  // non-pay   -> no row
   BOOST_REQUIRE( !get_epoch_log(3).is_null() );  // pay-epoch -> row written
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( pay_cadence_treasury_exhausted_gates_non_pay_epoch, sysio_emissions_tester ) try {
   // TREASURY_EXHAUSTED gates EVERY epoch (pay or non-pay), not just pay-epochs.
   // Otherwise the gate would silently advance non-pay epochs into a depleted
   // treasury and only block at the period boundary. Verifies that with
   // pay_cadence_epochs > 1 and a treasury at floor, the FIRST non-pay
   // advance attempt blocks with TREASURY_EXHAUSTED and state does not
   // advance.
   create_t5_holding_accounts();

   auto cfg = mvo()
      ("t1_allocation", T1_ALLOCATION.get_amount())
      ("t2_allocation", T2_ALLOCATION.get_amount())
      ("t3_allocation", T3_ALLOCATION.get_amount())
      ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable", MIN_CLAIMABLE_AMOUNT)
      ("t5_distributable", int64_t(125000000000000000LL))
      ("t5_floor",         int64_t(125000000000000000LL))   // distributable == floor -> remaining=0
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", int64_t(0))               // forces per-epoch emission == 0
      ("annual_max_emission", ANNUAL_MAX_EMISSION) ("annual_min_emission", int64_t(0))
      ("compute_bps", COMPUTE_BPS) ("capital_bps", CAPITAL_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)
      ("epoch_log_retention_count", uint32_t(8640))
      ("pay_cadence_epochs", uint16_t(3));                  // non-pay epochs in the period
   BOOST_REQUIRE_EQUAL( success(), setemitcfg(config::system_account_name, cfg) );
   BOOST_REQUIRE_EQUAL( success(), initt5(config::system_account_name, tpsec(head_secs() - ONE_EPOCH - 1)) );

   // Genesis advance (target=1) is a non-pay epoch under cadence=3 but the
   // gate must still block it because per-epoch emission is zero.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto bl = get_blocklog_row(1u);
   BOOST_REQUIRE( !bl.is_null() );
   BOOST_REQUIRE_EQUAL( bl["reason"].as_string(), "EMISSIONS_BLOCK_REASON_TREASURY_EXHAUSTED" );

   // State must not have advanced: no accrueepoch fired, last_epoch_index
   // stays at 0 (the initt5 value), pending stays at 0.
   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["last_epoch_index"].as<uint32_t>(), 0u );
   BOOST_REQUIRE_EQUAL( state["pending_emission_amount"].as<int64_t>(), 0 );
   BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 0u );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( pay_cadence_change_via_setemitcfg_takes_effect, sysio_emissions_tester ) try {
   // setemitcfg can change pay_cadence_epochs at any time; the new value
   // takes effect on the next advance. Verifies that lowering cadence
   // mid-period turns a previously-non-pay epoch into a pay-epoch.
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(3) ) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Advance 1 under cadence=3: target=1 is non-pay (1 < 0 + 3 - 1 = 2).
   // pending grows; epoch_count stays at 0.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 0u );
      BOOST_REQUIRE( state["pending_emission_amount"].as<int64_t>() > 0 );
   }

   // Lower cadence to 1 mid-period. Next advance should pay.
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(1) ) );

   // Advance 2 under cadence=1: target=2, period_start=0, condition
   // (2 >= 0 + 1 - 1 = 0) TRUE -> pay-epoch. period_emission = pending +
   // this-epoch's share; payepoch drains pending to 0 and advances
   // period_start_epoch to 3.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   {
      auto state = get_t5_state();
      BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );
      BOOST_REQUIRE_EQUAL( state["pending_emission_amount"].as<int64_t>(), 0 );
      BOOST_REQUIRE_EQUAL( state["period_start_epoch"].as<uint32_t>(), 3u );
   }
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END() // t5_emissions_tests
