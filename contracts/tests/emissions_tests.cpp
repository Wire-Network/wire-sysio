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
//                  enforcement via the gate's block path (recorded depot-locally in
//                  sysio.epoch::blocklog; the gate broadcasts nothing cross-chain)
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

#include <map>

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/opp/opp.hpp>

#include "sysio.system_tester.hpp"
#include <sysio/testing/bls_utils.hpp>

#include "finalizer_test_keys.hpp"

#include <fc/variant_object.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/slug_name.hpp>

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
   uint16_t  capex_bps;
   uint16_t  governance_bps;
   uint16_t  producer_bps;
   uint16_t  batch_op_bps;
   uint32_t  standby_end_rank;
   uint16_t  standby_bps;
   uint32_t  epoch_log_retention_count;
};
FC_REFLECT( emit_cfg_result,
   (t1_allocation)(t2_allocation)(t3_allocation)
   (t1_duration)(t2_duration)(t3_duration)(min_claimable)
   (t5_distributable)(t5_floor)
   (target_annual_decay_bps)
   (annual_initial_emission)(annual_max_emission)(annual_min_emission)
   (compute_bps)(capex_bps)(governance_bps)
   (producer_bps)(batch_op_bps)
   (standby_end_rank)(standby_bps)(epoch_log_retention_count) )

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
static constexpr uint16_t CAPEX_BPS              = 2000;
static constexpr uint16_t GOV_BPS                = 1000;
// Implicit capital reserve = whatever the three explicit shares leave behind.
// At the fixture defaults (4000 + 2000 + 1000), this is 3000 bps. Drained
// lazily via sysio.dclaim::onreward -> sysio.system::fundclaim, not paid at
// payepoch -- so it doesn't appear in t5state.total_distributed until the
// underlying fundclaim transfer happens.
// The implicit capital reserve at these defaults is
// 10000 - COMPUTE_BPS - CAPEX_BPS - GOV_BPS = 3000 bps.
static constexpr uint16_t PRODUCER_BPS           = 7000;

// Performance-based pay constants (keep in sync with emissions.cpp)
static constexpr uint32_t T_ACTIVE_PRODUCER_COUNT = 21;
static constexpr uint32_t T_STANDBY_START_RANK    = 22;
static constexpr uint32_t T_STANDBY_END_RANK      = 28;
/// Share of the producer pool reserved for the standby retainer -- 8% keeps the economics where
/// the weight-based model left them (28 of 343 weight units at full attendance).
static constexpr uint16_t T_STANDBY_BPS           = 800;
/// The fixture's default epoch (init_epoch_state) is 60s at one block per 500ms.
static constexpr uint32_t T_EPOCH_SECS            = 60;

// Helper: amount NOT transferred at payepoch when no producers / batch
// members are paid. Equals producer_pool + batch_pool (compute share, both
// undistributed) + implicit capital reserve (never paid at payepoch --
// drained lazily via fundclaim). The implicit reserve naturally absorbs
// the rounding dust from per-share split_bps floors, so the cleanest
// definition is `emission - (everything payepoch actually transfers)`,
// which collapses to `emission - capex_split - gov_split` when no
// producers / batch members are paid.
static int64_t compute_undistributed_if_no_operators(int64_t emission) {
   const int64_t capex = test_split_bps(emission, CAPEX_BPS);
   const int64_t gov   = test_split_bps(emission, GOV_BPS);
   return emission - capex - gov;
}

// ---------------------------------------------------------------------------
// Producer pay model (pay per block + a position-decaying standby retainer)
// ---------------------------------------------------------------------------

/// Block slots a pay period of `epoch_secs` (cadence 1) holds: one per 500ms block interval.
static uint64_t test_nominal_slots(uint32_t epoch_secs) {
   return static_cast<uint64_t>(epoch_secs) * 1000 / 500;
}
/// The slice of the producer pool spread over the period's slots as the per-block rate.
static int64_t test_active_pool(int64_t compute) {
   const int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   return producer_pool - test_split_bps(producer_pool, T_STANDBY_BPS);
}
/// The slice of the producer pool reserved for the standby retainer.
static int64_t test_standby_pool(int64_t compute) {
   return test_split_bps(test_split_bps(compute, PRODUCER_BPS), T_STANDBY_BPS);
}
/// What `blocks` blocks earn: the active slice over `divisor` slots (the nominal count, raised to
/// the blocks actually produced when the period ran long), truncated exactly as payepoch does.
static int64_t test_block_pay(int64_t active_pool, uint64_t blocks, uint64_t divisor) {
   return static_cast<int64_t>(static_cast<__int128>(active_pool) * blocks / divisor);
}
/// A standby POSITION's fixed share of the retainer: weight N at position 22 down to 1 at
/// T_STANDBY_END_RANK, over the constant sum of every position's weight.
static int64_t test_standby_pay(int64_t standby_pool, uint32_t position) {
   const uint64_t positions  = T_STANDBY_END_RANK + 1 - T_STANDBY_START_RANK;
   const uint64_t weight_sum = positions * (positions + 1) / 2;
   const uint64_t weight     = T_STANDBY_END_RANK + 1 - position;
   return static_cast<int64_t>(static_cast<__int128>(standby_pool) * weight / weight_sum);
}

class sysio_emissions_tester : public tester {
public:
   sysio_emissions_tester() {
      deploy_system_contract();

      base_tester::push_action(
         config::system_account_name,
         "init"_n,
         config::system_account_name,
         mvo()("version", 0)
              ("core", symbol(CORE_SYMBOL).to_string())
      );
      produce_blocks(1);

      bind_system_abi();

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
      create_accounts({ OPREG, EPOCH, CHALG, MSGCH, UWRIT });
      produce_blocks(1);

      // OPREG, EPOCH and UWRIT have real contract code set below and need the
      // full RAM policy; CHALG and MSGCH are inline-target placeholders with
      // no code deployed. Sizing the placeholders down keeps the sixth
      // allocation within nodedaddy's tier-1 SYS pool.
      for (auto acct : { OPREG, EPOCH, CHALG, MSGCH, UWRIT }) {
         if (get_roa_policy(acct, "nodedaddy"_n).is_null()) {
            const bool has_code = acct == OPREG || acct == EPOCH || acct == UWRIT;
            auto tr = addpolicy_ram_only("nodedaddy"_n, acct,
               asset::from_string(has_code ? "500.0000 SYS" : "100.0000 SYS"));
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

      // sysio.epoch::advance unconditionally inlines sysio.uwrit::chklocks,
      // so uwrit must be deployed for advance_epoch_state() to succeed.
      set_code( UWRIT, contracts::uwrit_wasm() );
      set_abi ( UWRIT, contracts::uwrit_abi().data() );
      set_privileged( UWRIT );

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

protected:
   /// Selects the minimal construction path used by sysio_fresh_deploy_tester below.
   struct deploy_only_t {};

   /// Deploy sysio.system and bind its ABI, and do nothing else -- in particular produce no block
   /// and push no action, so `onblock` has never run against this code and the contract's `global`
   /// row has never been written. Everything the full fixture does past this point (init, the
   /// token/roa/opreg/epoch bootstrap, produce_blocks) would persist that row, because each action
   /// constructs a fresh system_contract whose deferred write lands at end of action.
   explicit sysio_emissions_tester( deploy_only_t ) {
      deploy_system_contract();
      bind_system_abi();
   }

   /// Put sysio.system on chain. Shared by both construction paths so they cannot drift.
   void deploy_system_contract() {
      produce_blocks(2);

      // --- sysio.system (emissions lives here) ---
      set_code( config::system_account_name, contracts::system_wasm() );
      set_abi ( config::system_account_name, contracts::system_abi().data() );
   }

   /// Bind the sysio.system ABI serializer from the deployed account's metadata.
   void bind_system_abi() {
      const auto* accnt = control->find_account_metadata( config::system_account_name );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(accnt->abi, abi), true );
      sysio_abi_ser.set_abi( abi, abi_serializer::create_yield_function(abi_serializer_max_time) );
   }

public:
   /// Raw bytes of sysio.system's `global` singleton row, empty when the row does not exist.
   ///
   /// Spelled out rather than routed through get_row_by_id, which probes a 16-byte [scope][pk] key
   /// before falling back to the unscoped one -- this test asserts on the row's ABSENCE, so a
   /// lookup with a second way to match is the wrong instrument.
   ///
   /// kv::global keys its single entry by the table name alone: table_id = compute_table_id(name)
   /// and an 8-byte big-endian key of that same name, no scope.
   vector<char> global_row() const {
      char key_buf[chain::kv_pri_key_size];
      chain::kv_encode_be64( key_buf, "global"_n.to_uint64_t() );

      const auto& kv_idx = control->db().get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find( boost::make_tuple( config::system_account_name,
                                                chain::compute_table_id( "global"_n.to_uint64_t() ),
                                                std::string_view( key_buf, chain::kv_pri_key_size ) ) );

      vector<char> data;
      if( it != kv_idx.end() )
         data.assign( it->value.begin(), it->value.end() );
      return data;
   }

   bool global_row_exists() const { return !global_row().empty(); }

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
         "sysio.dclaim"_n, "sysio.gov"_n, "sysio.batch"_n, "sysio.ops"_n
      };
      for (auto a : accts) {
         if (!control->db().find<account_object, by_name>(a)) {
            create_accounts({ a }, false, false, false, true);
         }
      }
      produce_blocks(1);
   }

   /// Deploy the real sysio.dclaim contract on the placeholder account and
   /// mark it privileged. Required only by tests that push actions signed by
   /// sysio.dclaim (e.g. fundclaim, which `require_auth(CAPITAL_ACCOUNT)`):
   /// the sysio.* accounts get a 0-NET ROA policy, so without a deployed
   /// contract + privileged flag they cannot afford the inline transaction
   /// NET. Idempotent.
   void deploy_dclaim_for_signing() {
      const account_name DCLAIM = "sysio.dclaim"_n;
      if (get_roa_policy(DCLAIM, "nodedaddy"_n).is_null()) {
         auto tr = addpolicy_ram_only("nodedaddy"_n, DCLAIM,
            asset::from_string("500.0000 SYS"));
         BOOST_REQUIRE( tr );
         BOOST_REQUIRE( !tr->except );
         produce_blocks(1);
      }
      set_code( DCLAIM, contracts::dclaim_wasm() );
      set_abi ( DCLAIM, contracts::dclaim_abi().data() );
      set_privileged( DCLAIM );
      produce_blocks(1);
   }

   /// Deploy the real sysio.reserv contract (privileged) so the swap-fee
   /// fold-in test can seed its rewards bucket via a swap. Mirrors
   /// deploy_dclaim_for_signing's account + ROA-policy + code pattern.
   void deploy_reserv() {
      const account_name RESERV = "sysio.reserv"_n;
      if (!control->db().find<account_object, by_name>(RESERV)) {
         create_accounts({ RESERV });
         produce_blocks(1);
      }
      if (get_roa_policy(RESERV, "nodedaddy"_n).is_null()) {
         auto tr = addpolicy_ram_only("nodedaddy"_n, RESERV, asset::from_string("500.0000 SYS"));
         BOOST_REQUIRE( tr );
         BOOST_REQUIRE( !tr->except );
         produce_blocks(1);
      }
      set_code( RESERV, contracts::reserve_wasm() );
      set_abi ( RESERV, contracts::reserve_abi().data() );
      set_privileged( RESERV );
      produce_blocks(1);
   }

   /// Current balance of sysio.reserv's batch-operator rewards bucket.
   ///
   /// Requires deploy_reserv(). A missing bucket is reported as zero.
   int64_t reserv_reward_balance() {
      const account_name RESERV = "sysio.reserv"_n;
      auto data = get_row_by_account(RESERV, RESERV, "rewardbkt"_n, "rewardbkt"_n);
      if (data.empty()) return 0;

      const auto* meta = control->find_account_metadata(RESERV);
      BOOST_REQUIRE(meta != nullptr);
      abi_def def;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(meta->abi, def), true);
      abi_serializer reserv_ser;
      reserv_ser.set_abi(def, abi_serializer::create_yield_function(abi_serializer_max_time));
      auto bucket = reserv_ser.binary_to_variant(
         "rewards_bucket", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return static_cast<int64_t>(bucket["balance"].as_uint64());
   }

   /// Deploy sysio.reserv and seed its batch-operator rewards bucket with a
   /// real bootstrap-window swap fee, returning the exact accrued balance.
   int64_t seed_reserv_reward_bucket() {
      const account_name RESERV = "sysio.reserv"_n;
      const account_name UWRIT = "sysio.uwrit"_n;
      deploy_reserv();

      auto codename = [](std::string_view value) {
         return mvo()("value", fc::slug_name{value}.value);
      };
      BOOST_REQUIRE_EQUAL(success(), push_reserv_action(RESERV, "regreserve"_n, mvo()
         ("chain_code", codename("ETH"))("token_code", codename("ETH"))("reserve_code", codename("PRIMARY"))
         ("name", "eth")("description", "")
         ("initial_chain_amount", 1'000'000'000'000ULL)("initial_wire_amount", 1'000'000'000'000ULL)
         ("source_token_precision", 9u)("connector_weight_bps", 5000u)("is_private", false)("owner", name{})));
      BOOST_REQUIRE_EQUAL(success(), push_reserv_action(RESERV, "regreserve"_n, mvo()
         ("chain_code", codename("SOLANA"))("token_code", codename("SOL"))("reserve_code", codename("PRIMARY"))
         ("name", "sol")("description", "")
         ("initial_chain_amount", 1'000'000'000'000ULL)("initial_wire_amount", 1'000'000'000'000ULL)
         ("source_token_precision", 9u)("connector_weight_bps", 5000u)("is_private", false)("owner", name{})));
      BOOST_REQUIRE_EQUAL(success(), push_reserv_action(UWRIT, "applyswap"_n, mvo()
         ("src_chain_code", codename("ETH"))("src_token_code", codename("ETH"))("src_reserve_code", codename("PRIMARY"))
         ("src_amount", 1'000'000'000ULL)
         ("dst_chain_code", codename("SOLANA"))("dst_token_code", codename("SOL"))
         ("dst_reserve_code", codename("PRIMARY"))
         ("dst_amount", 100'000'000ULL)("underwriter", name{})));

      const int64_t balance = reserv_reward_balance();
      BOOST_REQUIRE_GT(balance, 0);
      return balance;
   }

   /// `sysio.reserv::wireclaims` balance owed to `acc`, or 0 when there is no row.
   /// Requires deploy_reserv(). Credited by paywire / refundwire, drained by claimwire, and
   /// reclaimed to the treasury by the retention sweep sysio.epoch::advance inlines.
   uint64_t wire_claimable( account_name acc ) {
      const account_name RESERV = "sysio.reserv"_n;
      auto data = get_row_by_account(RESERV, RESERV, "wireclaims"_n, acc);
      if (data.empty()) return 0u;

      const auto* meta = control->find_account_metadata( RESERV );
      BOOST_REQUIRE( meta != nullptr );
      abi_def def;
      BOOST_REQUIRE_EQUAL( abi_serializer::to_abi(meta->abi, def), true );
      abi_serializer reserv_ser;
      reserv_ser.set_abi( def, abi_serializer::create_yield_function(abi_serializer_max_time) );

      return reserv_ser.binary_to_variant("wire_claim", data,
                abi_serializer::create_yield_function(abi_serializer_max_time))["balance"].as_uint64();
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
      return setemitcfg(signer, default_emit_cfg(cadence));
   }

   /// The default emission config payload, split out so a caller that must push it through a
   /// different transport (see push_system_action_no_block) does not restate twenty fields.
   fc::variant_object default_emit_cfg( uint16_t cadence,
                                        int64_t annual_max_emission = ANNUAL_MAX_EMISSION,
                                        uint32_t epoch_log_retention_count = 8640 ) {
      return mvo()
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
         ("annual_max_emission",     annual_max_emission)
         ("annual_min_emission",     ANNUAL_MIN_EMISSION)
         ("compute_bps",            COMPUTE_BPS)
         ("capex_bps",              CAPEX_BPS)
         ("governance_bps",         uint16_t(1000))
         ("producer_bps",           PRODUCER_BPS)
         ("batch_op_bps",           uint16_t(3000))
         ("standby_end_rank",       T_STANDBY_END_RANK)
         ("standby_bps",            T_STANDBY_BPS)
         ("epoch_log_retention_count", epoch_log_retention_count)
         ("pay_cadence_epochs",     cadence);
   }

   /// Simulate an emission config written before the cadence bounds existed.
   /// This deliberately bypasses the current action validation and is used
   /// only to exercise the mixed-version recovery paths in sysio.epoch.
   void set_legacy_emitcfg_cadence_raw( uint16_t cadence ) {
      const auto bytes = sysio_abi_ser.variant_to_binary(
         "emission_config", default_emit_cfg(cadence),
         abi_serializer::create_yield_function(abi_serializer_max_time));

      char key_buf[chain::kv_pri_key_size];
      chain::kv_encode_be64(key_buf, "emitcfg"_n.to_uint64_t());
      auto& db = const_cast<chainbase::database&>(control->db());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(
         config::system_account_name,
         chain::compute_table_id("emitcfg"_n.to_uint64_t()),
         std::string_view(key_buf, chain::kv_pri_key_size)));
      BOOST_REQUIRE(it != kv_idx.end());
      db.modify(*it, [&](auto& row) {
         row.value.assign(bytes.data(), bytes.size());
      });
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

   action_result fundclaim( account_name signer, int64_t amount ) {
      return push_system_action(
         signer,
         "fundclaim"_n,
         mvo()("amount", amount)
      );
   }

   /// Push a sysio.system action as a READ-ONLY transaction.
   ///
   /// Read-only transactions carry no authorization and no signature, and the chain refuses any
   /// KV write attempted while one executes. Before sysio_global_state moved to
   /// kv::cached_global, ~system_contract() wrote the global unconditionally, so every action --
   /// including these pure-query view actions -- died here with table_operation_not_permitted
   /// ("cannot store a KV record when executing a readonly transaction").
   transaction_trace_ptr push_system_action_readonly( name action_name, const fc::variant_object& data ) {
      action act;
      act.account = config::system_account_name;
      act.name    = action_name;
      act.data    = sysio_abi_ser.variant_to_binary(
         sysio_abi_ser.get_action_type(action_name), data,
         abi_serializer::create_yield_function(abi_serializer_max_time));

      signed_transaction trx;
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      return push_transaction(trx, fc::time_point::maximum(), DEFAULT_BILLED_CPU_TIME_US,
                              false, transaction_metadata::trx_type::read_only);
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

   fc::variant get_batch_epoch( uint64_t sysio_epoch_index ) {
      // batch_epoch_key stores uint32_t in order-preserving big-endian form;
      // get_row_by_id is specialized for the common 8-byte integer key.
      const uint32_t index = static_cast<uint32_t>(sysio_epoch_index);
      const char key_buf[4] = {
         static_cast<char>(index >> 24),
         static_cast<char>(index >> 16),
         static_cast<char>(index >> 8),
         static_cast<char>(index),
      };
      const auto& kv_idx = control->db().get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(
         config::system_account_name,
         chain::compute_table_id("batchepochs"_n.to_uint64_t()),
         std::string_view(key_buf, sizeof(key_buf))));
      if (it == kv_idx.end()) return fc::variant();
      vector<char> data(it->value.begin(), it->value.end());
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant("batch_epoch", data,
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

   /// Blocks `producer` has made this pay period -- the row's `unpaid_blocks`, the one pay input.
   /// Read after the last block-closing call (`produce_blocks`, and every `push_system_action`
   /// such as `initt5`) and before the advance is pushed: the pending block's onblock has already
   /// counted, and the advance lands in that same pending block.
   uint32_t unpaid_blocks_of( account_name producer ) {
      auto info = get_producer_info(producer);
      BOOST_REQUIRE_MESSAGE(!info.is_null(), "no producers row for " << producer.to_string());
      return info["unpaid_blocks"].as<uint32_t>();
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
   /// Derive a producer's rank -- POSITION in the score-ordered index, counting from 1.
   ///
   /// `rank` is no longer a stored field: it is position in the "prodrank" index among schedulable
   /// producers. A test that asserts on rank therefore reproduces the contract's own ordering --
   /// ascending `rank_score`, ties broken by account name (the primary key). Scans the fixture's
   /// `producer_name_at` roster, which is every producer these fixtures create.
   ///
   /// @param target the producer whose position is wanted.
   /// @param scan   how many roster slots to consider.
   /// @return the 1-based position, or 0 when the producer holds none.
   uint32_t producer_rank_position(account_name target, uint32_t scan = 40) {
      std::vector<std::pair<uint64_t, uint64_t>> ordered;
      for (uint32_t i = 0; i < scan; ++i) {
         auto candidate = producer_name_at(i);
         auto info      = get_producer_info(candidate);
         if (info.is_null()) continue;
         if (!info["is_active"].as<bool>()) continue;
         ordered.emplace_back(info["rank_score"].as<uint64_t>(), candidate.to_uint64_t());
      }
      std::sort(ordered.begin(), ordered.end());
      for (uint32_t i = 0; i < ordered.size(); ++i) {
         if (ordered[i].second == target.to_uint64_t()) return i + 1;
      }
      return 0;
   }

   action_result register_finalizer_key(account_name act, const std::string& key, const std::string& pop) {
      return push_system_action(act, "regfinkey"_n, mvo()
         ("finalizer_name", act)("finalizer_key", key)("proof_of_possession", pop));
   }

   /// Register an active finalizer key for each of the first `count` names, and configure the node
   /// to vote with them. regfinkey auto-activates a producer's first key, which is what
   /// `producer_rank::is_schedulable` requires -- a producer without one occupies no rank position,
   /// so it is neither scheduled nor paid.
   ///
   /// The keys come from `get_bls_key(name)`, which the tester HOLDS the private half of. That is
   /// load-bearing: update_ranked_producers proposes a finalizer policy built from the registered
   /// keys, and a policy this node cannot sign for stops it voting -- LIB freezes, and a frozen LIB
   /// means a pending producer schedule never becomes final and so never activates. Deriving from
   /// the account name also gives a distinct key per producer, satisfying regfinkey's global
   /// uniqueness check without a fixed key table.
   void register_finalizer_keys(const std::vector<account_name>& names, uint32_t count) {
      std::vector<account_name> registered;
      for (uint32_t i = 0; i < count && i < names.size(); ++i) {
         auto [privkey, pubkey, pop, sig_provider] = sysio::testing::get_bls_key(names[i]);
         BOOST_REQUIRE_EQUAL(success(),
            register_finalizer_key(names[i], pubkey.to_string(), pop.to_string()));
         registered.push_back(names[i]);
      }
      set_node_finalizers(registered);
   }

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

      // Every producer needs an active finalizer key: rank is position among SCHEDULABLE producers,
      // and a producer without one holds no position -- so it is neither scheduled nor paid.
      // regfinkey stores a row on the producer, which needs RAM this fixture does not otherwise
      // grant (it does not activate the ROA / RAM market).
      for (auto& pname : prod_names) {
         BOOST_REQUIRE_EQUAL(success(), push_system_action(config::system_account_name, "setacctram"_n,
            mvo()("account", pname)("ram_bytes", int64_t(1'000'000))));
      }
      produce_blocks(1);

      register_finalizer_keys(prod_names, count);
      produce_blocks(1);

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

   /// Epoch pay credited by payepoch but not yet pulled. Zero when nothing is owed.
   ///
   /// payepoch credits `payclaims` instead of pushing `sysio.token::transfer`, because it runs
   /// inline from sysio.epoch::advance and a recipient's transfer-notify handler would otherwise
   /// be able to abort advance and halt epoch advancement chain-wide.
   int64_t pay_claimable( account_name acc ) {
      auto data = get_row_by_account(config::system_account_name, config::system_account_name,
                                     "payclaims"_n, acc);
      if (data.empty()) return 0;
      auto v = sysio_abi_ser.binary_to_variant(
         "pay_claim", data, abi_serializer::create_yield_function(abi_serializer_max_time));
      return static_cast<int64_t>(v["balance"].as_uint64());
   }

   /// Total epoch pay credited but not yet pulled, across every recipient (the `payclaimtot`
   /// singleton the contract maintains). This WIRE still sits in the treasury's token balance
   /// while being fully owed, which is why `fundclaim` and the epoch readiness gate both reserve
   /// it -- and why treasury-balance assertions must account for it.
   int64_t pay_outstanding_total() {
      auto data = get_row_by_account(config::system_account_name, config::system_account_name,
                                     "payclaimtot"_n, "payclaimtot"_n);
      if (data.empty()) return 0;
      auto v = sysio_abi_ser.binary_to_variant(
         "pay_claim_total", data, abi_serializer::create_yield_function(abi_serializer_max_time));
      return static_cast<int64_t>(v["outstanding"].as_uint64());
   }

   /// Pull `acc`'s credited epoch pay. No-op when nothing is owed. A block is produced afterwards
   /// so repeated claims across a test are distinct transactions rather than duplicates.
   void claim_pay( account_name acc ) {
      if (pay_claimable(acc) == 0) return;
      BOOST_REQUIRE_EQUAL(success(),
         push_system_action(acc, "claimpay"_n, mvo()("account_name", acc)));
      produce_blocks();
   }

   /// Balance after pulling any credited epoch pay -- the end state the previous push produced
   /// directly. Balance-based pay assertions use this so they keep measuring what was distributed.
   asset get_wire_balance_paid( account_name acc ) {
      claim_pay(acc);
      return get_wire_balance(acc);
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

protected:
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

   /// Push a sysio.system action WITHOUT crossing a block boundary.
   ///
   /// base_tester::push_action(action&&, uint64_t) calls produce_block() once the transaction
   /// lands. That is invisible to most tests and fatal to the absent-`global`-row one: the next
   /// block's onblock stamps last_pervote_bucket_fill on a fresh chain, creating the very row that
   /// test needs to stay missing. Pushing straight into the open block keeps onblock from ever
   /// running against sysio.system code. Error handling mirrors push_action so action_result
   /// comparisons (success(), wasm_assert_msg()) read the same at the call site.
   action_result push_system_action_no_block( const account_name& signer,
                                              const action_name& act_name,
                                              const variant_object& data ) {
      action act;
      act.account       = config::system_account_name;
      act.name          = act_name;
      act.authorization = vector<permission_level>{
         { signer, config::sysio_payer_name },
         { signer, config::active_name } };
      act.data = sysio_abi_ser.variant_to_binary(
         sysio_abi_ser.get_action_type(act_name), data,
         abi_serializer::create_yield_function(abi_serializer_max_time));

      signed_transaction trx;
      trx.actions.emplace_back( std::move(act) );
      set_transaction_headers(trx);
      trx.sign( get_private_key(signer, "active"), control->get_chain_id() );

      try {
         push_transaction(trx);
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
      return success();
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

   // Push an action against the (separately deployed) sysio.reserv contract.
   // Only the swap-fee fold-in test deploys reserv and uses this.
   action_result push_reserv_action(account_name signer, action_name act, const variant_object& data) {
      try {
         base_tester::push_action("sysio.reserv"_n, act, signer, data);
         return success();
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
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

protected:
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

/// sysio.system deployed onto a chain where its `global` row has never been written.
///
/// The full fixture cannot reach this state: it produces blocks after the deploy, and the first
/// onblock to run against sysio.system code stamps last_pervote_bucket_fill on a fresh chain, so
/// the row is always present by the time a test body runs. Once the row exists, seeding it and
/// merely reading it are indistinguishable -- which is why the absent-row path needs its own
/// fixture. Inherits every helper from the full fixture; only the constructor differs.
class sysio_fresh_deploy_tester : public sysio_emissions_tester {
public:
   sysio_fresh_deploy_tester() : sysio_emissions_tester( deploy_only_t{} ) {}
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
   create_user_accounts({ "nthione"_n, "nthi255"_n });

   auto r100 = addnodeowner( ROA, "nthione"_n, 100 );
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
      ("compute_bps", uint16_t(10000)) ("capex_bps", uint16_t(0)) ("governance_bps", uint16_t(0))
      ("producer_bps", uint16_t(5000)) ("batch_op_bps", uint16_t(5000))
      ("standby_end_rank", uint32_t(28))("standby_bps", T_STANDBY_BPS)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg("alice"_n, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_bad_category_bps, sysio_emissions_tester ) try {
   // compute + capex + governance must be <= 10000 (the remainder is the
   // implicit capital reserve drained lazily by fundclaim).
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(5000)) ("capex_bps", uint16_t(4000)) ("governance_bps", uint16_t(2000))
      ("producer_bps", uint16_t(5000)) ("batch_op_bps", uint16_t(5000))
      ("standby_end_rank", uint32_t(28))("standby_bps", T_STANDBY_BPS)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "compute + capex + governance BPS must be <= 10000" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_bad_compute_subsplit, sysio_emissions_tester ) try {
   auto cfg = mvo()
      ("t1_allocation", int64_t(1)) ("t2_allocation", int64_t(1)) ("t3_allocation", int64_t(1))
      ("t1_duration", uint32_t(1))  ("t2_duration", uint32_t(1))  ("t3_duration", uint32_t(1))
      ("min_claimable", int64_t(0))
      ("t5_distributable", int64_t(1)) ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", uint16_t(6940))
      ("annual_initial_emission", int64_t(1)) ("annual_max_emission", int64_t(1)) ("annual_min_emission", int64_t(0))
      ("compute_bps", uint16_t(4000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(6000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(28))("standby_bps", T_STANDBY_BPS)
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
      ("compute_bps", uint16_t(4000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(28))("standby_bps", T_STANDBY_BPS)
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
         ("compute_bps", uint16_t(4000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
         ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
         ("standby_end_rank", uint32_t(28))("standby_bps", T_STANDBY_BPS)
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
         ("compute_bps", COMPUTE_BPS)
         ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
         ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
         ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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
      ("compute_bps", uint16_t(4000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(21))("standby_bps", T_STANDBY_BPS)
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
      ("compute_bps", uint16_t(4000)) ("capex_bps", uint16_t(2000)) ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000)) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(101))("standby_bps", T_STANDBY_BPS)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "standby_end_rank exceeds safety cap" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_rejects_standby_bps_over_full, sysio_emissions_tester ) try {
   // The retainer is a slice of the producer pool; more than the whole pool is a typo.
   auto cfg = mvo(default_emit_cfg(uint16_t(1)))("standby_bps", uint16_t(10001));
   auto r = setemitcfg(config::system_account_name, cfg);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "standby_bps must be <= 10000" );
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
      ("capex_bps", uint16_t(2000))
      ("governance_bps", uint16_t(1000))
      ("producer_bps", uint16_t(7000))
      ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", uint32_t(28))("standby_bps", T_STANDBY_BPS)
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
   BOOST_REQUIRE_EQUAL( cfg.capex_bps, CAPEX_BPS );
   BOOST_REQUIRE_EQUAL( cfg.governance_bps, uint16_t(1000) );
   BOOST_REQUIRE_EQUAL( cfg.producer_bps, PRODUCER_BPS );
   BOOST_REQUIRE_EQUAL( cfg.batch_op_bps, uint16_t(3000) );
   BOOST_REQUIRE_EQUAL( cfg.standby_end_rank, T_STANDBY_END_RANK );
   BOOST_REQUIRE_EQUAL( cfg.standby_bps, T_STANDBY_BPS );
} FC_LOG_AND_RETHROW()

/// The view actions exist to be called through clio --read / send_read_only_transaction, so they
/// must execute inside a read-only transaction.
///
/// This is the regression for "cannot store a KV record when executing a readonly transaction".
/// The dispatcher instantiates system_contract as a temporary and destroys it the moment the
/// action returns; while sysio_global_state was a plain kv::global written unconditionally from
/// ~system_contract(), that destructor issued a kv_set on EVERY action and the chain refused it
/// here. The failure appeared only on the success path -- viewnodedist with a bad account still
/// reported "account is not a node owner", because sysio::check traps before the destructor runs.
/// With kv::cached_global the write happens only when an action actually mutated the global, so a
/// query issues none.
BOOST_FIXTURE_TEST_CASE( view_actions_execute_in_readonly_transaction, sysio_emissions_tester ) try {
   auto trace = push_system_action_readonly( "viewemitcfg"_n, mvo() );
   BOOST_REQUIRE( trace );
   if ( trace->except ) BOOST_FAIL( trace->except->to_detail_string() );
   BOOST_REQUIRE( !trace->action_traces.empty() );
   BOOST_REQUIRE( !trace->action_traces[0].return_value.empty() );

   // A read-only call must return exactly what a normal call returns.
   const auto ro_cfg = fc::raw::unpack<emit_cfg_result>( trace->action_traces[0].return_value );
   const auto rw_cfg = viewemitcfg();
   BOOST_REQUIRE_EQUAL( ro_cfg.t1_allocation,          rw_cfg.t1_allocation );
   BOOST_REQUIRE_EQUAL( ro_cfg.t5_distributable,       rw_cfg.t5_distributable );
   BOOST_REQUIRE_EQUAL( ro_cfg.annual_initial_emission, rw_cfg.annual_initial_emission );
   BOOST_REQUIRE_EQUAL( ro_cfg.producer_bps,           rw_cfg.producer_bps );

   // Repeatable: the query must not have left the singleton in a state that breaks the next call.
   auto again = push_system_action_readonly( "viewemitcfg"_n, mvo() );
   BOOST_REQUIRE( again );
   if ( again->except ) BOOST_FAIL( again->except->to_detail_string() );
} FC_LOG_AND_RETHROW()

/// viewnodedist's failure path was the misleading half of the bug report: it reported its own
/// error correctly even while the success path died in the destructor. Both halves must now work
/// inside a read-only transaction.
BOOST_FIXTURE_TEST_CASE( viewnodedist_readonly_reports_its_own_error, sysio_emissions_tester ) try {
   create_user_accounts({ "nobody3"_n });
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

   BOOST_REQUIRE_EXCEPTION(
      push_system_action_readonly( "viewnodedist"_n, mvo()("account_name", "nobody3"_n) ),
      sysio_assert_message_exception,
      sysio_assert_message_is( "account is not a node owner" )
   );
} FC_LOG_AND_RETHROW()

/// The missing-row half of the same regression: the global must not reach storage until an action
/// genuinely mutates it.
///
/// view_actions_execute_in_readonly_transaction cannot reach this state. Its fixture has produced
/// post-deploy blocks, so onblock has already persisted `global`, and once the row exists every
/// way of materializing defaults looks alike. The behaviour only has teeth while the row is absent.
///
/// What this pins, and what it does not. The write-suppression SEMANTICS -- that materializing a
/// default leaves the handle clean, so a pure read owes no kv_set -- belong to kv::cached_value and
/// are covered by its own unit tests, which fail when that property is broken. What is asserted
/// here is the integrated behaviour this contract depends on: a chain where nobody has written the
/// global still serves reads, a query does not bring the row into existence, the values served are
/// the declared defaults rather than a zero-initialized struct, and the first real mutation stores
/// defaults plus that change.
///
/// Order is load-bearing. Each action constructs its own system_contract and flushes on return, so
/// an action that persisted the row early would mask everything after it. setemitcfg is the probe
/// because it succeeds without touching the global at all -- the only mutators are onblock, setram,
/// setparams, the ranking updates and payepoch -- so the row must still be absent afterwards.
BOOST_FIXTURE_TEST_CASE( seeded_global_defaults_owe_no_write, sysio_fresh_deploy_tester ) try {
   // sysio_global_state::max_ram_size's in-contract default. Mirrored rather than included because
   // the tests link against the chain, not the contract.
   constexpr uint64_t DEFAULT_MAX_RAM_SIZE = 64ll*1024*1024*1024;

   // Every push below goes through push_system_action_no_block: crossing a block boundary would
   // run onblock, which writes the global itself and would mask what is being measured.

   // Nothing has written the singleton on this chain.
   BOOST_REQUIRE( !global_row_exists() );

   // An action that succeeds without mutating the global must not bring the row into existence.
   BOOST_REQUIRE_EQUAL( success(),
                        push_system_action_no_block( config::system_account_name, "setemitcfg"_n,
                                                     mvo()("cfg", default_emit_cfg(1)) ) );
   BOOST_REQUIRE( !global_row_exists() );

   // The read-only query runs with the row still missing -- the case that used to fail outright --
   // and must not create it either.
   auto trace = push_system_action_readonly( "viewemitcfg"_n, mvo() );
   BOOST_REQUIRE( trace );
   if ( trace->except ) BOOST_FAIL( trace->except->to_detail_string() );
   BOOST_REQUIRE( !trace->action_traces.empty() );
   BOOST_REQUIRE( !trace->action_traces[0].return_value.empty() );
   BOOST_REQUIRE( !global_row_exists() );

   // Seeded means the defaults are what the contract reads, not a zero-initialized struct: 64GiB
   // is already the max, so a decrease is rejected. Against a zeroed cache this would succeed.
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("ram may only be increased"),
                        push_system_action_no_block( config::system_account_name, "setram"_n,
                                                     mvo()("max_ram_size", DEFAULT_MAX_RAM_SIZE - 1) ) );
   BOOST_REQUIRE( !global_row_exists() );

   // The first genuine mutation is what persists the row.
   BOOST_REQUIRE_EQUAL( success(),
                        push_system_action_no_block( config::system_account_name, "setram"_n,
                                                     mvo()("max_ram_size", DEFAULT_MAX_RAM_SIZE + 1024) ) );
   BOOST_REQUIRE( global_row_exists() );

   // And what landed carries the mutation on top of the seeded defaults -- a value above the
   // default but below what was just stored has to be rejected on the stored value.
   BOOST_REQUIRE_EQUAL( wasm_assert_msg("ram may only be increased"),
                        push_system_action_no_block( config::system_account_name, "setram"_n,
                                                     mvo()("max_ram_size", DEFAULT_MAX_RAM_SIZE + 512) ) );
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
      ("capex_bps", uint16_t(2500))
      ("governance_bps", uint16_t(2500))
      ("producer_bps", uint16_t(5000))
      ("batch_op_bps", uint16_t(5000))
      ("standby_end_rank", uint32_t(30))("standby_bps", uint16_t(1234))
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
   BOOST_REQUIRE_EQUAL( result.standby_bps, uint16_t(1234) );
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

BOOST_FIXTURE_TEST_CASE( accrueepoch_saturates_pending_accumulator, sysio_emissions_tester ) try {
   // accrueepoch runs as an inline action on sysio.epoch::advance, so it caps the
   // pending accumulator at asset::max_amount rather than throwing or wrapping.
   // Two near-max accruals would drive the raw int64 sum past the asset ceiling;
   // assert the running total saturates there instead of overflowing negative.
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(head_secs()) ) );

   const int64_t near_max = asset::max_amount - 10;  // 2^62 - 11
   BOOST_REQUIRE_EQUAL( success(),
      push_system_action( EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", 1)("batch_group_index", 0)("per_epoch_emission", near_max) ) );
   BOOST_REQUIRE_EQUAL( near_max,
      get_t5_state()["pending_emission_amount"].as<int64_t>() );

   // Second accrual: room is only 10, so the accumulator clamps to the ceiling.
   BOOST_REQUIRE_EQUAL( success(),
      push_system_action( EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", 2)("batch_group_index", 0)("per_epoch_emission", near_max) ) );
   BOOST_REQUIRE_EQUAL( asset::max_amount,
      get_t5_state()["pending_emission_amount"].as<int64_t>() );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( payepoch_recovers_from_incomplete_batch_roster_history, sysio_emissions_tester ) try {
   // A mixed contract version can reach payepoch without any immutable roster
   // snapshots. That must retain and durably attribute the batch slice rather
   // than aborting the inline sysio.epoch::advance path chain-wide.
   const account_name BATCH_OP = "batchopa"_n;
   constexpr int64_t period_emission = 10'000;
   create_t5_holding_accounts();
   const int64_t fee_total = seed_reserv_reward_bucket();
   BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), initt5(config::system_account_name, tpsec(head_secs())));
   BOOST_REQUIRE_EQUAL(success(),
      push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", 1)("batch_group_index", 0)("per_epoch_emission", period_emission)));

   auto r = push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", 1)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", period_emission));
   BOOST_REQUIRE_EQUAL(success(), r);

   // The period completes and establishes a clean next-period boundary.
   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL(int64_t(0), state["pending_emission_amount"].as<int64_t>());
   BOOST_REQUIRE_EQUAL(uint32_t(2), state["period_start_epoch"].as<uint32_t>());

   const int64_t compute = test_split_bps(period_emission, COMPUTE_BPS);
   const int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   auto log = get_epoch_log(1);
   BOOST_REQUIRE(!log["batch_history_complete"].as_bool());
   BOOST_REQUIRE_EQUAL(compute - producer_pool,
                       log["batch_emission_retained"].as<int64_t>());
   BOOST_REQUIRE_EQUAL(int64_t(0), log["fee_distributed"].as<int64_t>());
   BOOST_REQUIRE_EQUAL(int64_t(0), log["batch_fee_retained"].as<int64_t>());
   BOOST_REQUIRE_EQUAL(fee_total, reserv_reward_balance());

   // The next complete period must pick up the same retained fee bucket. This
   // proves the recovery path did not merely leave it readable before silently
   // clearing or replacing it on the following pay epoch.
   create_accounts({ BATCH_OP }, false, false, false, true);
   BOOST_REQUIRE_EQUAL(success(),
      register_operator(BATCH_OP, OperatorType::OPERATOR_TYPE_BATCH,
                        /*is_bootstrapped*/true));
   BOOST_REQUIRE_EQUAL(success(),
      push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", 2)("batch_group_index", 0)("per_epoch_emission", period_emission)));
   BOOST_REQUIRE_EQUAL(success(),
      push_system_action(EPOCH, "rcrdbatch"_n, mvo()
         ("epoch_index", 2)("members", vector<name>{ BATCH_OP })));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", 2)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", period_emission)));

   auto recovery_log = get_epoch_log(2);
   BOOST_REQUIRE(recovery_log["batch_history_complete"].as_bool());
   BOOST_REQUIRE_EQUAL(fee_total, recovery_log["fee_distributed"].as<int64_t>());
   BOOST_REQUIRE_EQUAL(int64_t(0), reserv_reward_balance());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( payepoch_seeds_initial_roster_history_at_activation_epoch, sysio_emissions_tester ) try {
   // T5 may be initialized after sysio.epoch has already advanced. The first
   // retained roster, not literal epoch one, defines that initial period.
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), initt5(config::system_account_name, tpsec(head_secs())));
   BOOST_REQUIRE_EQUAL(success(),
      push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", 42)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
   BOOST_REQUIRE_EQUAL(success(),
      push_system_action(EPOCH, "rcrdbatch"_n, mvo()
         ("epoch_index", 42)("members", vector<name>{})));

   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", 42)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", int64_t(1))));

   BOOST_REQUIRE_EQUAL(uint32_t(43), get_t5_state()["period_start_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( payepoch_recovers_after_legacy_roster_history_exceeds_cap, sysio_emissions_tester ) try {
   // A legacy cadence above the new cap can leave more accrued epochs than
   // retained rosters. The eleventh rcrdbatch must prune its exact oldest row
   // rather than halting advance; payepoch then takes the retention/recovery
   // path and the following clean period pays normally.
   constexpr uint32_t roster_history_cap = 10;
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), initt5(config::system_account_name, tpsec(head_secs())));

   for (uint32_t epoch_index = 1; epoch_index <= roster_history_cap + 1; ++epoch_index) {
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", epoch_index)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "rcrdbatch"_n, mvo()
         ("epoch_index", epoch_index)("members", vector<name>{})));
   }

   // Reaching this point proves the cap boundary did not reject the mandatory
   // eleventh record. Its missing first snapshot takes the recovery path.
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", roster_history_cap + 1)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", int64_t(roster_history_cap + 1))));
   BOOST_REQUIRE_EQUAL(uint32_t(roster_history_cap + 2),
                       get_t5_state()["period_start_epoch"].as<uint32_t>());

   // The failed-completeness period cleared its history; the next complete
   // period is processed normally rather than inheriting stale rows.
   const uint32_t recovery_epoch = roster_history_cap + 2;
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
      ("epoch_index", recovery_epoch)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "rcrdbatch"_n, mvo()
      ("epoch_index", recovery_epoch)("members", vector<name>{})));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", recovery_epoch)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", int64_t(1))));
   BOOST_REQUIRE_EQUAL(uint32_t(recovery_epoch + 1),
                       get_t5_state()["period_start_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( payepoch_bounded_cleanup_drains_overlong_roster_history,
                         sysio_emissions_tester ) try {
   // Gapped legacy/corrupt keys bypass rcrdbatch's exact oldest-key healing and
   // can leave more than the normal ten rows. payepoch must bound each cleanup
   // transaction, retain rewards while stale rows remain, and drain the table
   // monotonically instead of attempting an unbounded erase loop.
   constexpr uint32_t overlong_rows = 25;
   constexpr uint32_t cleanup_rows = 20;
   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), initt5(config::system_account_name, tpsec(head_secs())));

   for (uint32_t i = 1; i <= overlong_rows; ++i) {
      const uint32_t epoch_index = i * 100;
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", epoch_index)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "rcrdbatch"_n, mvo()
         ("epoch_index", epoch_index)("members", vector<name>{})));
   }

   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", overlong_rows * 100)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", int64_t(overlong_rows))));

   for (uint32_t i = 1; i <= cleanup_rows; ++i) {
      BOOST_REQUIRE(get_batch_epoch(i * 100).is_null());
   }
   for (uint32_t i = cleanup_rows + 1; i <= overlong_rows; ++i) {
      BOOST_REQUIRE(!get_batch_epoch(i * 100).is_null());
   }

   // One more period removes the five stale rows plus its current row, still
   // within the bound. The following contiguous period then has clean history.
   const uint32_t recovery_epoch = overlong_rows * 100 + 1;
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
      ("epoch_index", recovery_epoch)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "rcrdbatch"_n, mvo()
      ("epoch_index", recovery_epoch)("members", vector<name>{})));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", recovery_epoch)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", int64_t(1))));
   BOOST_REQUIRE(get_batch_epoch(overlong_rows * 100).is_null());
   BOOST_REQUIRE(get_batch_epoch(recovery_epoch).is_null());

   const uint32_t clean_epoch = recovery_epoch + 1;
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
      ("epoch_index", clean_epoch)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "rcrdbatch"_n, mvo()
      ("epoch_index", clean_epoch)("members", vector<name>{})));
   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", clean_epoch)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", int64_t(1))));
   BOOST_REQUIRE(get_epoch_log(clean_epoch)["batch_history_complete"].as_bool());
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_bounds_period_accrual_to_asset_range, sysio_emissions_tester ) try {
   // The pending accumulator saturates at asset::max_amount (previous test), and
   // a saturated accumulator leaves the pay-epoch readiness gate demanding a
   // balance no account can hold -- permanently blocking epoch advancement. So
   // setemitcfg must reject any config whose worst-case pay-period accumulation
   // (per-epoch emission ceiling * pay_cadence_epochs) could reach the clamp.
   // A deliberately high annual ceiling scales to ~4.93e17 at a 30-day epoch:
   // cadence 9 fits the asset range, whereas cadence 10 must be rejected.
   constexpr int64_t high_annual_max = 6'000'000'000'000'000'000LL;
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(2'592'000) ); // 30-day epochs

   BOOST_REQUIRE_EQUAL( success(),
      setemitcfg(config::system_account_name, default_emit_cfg(uint16_t(9), high_annual_max)) );

   auto r = setemitcfg(config::system_account_name, default_emit_cfg(uint16_t(10), high_annual_max));
   BOOST_REQUIRE( r != success() );
   require_substr( r, "per-epoch emission ceiling x pay_cadence_epochs exceeds the asset range" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_caps_batch_roster_history, sysio_emissions_tester ) try {
   // The independent history cap keeps payepoch at no more than ten immutable
   // snapshots; the joint credit-work cap is covered separately below.
   BOOST_REQUIRE_EQUAL( success(),
      setemitcfg_with_cadence(config::system_account_name, uint16_t(10)) );

   auto r = setemitcfg_with_cadence(config::system_account_name, uint16_t(11));
   BOOST_REQUIRE( r != success() );
   require_substr(r, "pay_cadence_epochs exceeds batch roster history safety cap");
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( emission_config_boundaries_cap_batch_payout_credit_work,
                         sysio_emissions_tester ) try {
   // setemitcfg owns cadence changes. With the epoch roster at its maximum of
   // 100, cadence two would permit 200 distinct recipient credits and must be
   // rejected at that boundary.
   BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), init_epoch_state(60, /*operators_per_epoch*/100,
                                                   /*batch_op_groups_count*/3));
   auto r = setemitcfg_with_cadence(config::system_account_name, uint16_t(2));
   BOOST_REQUIRE(r != success());
   require_substr(r, "pay_cadence_epochs x operators_per_epoch exceeds the batch payout credit safety cap (100)");

   // sysio.epoch::setconfig owns roster-size changes and enforces the same
   // bound. Ten epochs at the normal seven-member roster fit; raising the
   // roster to eleven would permit 110 credits and is rejected.
   BOOST_REQUIRE_EQUAL(success(), init_epoch_state(60, /*operators_per_epoch*/7,
                                                   /*batch_op_groups_count*/3));
   BOOST_REQUIRE_EQUAL(success(),
      setemitcfg_with_cadence(config::system_account_name, uint16_t(10)));
   r = init_epoch_state(60, /*operators_per_epoch*/11, /*batch_op_groups_count*/3);
   BOOST_REQUIRE(r != success());
   require_substr(r, "pay_cadence_epochs x operators_per_epoch exceeds the batch payout credit safety cap (100)");
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( epoch_setconfig_names_prebootstrap_stored_cadence_in_work_bound_error,
                         sysio_emissions_tester ) try {
   // setemitcfg cannot validate the joint bound before sysio.epoch has a
   // configuration. The first epoch setconfig must reject an incompatible
   // roster size and point operators back to the already-stored system cadence.
   BOOST_REQUIRE_EQUAL(success(),
      setemitcfg_with_cadence(config::system_account_name, uint16_t(10)));

   auto r = init_epoch_state(60, /*operators_per_epoch*/11,
                             /*batch_op_groups_count*/3);
   BOOST_REQUIRE(r != success());
   require_substr(
      r,
      "stored sysio.system pay_cadence_epochs x operators_per_epoch exceeds the batch payout credit safety cap (100)");
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( epoch_setconfig_recovers_legacy_cadence_with_runtime_work_bound,
                         sysio_emissions_tester ) try {
   // A pre-bound cadence above today's history cap is shortened at runtime.
   // setconfig must validate that same effective value, while the preceding
   // boundary test continues to reject an unsafe roster change for a current,
   // otherwise-valid stored cadence.
   set_legacy_emitcfg_cadence_raw(uint16_t(1000));
   BOOST_REQUIRE_EQUAL(success(), init_epoch_state(
      60, /*operators_per_epoch*/11, /*batch_op_groups_count*/3));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( payepoch_recovers_when_legacy_rosters_exceed_credit_budget,
                         sysio_emissions_tester ) try {
   // Runtime defense-in-depth must keep malformed/legacy history from issuing
   // more than 100 expensive credit_pay calls even if it bypassed both current
   // configuration setters.
   constexpr int64_t per_epoch_emission = 10'000;
   vector<name> first_roster;
   vector<name> second_roster;
   for (uint64_t value = 1; value <= 51; ++value) {
      first_roster.emplace_back(value);
      second_roster.emplace_back(value + 100);
   }

   create_t5_holding_accounts();
   BOOST_REQUIRE_EQUAL(success(), setemitcfg_defaults(config::system_account_name));
   BOOST_REQUIRE_EQUAL(success(), initt5(config::system_account_name, tpsec(head_secs())));
   for (uint32_t epoch_index = 1; epoch_index <= 2; ++epoch_index) {
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", epoch_index)
         ("batch_group_index", 0)
         ("per_epoch_emission", per_epoch_emission)));
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "rcrdbatch"_n, mvo()
         ("epoch_index", epoch_index)
         ("members", epoch_index == 1 ? first_roster : second_roster)));
   }

   BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", 2)
      ("batch_op_groups", vector<vector<name>>{})
      ("period_emission", per_epoch_emission * 2)));

   auto log = get_epoch_log(2);
   BOOST_REQUIRE(!log["batch_history_complete"].as_bool());
   BOOST_REQUIRE_GT(log["batch_emission_retained"].as<int64_t>(), 0);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( epoch_setconfig_rejects_duration_breaking_accrual_bound, sysio_emissions_tester ) try {
   // Mirror of the previous test from the other config boundary:
   // scale_annual_to_epoch is linear in epoch_duration_sec, so a duration raise
   // can invalidate the period-accrual bound setemitcfg validated at the old
   // duration. With a high annual ceiling and maximum bounded cadence, 60s
   // epochs fit, a raise to 30-day epochs must block, and 600s still fits.
   constexpr int64_t high_annual_max = 6'000'000'000'000'000'000LL;
   BOOST_REQUIRE_EQUAL( success(),
      setemitcfg(config::system_account_name, default_emit_cfg(uint16_t(10), high_annual_max)) );

   auto r = init_epoch_state(2'592'000);
   BOOST_REQUIRE( r != success() );
   require_substr( r, "per-epoch emission ceiling x pay_cadence_epochs exceeds the asset range" );

   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(600) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( setemitcfg_post_initt5_rejects_brick_reduce, sysio_emissions_tester ) try {
   // After t5_state exists and epochs have run, setemitcfg must reject a
   // t5_distributable reduction that would make remaining (= distributable -
   // floor - total_distributed) negative. Otherwise the treasury silently
   // bricks on the next advance: the gate sees treasury exhausted and refuses to
   // advance the epoch.
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
      ("compute_bps", COMPUTE_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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
      ("compute_bps", COMPUTE_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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
   // exists, retry_count increments, last_retry_at advances. The gate state
   // is depot-local (no cross-chain broadcast), so the trx succeeds even
   // without sysio.msgch deployed.
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
      ("compute_bps",            COMPUTE_BPS)
      ("capex_bps",              CAPEX_BPS)   ("governance_bps", uint16_t(1000))
      ("producer_bps",           PRODUCER_BPS)("batch_op_bps", uint16_t(3000))
      ("standby_end_rank",       T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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
   int64_t capex     = log["capex_amount"].as<int64_t>();

   BOOST_REQUIRE_EQUAL( compute, test_split_bps(total, COMPUTE_BPS) );
   // capex gets only its base split (no producer dust redirect)
   BOOST_REQUIRE_EQUAL( capex, test_split_bps(total, CAPEX_BPS) );
   // capital is NOT in the epoch_log -- drained lazily via fundclaim, not at payepoch.
   BOOST_REQUIRE( !log.get_object().contains("capital_amount") );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( governance_gets_remainder_no_dust_loss, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t total   = log["total_emission"].as<int64_t>();
   int64_t gov     = log["governance_amount"].as<int64_t>();

   // governance is its own BPS share of total (independent of compute/capex):
   // the implicit capital reserve (drained lazily via fundclaim) is what
   // absorbs any unallocated remainder, not governance.
   BOOST_REQUIRE_EQUAL( gov, test_split_bps(total, uint16_t(1000)) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Compute distribution (producers)
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( no_producers_undistributed_stays_in_sysio, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const asset sysio_before = get_wire_balance_paid(config::system_account_name);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t capex_base = test_split_bps(emission, CAPEX_BPS);

   // Capex gets only its base split (no producer dust redirect). The category buckets are PUSHED
   // rather than credited (they can neither sign a claim nor host a contract to emit one), so
   // what capex "received" is a real balance.
   int64_t capex_received = get_wire_balance("sysio.ops"_n).get_amount();
   BOOST_REQUIRE_EQUAL( capex_received, capex_base );

   // sysio's balance decreases by (emission - producer_pool - batch_pool) since
   // both producer and batch-op pools are undistributed when no operators are
   // registered and no batch-op rotation group has been populated.
   const asset sysio_after = get_wire_balance_paid(config::system_account_name);
   // payepoch credits rather than transfers, so the treasury only sheds WIRE as
   // recipients claim. Add back what is still owed to recover the pre-change quantity.
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount() + pay_outstanding_total();
   int64_t undist = compute_undistributed_if_no_operators(emission);
   BOOST_REQUIRE_EQUAL( sysio_decrease, emission - undist );
} FC_LOG_AND_RETHROW()

// Pay is per block: every producer is credited the period's per-block rate times the blocks it
// made, and the slots nobody filled are paid to nobody -- they stay in the treasury rather than
// flowing to the producers that did show up.
// Integer division is the one way the no-forfeiture rule could be broken silently: a real block
// count over a pool too small to represent it pays zero, and consuming the count then would destroy
// work that was actually done. The blocks wait instead, for a period whose pool can pay them.
BOOST_FIXTURE_TEST_CASE( pay_rounding_to_zero_does_not_consume_the_blocks, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const auto  target = "producera"_n;
   const asset before = get_wire_balance_paid(target);

   // A period whose entire producer pool is one unit. active_pool * blocks / slot_divisor is
   // integer division, so every producer's block pay floors to zero.
   const int64_t tiny_emission = 2;
   BOOST_REQUIRE_EQUAL( success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
      ("epoch_index", 1)("batch_group_index", 0)("per_epoch_emission", tiny_emission)) );

   // Read the count LAST, immediately before the payout: every push_system_action closes a block,
   // and the target keeps producing into its own counter while the setup runs.
   const uint32_t blocks = unpaid_blocks_of(target);
   BOOST_REQUIRE_GT( blocks, 0u );
   // Fewer blocks than the period's nominal slots, so the divisor is the nominal count.
   BOOST_REQUIRE_LT( uint64_t(blocks), test_nominal_slots(T_EPOCH_SECS) );

   BOOST_REQUIRE_EQUAL( success(), push_system_action(EPOCH, "payepoch"_n, mvo()
      ("epoch_index", 1)("batch_op_groups", vector<vector<name>>{})("period_emission", tiny_emission)) );

   // Nothing was credited ...
   BOOST_REQUIRE_MESSAGE( get_wire_balance_paid(target) == before,
      "the pool was large enough to pay after all -- this test no longer exercises the rounding case" );
   // ... so nothing may be consumed. The count can only have GROWN, by the blocks the payout's own
   // transaction produced; a reset would drop it far below what it was.
   BOOST_REQUIRE_MESSAGE( unpaid_blocks_of(target) >= blocks,
      "uncredited blocks were consumed: had " << blocks << ", now " << unpaid_blocks_of(target) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( active_producers_are_paid_per_block, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);

   // Wait for schedule to activate, then produce complete cycles
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const std::vector<name> producers{ "producera"_n, "producerb"_n, "producerc"_n };
   std::map<name, uint32_t> blocks;
   std::map<name, asset>    before;
   uint64_t produced = 0;
   for (const auto& p : producers) {
      blocks.emplace(p, unpaid_blocks_of(p));
      before.emplace(p, get_wire_balance_paid(p));
      produced += blocks.at(p);
   }
   BOOST_REQUIRE_GT( produced, 0u );
   // Two rotations of three producers fill far fewer than the period's slots, so the divisor is
   // the nominal count and the unfilled slots are the treasury's.
   const uint64_t slots = test_nominal_slots(T_EPOCH_SECS);
   BOOST_REQUIRE_LT( produced, slots );

   const int64_t t5_before = get_t5_state()["total_distributed"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   const int64_t compute     = log["compute_amount"].as<int64_t>();
   const int64_t active_pool = test_active_pool(compute);
   int64_t paid = 0;
   for (const auto& p : producers) {
      const int64_t got = get_wire_balance_paid(p).get_amount() - before.at(p).get_amount();
      BOOST_REQUIRE_EQUAL( got, test_block_pay(active_pool, blocks.at(p), slots) );
      BOOST_REQUIRE_GT( got, 0 );
      paid += got;
   }
   // The unfilled slots' pay was distributed to no one.
   BOOST_REQUIRE_LT( paid, active_pool );
   BOOST_REQUIRE_EQUAL( get_t5_state()["total_distributed"].as<int64_t>() - t5_before,
                        paid + log["capex_amount"].as<int64_t>() + log["governance_amount"].as<int64_t>() );
} FC_LOG_AND_RETHROW()

// A producer cannot halt epoch pay for everyone by refusing its own payout.
//
// `sysio.token::transfer` notifies `to`, and the chain runs notified receivers with no exception
// isolation, so a recipient asserting in its transfer-notify handler aborts the WHOLE transaction.
// While payepoch pushed transfers, any ranked producer could park such a handler and abort
// payepoch -- and because payepoch runs inline from sysio.epoch::advance, that halted epoch
// advancement, emissions accrual and outbound envelope construction chain-wide, every pay-epoch,
// for as long as the producer stayed ranked.
//
// payepoch now credits `payclaims` and transfers nothing, so no recipient handler runs on the
// advance path at all. The hostile producer is still credited its full share; the only thing its
// handler can block is its own claimpay.
BOOST_FIXTURE_TEST_CASE( blocking_producer_cannot_stall_payepoch, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Park a contract on producerb that asserts on every incoming transfer. Producers are created
   // with only enough RAM to exist, so grant what the contract image needs before deploying it.
   control->get_mutable_resource_limits_manager()
      .set_account_limits("producerb"_n, 1024 * 1024, -1, -1, false);
   produce_blocks();
   set_code("producerb"_n, contracts::util::block_transfer_wasm());
   set_abi("producerb"_n, contracts::util::block_transfer_abi().data());
   produce_blocks();

   // The epoch still advances and payepoch still runs to completion. Before this change it aborted.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Every producer is credited, the blocker included -- pay is owed, not withheld.
   const int64_t owed_a = pay_claimable("producera"_n);
   const int64_t owed_b = pay_claimable("producerb"_n);
   const int64_t owed_c = pay_claimable("producerc"_n);
   BOOST_REQUIRE( owed_a > 0 );
   BOOST_REQUIRE( owed_b > 0 );   // the blocker is credited for its blocks like anyone else
   BOOST_REQUIRE( owed_c > 0 );

   // The cooperative producers pull their pay normally.
   BOOST_REQUIRE_EQUAL( success(),
      push_system_action("producera"_n, "claimpay"_n, mvo()("account_name", "producera")) );
   produce_blocks();
   BOOST_REQUIRE_EQUAL( owed_a, get_wire_balance("producera"_n).get_amount() );

   // The blocker's reach ends at its own claim: its handler rejects the incoming transfer.
   auto r = push_system_action("producerb"_n, "claimpay"_n, mvo()("account_name", "producerb"));
   BOOST_REQUIRE_MESSAGE( r != success(), "blocking producer unexpectedly claimed its own pay" );
   BOOST_REQUIRE_MESSAGE( r.find("block_transfer: rejecting incoming transfer") != std::string::npos,
                          "unexpected failure reason: " + r );

   // The failed claim rolled back whole -- still owed, not burned.
   BOOST_REQUIRE_EQUAL( owed_b, pay_claimable("producerb"_n) );
   BOOST_REQUIRE_EQUAL( 0, get_wire_balance("producerb"_n).get_amount() );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Holding account stub transfers
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( holding_accounts_receive_correct_amounts, sysio_emissions_tester ) try {
   // Category bucket recipients are fixed accounts (sysio.dclaim / sysio.gov / sysio.ops).
   // The batch-op share is NOT sent to a holding account -- it is split across the
   // members of the current sysio.epoch batch_op_groups rotation slot. When no
   // operators are registered, the entire batch_pool stays in the treasury.
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset dclaim_before   = get_wire_balance("sysio.dclaim"_n);
   asset batch_before = get_wire_balance("sysio.batch"_n);
   asset gov_before   = get_wire_balance("sysio.gov"_n);
   asset ops_before   = get_wire_balance("sysio.ops"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t gov      = log["governance_amount"].as<int64_t>();
   int64_t capex_base = test_split_bps(emission, CAPEX_BPS);

   // The category buckets are PUSHED, not credited -- the deliberate exception to the pull rule,
   // so what they "received" is a real balance delta. They are protocol-owned holding accounts
   // with no code (no notify handler to abort `advance`) and, being `sysio.*`, no net/cpu to sign
   // a claim with and no contract to emit one inline: crediting them would strand the pay in
   // `payclaims` permanently. `sysio.dclaim` is pushed for the same reason, via fundclaim.
   int64_t dclaim_received   = get_wire_balance("sysio.dclaim"_n).get_amount() - dclaim_before.get_amount();
   int64_t gov_received   = get_wire_balance("sysio.gov"_n).get_amount() - gov_before.get_amount();
   int64_t batch_received = get_wire_balance("sysio.batch"_n).get_amount() - batch_before.get_amount();
   int64_t ops_received   = get_wire_balance("sysio.ops"_n).get_amount() - ops_before.get_amount();

   // dclaim is not funded at payepoch anymore -- capital draws are lazy
   // via sysio.dclaim::onreward -> sysio.system::fundclaim.
   BOOST_REQUIRE_EQUAL( dclaim_received, 0 );
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
   // Producers holding active positions but with 0 blocks made are paid nothing
   create_t5_holding_accounts();
   setup_producers(3);
   // Do NOT produce extra blocks — schedule hasn't activated, so no producer has made a block

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance_paid("producera"_n);
   asset bal_b_before = get_wire_balance_paid("producerb"_n);
   asset bal_c_before = get_wire_balance_paid("producerc"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Producers should receive nothing (0 blocks → nothing to pay for)
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid("producera"_n), bal_a_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid("producerb"_n), bal_b_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid("producerc"_n), bal_c_before );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( partial_uptime_pays_the_per_block_rate, sysio_emissions_tester ) try {
   // A producer that made a fraction of its period's slots is paid exactly that fraction. A long
   // epoch makes the slot count dwarf the blocks the test produces (~24 of 14400), so the rate is
   // small and the pay is far below an even third of the pool.
   constexpr uint32_t EPOCH_SECS = 7200;
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(EPOCH_SECS) );
   create_t5_holding_accounts();
   setup_producers(3);

   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const uint32_t blocks_a = unpaid_blocks_of("producera"_n);
   BOOST_REQUIRE_GT( blocks_a, 0u );
   asset bal_a_before = get_wire_balance_paid("producera"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance_paid("producera"_n).get_amount() - bal_a_before.get_amount();
   BOOST_REQUIRE( got_a > 0 );

   auto log = get_epoch_log(1);
   const int64_t compute = log["compute_amount"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( got_a, test_block_pay(test_active_pool(compute), blocks_a,
                                              test_nominal_slots(EPOCH_SECS)) );
   BOOST_REQUIRE_LT( got_a, test_split_bps(compute, PRODUCER_BPS) / 3 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( standby_paid_without_block_check, sysio_emissions_tester ) try {
   // Standby producers (rank 22-28) get full weight share without performance check
   create_t5_holding_accounts();

   // Set up 24 producers: 21 active + 3 standby (ranks 22-24)
   setup_producers(24);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Standby producer (rank 22) is "producerw" (index 22)
   name standby_name = producer_name_at(21); // index 21 = 'v', rank 22
   BOOST_REQUIRE_EQUAL( 0u, unpaid_blocks_of(standby_name) );
   asset standby_before = get_wire_balance_paid(standby_name);

   // Verify the standby producer has rank 22
   auto standby_info = get_producer_info(standby_name);
   BOOST_REQUIRE( !standby_info.is_null() );
   uint32_t standby_rank = producer_rank_position(standby_name);
   BOOST_REQUIRE( standby_rank >= T_STANDBY_START_RANK && standby_rank <= T_STANDBY_END_RANK );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Standby should receive payment even with 0 blocks produced -- its POSITION's fixed share of
   // the retainer slice, not the whole slice: the vacant positions' shares stay in the treasury.
   int64_t standby_got = get_wire_balance_paid(standby_name).get_amount() - standby_before.get_amount();
   BOOST_REQUIRE( standby_got > 0 );
   const int64_t standby_pool = test_standby_pool(get_epoch_log(1)["compute_amount"].as<int64_t>());
   BOOST_REQUIRE_EQUAL( standby_got, test_standby_pay(standby_pool, standby_rank) );
   BOOST_REQUIRE_LT( standby_got, standby_pool );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( block_count_reset_after_pay, sysio_emissions_tester ) try {
   // After the pay-epoch every paid producer's block count starts over
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   BOOST_REQUIRE_GT( unpaid_blocks_of("producera"_n), 0u );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   produce_blocks(1);

   // The count is reset by payepoch; the block that followed it is produced by one of the test
   // producers, so onblock may have counted one block for producera again.
   BOOST_REQUIRE_LE( unpaid_blocks_of("producera"_n), 1u );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( total_distributed_excludes_undistributed, sysio_emissions_tester ) try {
   // When some producers are excluded, total_distributed < emission
   create_t5_holding_accounts();
   setup_producers(3);
   // No producer has made a block - nothing to pay - producer_pool undistributed.
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

// There is no round threshold: a producer that made a handful of blocks in a round it did not
// complete is paid for exactly those blocks. (A fork switch or a rough handoff costs a producer
// the blocks it lost, and nothing more.)
BOOST_FIXTURE_TEST_CASE( every_block_is_paid_without_a_round_threshold, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();

   // A few blocks past activation: whoever holds the current window has made fewer than half a
   // round, and no producer has completed one.
   produce_blocks(2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Read AFTER initt5 -- push_system_action closes a block, and its producer is credited for it.
   const std::vector<name> producers{ "producera"_n, "producerb"_n, "producerc"_n };
   std::map<name, uint32_t> blocks;
   std::map<name, asset>    before;
   uint32_t partial_producers = 0;
   for (const auto& p : producers) {
      blocks.emplace(p, unpaid_blocks_of(p));
      before.emplace(p, get_wire_balance_paid(p));
      BOOST_REQUIRE_LT( blocks.at(p), 6u );
      if (blocks.at(p) > 0) ++partial_producers;
   }
   BOOST_REQUIRE_GT( partial_producers, 0u );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t active_pool = test_active_pool(get_epoch_log(1)["compute_amount"].as<int64_t>());
   for (const auto& p : producers) {
      const int64_t got = get_wire_balance_paid(p).get_amount() - before.at(p).get_amount();
      BOOST_REQUIRE_EQUAL( got, test_block_pay(active_pool, blocks.at(p), test_nominal_slots(T_EPOCH_SECS)) );
      if (blocks.at(p) > 0) BOOST_REQUIRE_GT( got, 0 );
   }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Mid-epoch schedule changes
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( producer_promoted_mid_epoch, sysio_emissions_tester ) try {
   // Producer starts as standby, gets promoted to active mid-epoch
   // Should be paid for the blocks it made after promotion
   create_t5_holding_accounts();

   // Start with 22 producers: 21 active + 1 standby
   setup_producers(22);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // 1 cycle sufficient

   // Promote the standby (position 22) into the active band. Rank is POSITION in the score-ordered
   // index, so governance can no longer hand out ranks -- `setprodkeys` proposes a schedule and
   // nothing more. The lever that actually moves a position is the set of schedulable producers:
   // unregistering the producer holding position 1 shifts every later producer up by one, promoting
   // the standby at 22 into 21.
   BOOST_REQUIRE_EQUAL( success(), push_system_action(producer_name_at(0), "unregprod"_n,
      mvo()("producer", producer_name_at(0))) );
   produce_blocks(1);

   // `setprodkeys` still publishes a schedule -- it simply no longer assigns ranks -- so it stays
   // the way this fixture puts the promoted producer on the roster that produces blocks.
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
   uint32_t promoted_rank = producer_rank_position(promoted);
   BOOST_REQUIRE( promoted_rank >= 1 && promoted_rank <= T_ACTIVE_PRODUCER_COUNT );

   asset promoted_before = get_wire_balance_paid(promoted);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t promoted_got = get_wire_balance_paid(promoted).get_amount() - promoted_before.get_amount();
   // Should get proportional active pay (they produced blocks after promotion)
   BOOST_REQUIRE( promoted_got > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( producer_unregistered_mid_epoch, sysio_emissions_tester ) try {
   // Producer starts as active and makes blocks, then unregisters mid-epoch.
   // It holds no rank position at epoch end, so it draws neither active nor standby pay.
   create_t5_holding_accounts();

   // Start with 22 producers: 21 active + 1 standby
   setup_producers(22);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1); // producera makes blocks

   // Take producera out of the schedulable set. Rank is POSITION in the score-ordered index, so
   // governance cannot demote a producer by republishing a schedule -- `setprodkeys` proposes and
   // nothing more. `unregprod` is the real lever: it clears `is_active`, which drops the producer
   // out of every rank position.
   BOOST_REQUIRE_EQUAL( success(), push_system_action("producera"_n, "unregprod"_n,
      mvo()("producer", "producera"_n)) );
   produce_blocks(1);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // An unregistered producer holds NO rank position -- it is not merely pushed into the standby
   // band. Displacement into standby by a higher-scoring producer is a different scenario, covered
   // by the collateral-ordering tests.
   uint32_t pa_rank = producer_rank_position("producera"_n);
   BOOST_REQUIRE_EQUAL( 0u, pa_rank );

   asset demoted_before = get_wire_balance_paid("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // No position means no pay at THIS payout -- neither block pay nor the standby retainer. The
   // blocks it made before parking stay on the row and are paid at the first payout after it
   // re-registers (see the park_and_return tests).
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid("producera"_n), demoted_before );
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

   asset old_before = get_wire_balance_paid("producera"_n);
   name new_producer = producer_name_at(21);
   asset new_before = get_wire_balance_paid(new_producer);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();

   // Verify: old producer (now standby) gets standby pay if in range
   auto pa_info = get_producer_info("producera"_n);
   uint32_t pa_rank = producer_rank_position("producera"_n);
   if (pa_rank <= T_STANDBY_END_RANK) {
      int64_t old_got = get_wire_balance_paid("producera"_n).get_amount() - old_before.get_amount();
      BOOST_REQUIRE( old_got > 0 );
   }

   // Verify: new active producer gets proportional active pay
   int64_t new_got = get_wire_balance_paid(new_producer).get_amount() - new_before.get_amount();
   BOOST_REQUIRE( new_got > 0 );

   // Total distributed should be less than full emission (both had partial epochs)
   auto state = get_t5_state();
   BOOST_REQUIRE( state["total_distributed"].as<int64_t>() < emission );
} FC_LOG_AND_RETHROW()

// Every block a producer makes is paid, at the first payout where it is back in the pay walk.
// A park (`unregprod`) does not cost the blocks made before it: re-register before the payout and
// they are paid at that payout like anyone else's.
BOOST_FIXTURE_TEST_CASE( park_and_return_before_the_payout_keeps_the_blocks, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t made_before_park = unpaid_blocks_of("producera"_n);
   BOOST_REQUIRE_GT( made_before_park, 0u );

   // Park, fix "the issue", come back -- all before the payout.
   BOOST_REQUIRE_EQUAL( success(), push_system_action("producera"_n, "unregprod"_n,
      mvo()("producer", "producera"_n)) );
   BOOST_REQUIRE_GE( unpaid_blocks_of("producera"_n), made_before_park );   // the park kept them
   BOOST_REQUIRE_EQUAL( success(), push_system_action("producera"_n, "regproducer"_n, mvo()
      ("producer", "producera"_n)
      ("producer_key", get_public_key("producera"_n, "active"))
      ("url", "")("location", 0)) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const uint32_t blocks_a = unpaid_blocks_of("producera"_n);
   BOOST_REQUIRE_GE( blocks_a, made_before_park );
   asset bal_a_before = get_wire_balance_paid("producera"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t got_a = get_wire_balance_paid("producera"_n).get_amount() - bal_a_before.get_amount();
   const int64_t active_pool = test_active_pool(get_epoch_log(1)["compute_amount"].as<int64_t>());
   BOOST_REQUIRE_EQUAL( got_a, test_block_pay(active_pool, blocks_a, test_nominal_slots(T_EPOCH_SECS)) );
   BOOST_REQUIRE_GT( got_a, 0 );
} FC_LOG_AND_RETHROW()

// A park that spans a payout defers the blocks rather than losing them: nothing at that payout
// (the row sits below the walk), and the carried count is paid at the first payout after the
// return -- at that period's rate, and counted in that period's divisor.
BOOST_FIXTURE_TEST_CASE( park_across_a_payout_defers_the_blocks_to_the_return, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), push_system_action("producera"_n, "unregprod"_n,
      mvo()("producer", "producera"_n)) );
   const uint32_t carried = unpaid_blocks_of("producera"_n);
   BOOST_REQUIRE_GT( carried, 0u );

   // Payout 1: parked, so nothing -- and the count is untouched.
   asset bal_a_before = get_wire_balance_paid("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid("producera"_n), bal_a_before );
   BOOST_REQUIRE_EQUAL( carried, unpaid_blocks_of("producera"_n) );

   // Return, then run out the next period.
   BOOST_REQUIRE_EQUAL( success(), push_system_action("producera"_n, "regproducer"_n, mvo()
      ("producer", "producera"_n)
      ("producer_key", get_public_key("producera"_n, "active"))
      ("url", "")("location", 0)) );
   produce_blocks(130);

   const std::vector<name> producers{ "producera"_n, "producerb"_n, "producerc"_n };
   uint64_t produced = 0;
   for (const auto& p : producers) produced += unpaid_blocks_of(p);
   const uint32_t blocks_a = unpaid_blocks_of("producera"_n);
   BOOST_REQUIRE_GE( blocks_a, carried );
   bal_a_before = get_wire_balance_paid("producera"_n);

   // Payout 2: the carried blocks are paid with this period's, at this period's rate.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   const int64_t got_a = get_wire_balance_paid("producera"_n).get_amount() - bal_a_before.get_amount();
   const int64_t active_pool = test_active_pool(get_epoch_log(2)["compute_amount"].as<int64_t>());
   const uint64_t divisor = std::max<uint64_t>(produced, test_nominal_slots(T_EPOCH_SECS));
   BOOST_REQUIRE_EQUAL( got_a, test_block_pay(active_pool, blocks_a, divisor) );
   BOOST_REQUIRE_GT( got_a, 0 );
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
   // Verify compute + capex + governance + implicit_capital_reserve == total_emission.
   // The implicit capital reserve isn't recorded in epoch_log (drained lazily
   // by fundclaim); back it out from the curve total to confirm the math.
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto log = get_epoch_log(1);
   int64_t emission    = log["total_emission"].as<int64_t>();
   int64_t compute     = log["compute_amount"].as<int64_t>();
   int64_t capex       = log["capex_amount"].as<int64_t>();
   int64_t governance  = log["governance_amount"].as<int64_t>();

   // Each non-capital share is its own BPS split of period_emission (no
   // remainder-absorption). Implicit capital reserve = whatever's left.
   BOOST_REQUIRE_EQUAL( compute,    test_split_bps(emission, COMPUTE_BPS) );
   BOOST_REQUIRE_EQUAL( capex,      test_split_bps(emission, CAPEX_BPS) );
   BOOST_REQUIRE_EQUAL( governance, test_split_bps(emission, uint16_t(1000)) );
   const int64_t implicit_capital = emission - compute - capex - governance;
   BOOST_REQUIRE( implicit_capital >= 0 );

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

   // All recorded category amounts are positive (capital_amount is no
   // longer in epoch_log -- drained lazily via fundclaim).
   BOOST_REQUIRE( log["compute_amount"].as<int64_t>() > 0 );
   BOOST_REQUIRE( log["capex_amount"].as<int64_t>() > 0 );
   BOOST_REQUIRE( log["governance_amount"].as<int64_t>() > 0 );
   BOOST_REQUIRE( !log.get_object().contains("capital_amount") );

   // Category amounts match expected BPS splits.
   int64_t emission = log["total_emission"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( log["compute_amount"].as<int64_t>(), test_split_bps(emission, COMPUTE_BPS) );
   BOOST_REQUIRE_EQUAL( log["capex_amount"].as<int64_t>(), test_split_bps(emission, CAPEX_BPS) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: producer pay edge cases
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( all_actives_excluded_standbys_still_paid, sysio_emissions_tester ) try {
   // When no active producer has made a block, only standbys receive payment.
   create_t5_holding_accounts();

   // Set up 24 producers: 21 active + 3 standby
   // Do NOT wait for schedule or produce blocks — no active has made a block
   setup_producers(24);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Active producer has made no block
   name active = producer_name_at(0);
   name standby = producer_name_at(21);

   asset active_before  = get_wire_balance_paid(active);
   asset standby_before = get_wire_balance_paid(standby);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Active should get nothing (0 blocks)
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid(active), active_before );

   // Standby should get paid (no block production check for standbys)
   auto standby_info = get_producer_info(standby);
   uint32_t standby_rank = producer_rank_position(standby);
   if (standby_rank >= T_STANDBY_START_RANK && standby_rank <= T_STANDBY_END_RANK) {
      int64_t standby_got = get_wire_balance_paid(standby).get_amount() - standby_before.get_amount();
      BOOST_REQUIRE( standby_got > 0 );
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( single_active_producer_paid_per_block, sysio_emissions_tester ) try {
   // A lone producer is paid the per-block rate for its blocks -- never the whole pool: the slots
   // it did not fill and the standby slice both stay in the treasury.
   create_t5_holding_accounts();
   setup_producers(1);
   wait_for_producer_schedule();
   produce_complete_cycles(1, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const uint32_t blocks = unpaid_blocks_of("producera"_n);
   asset bal_before = get_wire_balance_paid("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got = get_wire_balance_paid("producera"_n).get_amount() - bal_before.get_amount();
   BOOST_REQUIRE( got > 0 );

   const int64_t compute = get_epoch_log(1)["compute_amount"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( got, test_block_pay(test_active_pool(compute), blocks,
                                            test_nominal_slots(T_EPOCH_SECS)) );
   BOOST_REQUIRE_LT( got, test_active_pool(compute) );
} FC_LOG_AND_RETHROW()

// Swap-fee rewards (sysio.reserv rewards_bucket) are folded into payepoch's
// BATCH-OPERATOR distribution only: batch ops receive them on top of their
// emission share, and producers receive none of them (producer_bps /
// batch_op_bps govern the emission split alone). End-to-end: deploy reserv,
// seed the bucket with a real swap fee, advance to the (cadence-1) pay-epoch,
// and verify the single producer is paid its emission producer_pool and NOTHING
// more, the all-empty roster leaves the bucket in reserv, and the fee is NOT
// counted against the emission treasury.
BOOST_FIXTURE_TEST_CASE( payepoch_folds_swap_fee_rewards, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const int64_t fee_total = seed_reserv_reward_bucket();

   // --- Single full-round producer; advance to the cadence-1 pay-epoch ---
   setup_producers(1);
   wait_for_producer_schedule();
   produce_complete_cycles(1, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const int64_t t5_before = get_t5_state()["total_distributed"].as<int64_t>();
   const uint32_t blocks   = unpaid_blocks_of("producera"_n);
   const int64_t bal_before = get_wire_balance_paid("producera"_n).get_amount();

   // Must NOT overdraw: payepoch queues the reserv->sysio drain ahead of the
   // payout transfers, so the swept fee lands in sysio's balance first.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t got = get_wire_balance_paid("producera"_n).get_amount() - bal_before;

   auto log = get_epoch_log(1);
   const int64_t compute       = log["compute_amount"].as<int64_t>();
   const int64_t capex         = log["capex_amount"].as<int64_t>();
   const int64_t gov           = log["governance_amount"].as<int64_t>();
   const int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   const int64_t batch_pool    = compute - producer_pool;

   // The producer received its emission share and NOTHING MORE. Swap fees pay
   // the parties that carry an individual swap — the winning underwriter and the
   // batch operators that relay it — never producers, who earn emissions for
   // securing the chain. (The share is the per-block rate times its blocks; the
   // slots it did not fill and the standby slice stay in the treasury.)
   BOOST_REQUIRE_EQUAL( got, test_block_pay(test_active_pool(compute), blocks,
                                            test_nominal_slots(T_EPOCH_SECS)) );
   BOOST_REQUIRE_LT( got, producer_pool );

   // Nothing was distributed out of the fee: this fixture has no non-empty
   // rotation group, so the bucket stays in reserv for a future payable period.
   // This is the NEGATIVE case — a batch operator actually receiving the fee is
   // `payepoch_pays_swap_fee_to_active_batch_operator` below.
   BOOST_REQUIRE_EQUAL( log["fee_distributed"].as<int64_t>(), 0 );
   BOOST_REQUIRE(log["batch_history_complete"].as_bool());
   BOOST_REQUIRE_EQUAL(log["batch_emission_retained"].as<int64_t>(), batch_pool);
   BOOST_REQUIRE_EQUAL(log["batch_fee_retained"].as<int64_t>(), int64_t(0));

   BOOST_REQUIRE_EQUAL( reserv_reward_balance(), fee_total );

   // total_distributed counts emission only (the producer's block pay + capex +
   // gov, with the unfilled slots, the standby slice and the empty batch group's
   // share staying in treasury) -- the fee is NOT charged against the emission
   // curve.
   const int64_t t5_after = get_t5_state()["total_distributed"].as<int64_t>();
   BOOST_REQUIRE_EQUAL( t5_after - t5_before, got + capex + gov );
} FC_LOG_AND_RETHROW()

// The POSITIVE counterpart: a swap fee actually reaching an ACTIVE batch
// operator's balance. `payepoch_folds_swap_fee_rewards` proves that an all-empty
// history preserves reserv custody; this positive case proves a non-empty,
// eligible roster drains and distributes the bucket.
//
// Scaled to a ONE-member rotation (operators_per_epoch = batch_op_groups = 1) so
// the arithmetic is exact rather than a proportional bound: with one group active
// for the single epoch of a cadence-1 period, that member's slice is the entire
// batch pool and the entire fee pool. Asserts the recipient's balance delta and
// the exact positive `epochlog.fee_distributed`.
BOOST_FIXTURE_TEST_CASE( payepoch_pays_swap_fee_to_active_batch_operator, sysio_emissions_tester ) try {
   const account_name BATCH_OP  = "batchopa"_n;

   create_t5_holding_accounts();
   const int64_t fee_total = seed_reserv_reward_bucket();

   // --- A one-member rotation group, ACTIVE in opreg ---
   // Bootstrapped so the ACTIVE flip bypasses the collateral gate (see
   // .claude/rules/bootstrapped-operator-invariants.md); payepoch's own filter is
   // `is_op_active(member, OPERATOR_TYPE_BATCH)`, which this satisfies.
   create_accounts( { BATCH_OP }, false, false, false, true ); // include_ram_gift
   BOOST_REQUIRE_EQUAL( success(),
      register_operator( BATCH_OP, OperatorType::OPERATOR_TYPE_BATCH, /*is_bootstrapped*/true ) );

   // operators_per_epoch = batch_op_groups = 1 -> batch_operator_minimum_active
   // is 1, so this single operator satisfies schbatchgps and fills the whole
   // window: one group, one member, paid every epoch.
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(60, /*operators_per_epoch*/1,
                                                    /*batch_op_groups_count*/1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( success(), push_epoch_action(EPOCH, "schbatchgps"_n, mvo()) );

   setup_producers(1);
   wait_for_producer_schedule();
   produce_complete_cycles(1, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const int64_t t5_before  = get_t5_state()["total_distributed"].as<int64_t>();
   const uint32_t producer_blocks = unpaid_blocks_of("producera"_n);
   const int64_t bal_before = get_wire_balance(BATCH_OP).get_amount();

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // --- The payout ---
   auto log = get_epoch_log(1);
   const int64_t compute       = log["compute_amount"].as<int64_t>();
   const int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   const int64_t batch_pool    = compute - producer_pool;

   // One group, active for the single epoch of a cadence-1 period, one member:
   // the member's slice is the whole batch pool AND the whole fee pool. The
   // emission and fee shares ride ONE credit, so the balance delta after the
   // claim is the sum.
   const int64_t got = get_wire_balance_paid(BATCH_OP).get_amount() - bal_before;
   BOOST_REQUIRE_EQUAL( got, batch_pool + fee_total );

   // The fee reached a real recipient — the assertion the negative case cannot
   // make. Exact value, not merely positive: a fee that leaked into the producer
   // pool or was double-counted would still be > 0 here.
   BOOST_REQUIRE_EQUAL( log["fee_distributed"].as<int64_t>(), fee_total );
   BOOST_REQUIRE(log["batch_history_complete"].as_bool());
   BOOST_REQUIRE_EQUAL(log["batch_emission_retained"].as<int64_t>(), int64_t(0));
   BOOST_REQUIRE_EQUAL(log["batch_fee_retained"].as<int64_t>(), int64_t(0));

   // Bucket swept, and the fee is NOT charged against the emission curve —
   // total_distributed moves by the EMISSION only, excluding fee_total.
   BOOST_REQUIRE_EQUAL( reserv_reward_balance(), 0 );
   const int64_t capex = log["capex_amount"].as<int64_t>();
   const int64_t gov   = log["governance_amount"].as<int64_t>();
   const int64_t t5_after = get_t5_state()["total_distributed"].as<int64_t>();
   const int64_t producer_pay = test_block_pay(test_active_pool(compute), producer_blocks,
                                               test_nominal_slots(T_EPOCH_SECS));
   BOOST_REQUIRE_EQUAL( t5_after - t5_before, producer_pay + batch_pool + capex + gov );
} FC_LOG_AND_RETHROW()

// Lowering pay_cadence_epochs MID-PERIOD must not multiply the payout.
// `accrueepoch` increments one batch_group_epochs slot per epoch unconditionally,
// while `setemitcfg` may change pay_cadence_epochs at any time. Normalizing by
// cfg.pay_cadence_epochs instead of the accrued total made those two disagree:
// cadence 3 -> 1 after one accrual leaves the counters summing to 2 against a
// divisor of 1, paying 2x batch_pool AND 2x fee_batch_pool. The surplus fee is the
// dangerous half — only ONE fee pool was swept from sysio.reserv, so the extra is
// drawn from this treasury, and fee payouts are excluded from total_distributed,
// so it never shows up against the emission curve.
//
// Runs with an ACTIVE batch group and a NON-ZERO fee, because with either absent
// the overpayment is unobservable: no group means nothing is distributed, and a
// zero fee makes the fee half of the bug invisible.
BOOST_FIXTURE_TEST_CASE( cadence_drop_midperiod_does_not_multiply_batch_fee_payout,
                         sysio_emissions_tester ) try {
   const account_name BATCH_OP = "batchopb"_n;

   create_t5_holding_accounts();
   const int64_t fee_total = seed_reserv_reward_bucket();

   create_accounts( { BATCH_OP }, false, false, false, true );
   BOOST_REQUIRE_EQUAL( success(),
      register_operator( BATCH_OP, OperatorType::OPERATOR_TYPE_BATCH, /*is_bootstrapped*/true ) );
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(60, /*operators_per_epoch*/1,
                                                    /*batch_op_groups_count*/1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( success(), push_epoch_action(EPOCH, "schbatchgps"_n, mvo()) );

   setup_producers(1);
   wait_for_producer_schedule();
   produce_complete_cycles(1, 2);

   // Cadence 3: the first advance accrues without paying.
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(3) ) );
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   BOOST_REQUIRE_EQUAL( get_t5_state()["epoch_count"].as<uint64_t>(), 0u ); // non-pay
   BOOST_REQUIRE_GT( get_t5_state()["pending_emission_amount"].as<int64_t>(), 0 );

   // Drop to cadence 1 mid-period. The counters now sum to 2 while cfg says 1.
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_with_cadence( config::system_account_name, uint16_t(1) ) );

   const int64_t bal_before = get_wire_balance(BATCH_OP).get_amount();
   produce_blocks(130);
   const uint32_t producer_blocks = unpaid_blocks_of("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );   // pay-epoch
   BOOST_REQUIRE_EQUAL( get_t5_state()["epoch_count"].as<uint64_t>(), 1u );

   auto log = get_epoch_log(2);
   const int64_t compute       = log["compute_amount"].as<int64_t>();
   const int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   const int64_t batch_pool    = compute - producer_pool;

   // The single group holds BOTH accrued epochs, so normalizing by the accrued
   // total (2) gives it the whole pool exactly once -- not twice.
   const int64_t got = get_wire_balance_paid(BATCH_OP).get_amount() - bal_before;
   BOOST_REQUIRE_EQUAL( got, batch_pool + fee_total );

   // The load-bearing assertion: the fee is distributed ONCE. Under the old
   // divisor this was 2 * fee_total, with the surplus drawn from the treasury.
   BOOST_REQUIRE_EQUAL( log["fee_distributed"].as<int64_t>(), fee_total );
   BOOST_REQUIRE_EQUAL( reserv_reward_balance(), 0 );

   // And the emission side is not double-paid either: the producer's count spans both accrued
   // epochs and is paid once, over the two epochs' worth of slots.
   const int64_t capex = log["capex_amount"].as<int64_t>();
   const int64_t gov   = log["governance_amount"].as<int64_t>();
   const int64_t producer_pay = test_block_pay(test_active_pool(compute), producer_blocks,
                                               2 * test_nominal_slots(T_EPOCH_SECS));
   BOOST_REQUIRE_EQUAL( get_t5_state()["total_distributed"].as<int64_t>(),
                        producer_pay + batch_pool + capex + gov );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( standby_weight_decreases_by_rank, sysio_emissions_tester ) try {
   // Position 22 receives more than 23, which receives more than 24, etc. -- each an exact,
   // position-fixed share of the retainer slice: weight 29 - position (22→7, 23→6, 24→5) over
   // the constant sum 28, so the four vacant positions' shares stay in the treasury.
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

   // The fixture's setprodkeys schedule names all 25 producers until the ranked rebuild trims it
   // to 21, so a standby may have held a window -- and block pay is not gated on position, so
   // those blocks are paid on top of the retainer. Expect both, over the roster-wide divisor
   // (one 21-producer rotation already exceeds the 120 slots a 60s period holds).
   uint64_t produced = 0;
   for (uint32_t i = 0; i < 25; ++i) produced += unpaid_blocks_of(producer_name_at(i));
   const uint64_t divisor = std::max<uint64_t>(produced, test_nominal_slots(T_EPOCH_SECS));
   const uint32_t blocks1 = unpaid_blocks_of(sb1), blocks2 = unpaid_blocks_of(sb2), blocks3 = unpaid_blocks_of(sb3);

   asset sb1_before = get_wire_balance_paid(sb1);
   asset sb2_before = get_wire_balance_paid(sb2);
   asset sb3_before = get_wire_balance_paid(sb3);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got1 = get_wire_balance_paid(sb1).get_amount() - sb1_before.get_amount();
   int64_t got2 = get_wire_balance_paid(sb2).get_amount() - sb2_before.get_amount();
   int64_t got3 = get_wire_balance_paid(sb3).get_amount() - sb3_before.get_amount();

   // Higher rank (lower number) should get more: got1 > got2 > got3
   BOOST_REQUIRE( got1 > got2 );
   BOOST_REQUIRE( got2 > got3 );
   BOOST_REQUIRE( got3 > 0 );

   const int64_t compute      = get_epoch_log(1)["compute_amount"].as<int64_t>();
   const int64_t standby_pool = test_standby_pool(compute);
   const int64_t active_pool  = test_active_pool(compute);
   BOOST_REQUIRE_EQUAL( got1, test_standby_pay(standby_pool, 22) + test_block_pay(active_pool, blocks1, divisor) );
   BOOST_REQUIRE_EQUAL( got2, test_standby_pay(standby_pool, 23) + test_block_pay(active_pool, blocks2, divisor) );
   BOOST_REQUIRE_EQUAL( got3, test_standby_pay(standby_pool, 24) + test_block_pay(active_pool, blocks3, divisor) );
   // The retainer alone never exhausts its slice: four positions are vacant.
   BOOST_REQUIRE_LT( test_standby_pay(standby_pool, 22) + test_standby_pay(standby_pool, 23)
                     + test_standby_pay(standby_pool, 24), standby_pool );
} FC_LOG_AND_RETHROW()

// A single standby holds position 22's share alone: the six vacant positions pay nobody.
BOOST_FIXTURE_TEST_CASE( vacant_standby_positions_pay_nobody, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(22);
   wait_for_producer_schedule();
   produce_complete_cycles(21, 1);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const name standby = producer_name_at(21);
   BOOST_REQUIRE_EQUAL( 22u, producer_rank_position(standby) );
   asset standby_before = get_wire_balance_paid(standby);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t standby_pool = test_standby_pool(get_epoch_log(1)["compute_amount"].as<int64_t>());
   const int64_t got = get_wire_balance_paid(standby).get_amount() - standby_before.get_amount();
   BOOST_REQUIRE_EQUAL( got, test_standby_pay(standby_pool, 22) );
   BOOST_REQUIRE_EQUAL( got, standby_pool * 7 / 28 );
} FC_LOG_AND_RETHROW()

// A period that runs long holds more blocks than its nominal slots (an epoch can extend while a
// batch operator delivers). The divisor rises to the blocks actually produced, so the rate scales
// down and the active slice is never exceeded.
BOOST_FIXTURE_TEST_CASE( period_running_long_scales_the_rate_down, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   // Four rotations of three producers: 144 blocks, past the 120 slots a 60s period holds.
   produce_complete_cycles(3, 4);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const std::vector<name> producers{ "producera"_n, "producerb"_n, "producerc"_n };
   std::map<name, uint32_t> blocks;
   std::map<name, asset>    before;
   uint64_t produced = 0;
   for (const auto& p : producers) {
      blocks.emplace(p, unpaid_blocks_of(p));
      before.emplace(p, get_wire_balance_paid(p));
      produced += blocks.at(p);
   }
   BOOST_REQUIRE_GT( produced, test_nominal_slots(T_EPOCH_SECS) );

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t active_pool = test_active_pool(get_epoch_log(1)["compute_amount"].as<int64_t>());
   int64_t paid = 0;
   for (const auto& p : producers) {
      const int64_t got = get_wire_balance_paid(p).get_amount() - before.at(p).get_amount();
      BOOST_REQUIRE_EQUAL( got, test_block_pay(active_pool, blocks.at(p), produced) );
      paid += got;
   }
   BOOST_REQUIRE_LE( paid, active_pool );
   // Within rounding of the whole slice: every slot the period held was filled.
   BOOST_REQUIRE_GT( paid, active_pool - static_cast<int64_t>(producers.size()) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: sysio balance accounting
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( sysio_balance_decreases_by_distributed_amount, sysio_emissions_tester ) try {
   // The sysio account should lose exactly (emission - undistributed_producer) in WIRE balance
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset sysio_before = get_wire_balance_paid(config::system_account_name);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   asset sysio_after = get_wire_balance_paid(config::system_account_name);

   auto state = get_t5_state();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   // payepoch commits `distributed` but no longer MOVES it -- recipients are credited and
   // the WIRE stays in the treasury's balance until each one pulls it. The conservation law is
   // therefore "what left + what is owed == what was distributed", which reduces to the original
   // assertion once every recipient has claimed.
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   BOOST_REQUIRE_EQUAL( sysio_decrease + pay_outstanding_total(), distributed );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( sysio_balance_with_producers, sysio_emissions_tester ) try {
   // When producers ARE paid, sysio loses emission minus only the rounding dust
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset sysio_before = get_wire_balance_paid(config::system_account_name);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   asset sysio_after = get_wire_balance_paid(config::system_account_name);

   auto state = get_t5_state();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   // payepoch credits rather than transfers, so the treasury only sheds WIRE as
   // recipients claim. Add back what is still owed to recover the pre-change quantity.
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount() + pay_outstanding_total();
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

   asset beyond1_before = get_wire_balance_paid(beyond1);
   asset beyond2_before = get_wire_balance_paid(beyond2);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto b1_info = get_producer_info(beyond1);
   auto b2_info = get_producer_info(beyond2);

   // Only check if their rank is actually > 28
   if (!b1_info.is_null() && producer_rank_position(beyond1) > T_STANDBY_END_RANK) {
      BOOST_REQUIRE_EQUAL( get_wire_balance_paid(beyond1), beyond1_before );
   }
   if (!b2_info.is_null() && producer_rank_position(beyond2) > T_STANDBY_END_RANK) {
      BOOST_REQUIRE_EQUAL( get_wire_balance_paid(beyond2), beyond2_before );
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
   asset last_before = get_wire_balance_paid(last_standby);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   auto info = get_producer_info(last_standby);
   if (!info.is_null() && producer_rank_position(last_standby) == T_STANDBY_END_RANK) {
      int64_t got = get_wire_balance_paid(last_standby).get_amount() - last_before.get_amount();
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

   asset bal_a_before = get_wire_balance_paid("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   // Inactive producer should receive nothing
   auto pa_info = get_producer_info("producera"_n);
   if (!pa_info.is_null() && !pa_info["is_active"].as<bool>()) {
      BOOST_REQUIRE_EQUAL( get_wire_balance_paid("producera"_n), bal_a_before );
   }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Additional coverage: round tracking correctness
// ---------------------------------------------------------------------------

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

   const uint32_t blocks_a = unpaid_blocks_of("producera"_n);
   const uint32_t blocks_c = unpaid_blocks_of("producerc"_n);
   asset bal_a_before = get_wire_balance_paid("producera"_n);
   asset bal_b_before = get_wire_balance_paid("producerb"_n);
   asset bal_c_before = get_wire_balance_paid("producerc"_n);

   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   int64_t got_a = get_wire_balance_paid("producera"_n).get_amount() - bal_a_before.get_amount();
   int64_t got_b = get_wire_balance_paid("producerb"_n).get_amount() - bal_b_before.get_amount();
   int64_t got_c = get_wire_balance_paid("producerc"_n).get_amount() - bal_c_before.get_amount();

   BOOST_REQUIRE_EQUAL( got_b, 0 );
   BOOST_REQUIRE( got_a > 0 );
   BOOST_REQUIRE( got_c > 0 );
   // producera / producerc are paid exactly their own blocks at the period's rate; producerb's
   // slots' pay does not flow to them.
   const int64_t active_pool = test_active_pool(get_epoch_log(1)["compute_amount"].as<int64_t>());
   BOOST_REQUIRE_EQUAL( got_a, test_block_pay(active_pool, blocks_a, test_nominal_slots(T_EPOCH_SECS)) );
   BOOST_REQUIRE_EQUAL( got_c, test_block_pay(active_pool, blocks_c, test_nominal_slots(T_EPOCH_SECS)) );
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

   asset sysio_before = get_wire_balance_paid(config::system_account_name);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   asset sysio_after = get_wire_balance_paid(config::system_account_name);

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();

   // With all producers unregistered in opreg, the producer_pool is fully
   // undistributed (same baseline as "no producers at all"). Batch pool also
   // stays in treasury.
   int64_t expected_baseline = emission - compute_undistributed_if_no_operators(emission);
   // payepoch credits rather than transfers, so the treasury only sheds WIRE as
   // recipients claim. Add back what is still owed to recover the pre-change quantity.
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount() + pay_outstanding_total();
   BOOST_REQUIRE_EQUAL( sysio_decrease, expected_baseline );
} FC_LOG_AND_RETHROW()

// Node-owner vesting draws on the same `sysio` WIRE balance that backs unclaimed epoch pay, so it
// has to respect the same reserve `fundclaim` and the epoch gate hold. Without that, a vested owner
// can withdraw WIRE already promised to a `payclaims` row, leaving the later `claimpay` unpayable
// and `payepoch` balance-blocked from then on. That exposure is new: before payouts became
// claimable, epoch pay had already left the treasury by the time a node claim ran.
BOOST_FIXTURE_TEST_CASE( claimnodedis_reserves_outstanding_epoch_pay, sysio_emissions_tester ) try {
   create_t5_holding_accounts();

   // A fully vested T1 owner: claimable == the whole allocation.
   create_user_accounts({ "noderesv"_n });
   const uint32_t start = head_secs() - (T1_DURATION + 10);
   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), addnodeowner( ROA, "noderesv"_n, 1 ) );

   const auto vested = viewnodedist( "noderesv"_n );
   BOOST_REQUIRE( vested.can_claim );
   const int64_t claimable = vested.claimable.get_amount();
   BOOST_REQUIRE_GT( claimable, 0 );

   // Credit epoch pay so something is outstanding, using the real payepoch path: one ACTIVE
   // bootstrapped batch operator in a single-member rotation group takes the whole batch pool.
   const account_name BATCH_OP = "batchresv"_n;
   create_accounts( { BATCH_OP }, false, false, false, true );
   BOOST_REQUIRE_EQUAL( success(),
      register_operator( BATCH_OP, OperatorType::OPERATOR_TYPE_BATCH, /*is_bootstrapped*/true ) );
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(60, /*operators_per_epoch*/1,
                                                    /*batch_op_groups_count*/1) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( success(), push_epoch_action(EPOCH, "schbatchgps"_n, mvo()) );
   setup_producers(1);
   wait_for_producer_schedule();
   produce_complete_cycles(1, 2);
   const uint32_t t5_start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(t5_start) ) );
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );

   const int64_t outstanding = pay_outstanding_total();
   BOOST_REQUIRE_GT( outstanding, 0 );

   // Leave the treasury able to cover the outstanding pay but NOT that plus this node claim.
   create_user_accounts({ "resvdrain"_n });
   const int64_t balance = get_wire_balance(config::system_account_name).get_amount();
   const int64_t target  = outstanding + claimable - 1;   // one short of both
   BOOST_REQUIRE_GT( balance, target );
   base_tester::push_action(
      TOKEN, "transfer"_n,
      vector<permission_level>{{ config::system_account_name, "active"_n }},
      mvo()("from", config::system_account_name)
           ("to", "resvdrain"_n)
           ("quantity", asset(balance - target, WIRE_SYMBOL))
           ("memo", "leave the treasury one short of pay + node claim")
   );
   produce_blocks(1);

   // The withdrawal would eat into WIRE already owed to `payclaims`, so it is refused -- and
   // nothing is spent: the outstanding pay is still fully backed.
   auto blocked = claimnodedis( "noderesv"_n, "noderesv"_n );
   BOOST_REQUIRE( blocked != success() );
   require_substr( blocked, "treasury balance is reserved against unclaimed epoch pay" );
   BOOST_REQUIRE_EQUAL( get_wire_balance(config::system_account_name).get_amount(), target );
   BOOST_REQUIRE_EQUAL( pay_outstanding_total(), outstanding );

   // Fund the treasury by exactly the missing unit and the same claim goes through: the reserve
   // gates on real headroom, it does not block node owners categorically.
   base_tester::push_action(
      TOKEN, "transfer"_n,
      vector<permission_level>{{ "resvdrain"_n, "active"_n }},
      mvo()("from", "resvdrain"_n)
           ("to", config::system_account_name)
           ("quantity", asset(1, WIRE_SYMBOL))
           ("memo", "one unit of headroom")
   );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( success(), claimnodedis( "noderesv"_n, "noderesv"_n ) );

   // The point of the reserve: the epoch pay promised earlier is STILL fully backed after the
   // node owner withdrew. Its claim is untouched and the balance can honour it.
   BOOST_REQUIRE_EQUAL( pay_outstanding_total(), outstanding );
   BOOST_REQUIRE_GE( get_wire_balance(config::system_account_name).get_amount(), outstanding );

   // And it really is payable, not merely covered on paper.
   BOOST_REQUIRE_EQUAL(success(),
      push_system_action(BATCH_OP, "claimpay"_n, mvo()("account_name", BATCH_OP)));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( advance_gate_blocks_on_insufficient_treasury_balance, sysio_emissions_tester ) try {
   // The fixture funds sysio with 1_000_000_000 WIRE which easily covers the
   // default emission schedule. If sysio's balance is drained below the next
   // epoch's emission, sysio.epoch's readiness gate refuses to advance: the
   // epoch index stays at 0 and a blocklog row is written. The gate state is
   // depot-local -- nothing is queued for the outposts.
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

   // advance must succeed (no throw -- the gate records the block and returns cleanly).
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

// A balance-blocked epoch reclaims forfeited WIRE and unblocks ITSELF, with no manual sweep and
// no top-up. This pins the placement of the `sysio.reserv::sweepclaims` inline in advance: it sits
// BEFORE the emissions gate, so a BALANCE_INSUFFICIENT epoch still queues the reclaim on its way
// out. Move that call below the gate (where the other maintenance sweeps live) and the epoch
// returns without ever reclaiming the WIRE that covers its own shortfall -- every retry repeating
// it, with `sweepclaims` taking only epoch/reserv authority so no keeper can break the cycle.
//
// The two-attempt shape is the guarantee, not an artifact: `action.send()` QUEUES the inline, so
// this advance's gate has already read the treasury by the time the reclaim executes. The first
// attempt therefore records the block AND performs the reclaim; the next chkcons retry sees the
// larger balance and advances.
BOOST_FIXTURE_TEST_CASE( expired_wire_claims_unblock_a_balance_blocked_epoch, sysio_emissions_tester ) try {
   const account_name RESERV = "sysio.reserv"_n;

   create_t5_holding_accounts();
   deploy_reserv();

   auto codename = [](std::string_view s) { return mvo()("value", fc::slug_name{s}.value); };

   // Move real WIRE into reserv custody so a claim has backing. regreserve is bootstrap-window
   // only, which holds here: current_epoch_index is still 0.
   constexpr uint64_t RESERVE_SEED = 1'000'000'000'000ULL;
   BOOST_REQUIRE_EQUAL( success(), push_reserv_action(RESERV, "regreserve"_n, mvo()
      ("chain_code", codename("ETH"))("token_code", codename("ETH"))("reserve_code", codename("PRIMARY"))
      ("name", "eth")("description", "")
      ("initial_chain_amount", RESERVE_SEED)("initial_wire_amount", RESERVE_SEED)
      ("source_token_precision", 9u)("connector_weight_bps", 5000u)("is_private", false)("owner", name{}) ) );

   // Fund the escrow the refund gives back, SEPARATELY from the reserve's booked liquidity.
   // `regreserve` books RESERVE_SEED into `reserve_wire_amount`, and `refundwire` credits a claim
   // without debiting that row — in production it is reached only after `swapfromwire` has already
   // deposited the user's in-flight escrow on top. Skipping that deposit would make the sweep hand
   // the treasury registered reserve liquidity instead of forfeited escrow, so the test would
   // unblock emissions by breaking reserv's custody invariant rather than by reclaiming a claim.
   constexpr uint64_t FORFEIT = 100'000'000'000ULL;
   base_tester::push_action(
      TOKEN, "transfer"_n,
      vector<permission_level>{{ config::system_account_name, "active"_n }},
      mvo()("from", config::system_account_name)
           ("to", RESERV)
           ("quantity", asset(static_cast<int64_t>(FORFEIT), WIRE_SYMBOL))
           ("memo", "in-flight swap-from-WIRE escrow the refund returns")
   );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( RESERVE_SEED + FORFEIT,
                        static_cast<uint64_t>(get_wire_balance(RESERV).get_amount()) );

   // A swap-from-WIRE refund credits a claimable balance that nobody ever pulls. Custody now
   // reads `reserve_wire_amount (RESERVE_SEED) + Σ wireclaims (FORFEIT)`.
   create_user_accounts({ "lapseduser"_n });
   BOOST_REQUIRE_EQUAL( success(), push_reserv_action(UWRIT, "refundwire"_n, mvo()
      ("recipient",      "lapseduser")
      ("wire_amount",    FORFEIT)
      ("revert_fee_bps", 0)) );
   BOOST_REQUIRE_EQUAL( FORFEIT, wire_claimable("lapseduser"_n) );

   // Age past the one-year window with NO further credit, so `credit_wire_claim`'s opportunistic
   // sweep never fires and the epoch-driven one is the only thing that can collect the row.
   produce_block();
   produce_block(fc::days(366));
   produce_blocks(2);
   BOOST_REQUIRE_EQUAL( FORFEIT, wire_claimable("lapseduser"_n) );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Epoch 1's emission is the annual initial scaled to the fixture's 60s epoch; it is also the
   // period total, since nothing has accrued yet.
   const int64_t period_emission = test_scale_annual_to_epoch(ANNUAL_INITIAL_EMISSION, 60);
   BOOST_REQUIRE_GT( period_emission, static_cast<int64_t>(FORFEIT) );

   // Leave the treasury short by EXACTLY the forfeited claim.
   create_user_accounts({ "lapsedrain"_n });
   const int64_t balance = get_wire_balance(config::system_account_name).get_amount();
   const int64_t target  = period_emission - static_cast<int64_t>(FORFEIT);
   BOOST_REQUIRE_GT( balance, target );
   base_tester::push_action(
      TOKEN, "transfer"_n,
      vector<permission_level>{{ config::system_account_name, "active"_n }},
      mvo()("from", config::system_account_name)
           ("to", "lapsedrain"_n)
           ("quantity", asset(balance - target, WIRE_SYMBOL))
           ("memo", "leave the treasury one forfeited claim short of the epoch emission")
   );
   produce_blocks(1);

   // FIRST attempt: blocked on the balance the gate could see, and the queued reclaim still runs.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto blocked = get_blocklog_row(1u);
   BOOST_REQUIRE( !blocked.is_null() );
   BOOST_REQUIRE_EQUAL( blocked["reason"].as_string(), "EMISSIONS_BLOCK_REASON_BALANCE_INSUFFICIENT" );
   BOOST_REQUIRE_EQUAL( blocked["attempted_emission"].as<int64_t>(), period_emission );
   BOOST_REQUIRE_EQUAL( get_t5_state()["last_epoch_index"].as<uint32_t>(), 0u );

   // The forfeited claim is gone and its WIRE is in the treasury -- reclaimed by the same
   // transaction that recorded the block.
   BOOST_REQUIRE_EQUAL( 0u, wire_claimable("lapseduser"_n) );
   BOOST_REQUIRE_EQUAL( period_emission, get_wire_balance(config::system_account_name).get_amount() );

   // What moved was the ESCROW, not the reserve. Custody is back to exactly the booked
   // `reserve_wire_amount`, so the epoch unblocked itself on forfeited value rather than on
   // registered liquidity.
   BOOST_REQUIRE_EQUAL( RESERVE_SEED, static_cast<uint64_t>(get_wire_balance(RESERV).get_amount()) );

   // SECOND attempt: no manual sweepclaims, no funding -- the epoch advances on its own.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state(EPOCH) );

   auto est = get_epoch_state_row();
   BOOST_REQUIRE( !est.is_null() );
   BOOST_REQUIRE_EQUAL( est["current_epoch_index"].as<uint32_t>(), 1u );
   BOOST_REQUIRE( get_blocklog_row(1u).is_null() );
   BOOST_REQUIRE_EQUAL( get_t5_state()["last_epoch_index"].as<uint32_t>(), 1u );
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
      ("compute_bps", COMPUTE_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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
      ("compute_bps", COMPUTE_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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

BOOST_FIXTURE_TEST_CASE( epochlog_retention_counts_payment_rows_not_epoch_distance,
                         sysio_emissions_tester ) try {
   // With cadence > 1, pay-epoch indexes are spaced apart. Retention is a row
   // count, so an index-distance calculation would prematurely prune rows.
   create_t5_holding_accounts();
   auto cfg = default_emit_cfg(uint16_t(2), ANNUAL_MAX_EMISSION, uint32_t(3));
   BOOST_REQUIRE_EQUAL(success(), setemitcfg(config::system_account_name, cfg));
   BOOST_REQUIRE_EQUAL(success(), initt5(config::system_account_name, tpsec(head_secs())));

   for (const uint32_t epoch_index : {1u, 3u, 5u, 7u, 9u}) {
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "accrueepoch"_n, mvo()
         ("epoch_index", epoch_index)("batch_group_index", 0)("per_epoch_emission", int64_t(1))));
      BOOST_REQUIRE_EQUAL(success(), push_system_action(EPOCH, "payepoch"_n, mvo()
         ("epoch_index", epoch_index)
         ("batch_op_groups", vector<vector<name>>{})
         ("period_emission", int64_t(1))));
   }

   BOOST_REQUIRE(get_epoch_log(1).is_null());
   BOOST_REQUIRE(get_epoch_log(3).is_null());
   BOOST_REQUIRE(!get_epoch_log(5).is_null());
   BOOST_REQUIRE(!get_epoch_log(7).is_null());
   BOOST_REQUIRE(!get_epoch_log(9).is_null());
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

// WNS-13 / WIRE-343: advance() slides the schedule before it queues accrual,
// so the position-based counter cannot identify the roster that accrued a past
// epoch. A cadence-two period must split the batch pool between its two
// historical rosters, and clearing that history must make the next period work
// independently as well.
BOOST_FIXTURE_TEST_CASE( pay_cadence_rotating_batch_rosters_receive_their_own_epochs,
                         sysio_emissions_tester ) try {
   const account_name BATCH_OP_A = "batchopa"_n;
   const account_name BATCH_OP_B = "batchopb"_n;

   create_t5_holding_accounts();
   create_accounts({ BATCH_OP_A, BATCH_OP_B }, false, false, false, true);
   BOOST_REQUIRE_EQUAL( success(),
      register_operator(BATCH_OP_A, OperatorType::OPERATOR_TYPE_BATCH, /*is_bootstrapped*/true) );
   BOOST_REQUIRE_EQUAL( success(),
      register_operator(BATCH_OP_B, OperatorType::OPERATOR_TYPE_BATCH, /*is_bootstrapped*/true) );

   // Two one-member groups rotate on every advance. The scheduler chooses the
   // front group only after it has shifted the window, which is exactly the
   // positional-identity loss this regression covers.
   BOOST_REQUIRE_EQUAL( success(), init_epoch_state(60, /*operators_per_epoch*/1,
                                                    /*batch_op_groups_count*/2) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( success(), push_epoch_action(EPOCH, "schbatchgps"_n, mvo()) );

   BOOST_REQUIRE_EQUAL( success(),
      setemitcfg_with_cadence(config::system_account_name, uint16_t(2)) );
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5(config::system_account_name, tpsec(start)) );

   // Epoch 1 is the shortened genesis pay period. Claim it before comparing
   // the two full cadence-two periods below.
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() );
   const int64_t a_before = get_wire_balance_paid(BATCH_OP_A).get_amount();
   const int64_t b_before = get_wire_balance_paid(BATCH_OP_B).get_amount();

   // Epochs 2 and 3 are the first full period; each roster is active once.
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() ); // epoch 2, non-pay
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() ); // epoch 3, pay
   const int64_t a_first = get_wire_balance_paid(BATCH_OP_A).get_amount() - a_before;
   const int64_t b_first = get_wire_balance_paid(BATCH_OP_B).get_amount() - b_before;
   BOOST_REQUIRE_GT(a_first, 0);
   BOOST_REQUIRE_GT(b_first, 0);
   BOOST_REQUIRE_EQUAL(a_first, b_first);

   // A second period proves payepoch cleared the consumed history. If rows
   // from epochs 2-3 survived, the current period's history is malformed and
   // the compatibility fallback would again pay only the boundary roster.
   const int64_t a_after_first = get_wire_balance(BATCH_OP_A).get_amount();
   const int64_t b_after_first = get_wire_balance(BATCH_OP_B).get_amount();
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() ); // epoch 4, non-pay
   produce_blocks(130);
   BOOST_REQUIRE_EQUAL( success(), advance_epoch_state() ); // epoch 5, pay
   const int64_t a_second = get_wire_balance_paid(BATCH_OP_A).get_amount() - a_after_first;
   const int64_t b_second = get_wire_balance_paid(BATCH_OP_B).get_amount() - b_after_first;
   BOOST_REQUIRE_GT(a_second, 0);
   BOOST_REQUIRE_GT(b_second, 0);
   BOOST_REQUIRE_EQUAL(a_second, b_second);
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
      ("compute_bps", COMPUTE_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", uint16_t(1000))
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
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

// ---------------------------------------------------------------------------
// fundclaim: per-onreward immediate funding for sysio.dclaim
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( fundclaim_requires_dclaim_auth, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   deploy_dclaim_for_signing();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // alice has no claim to sysio.dclaim's authority.
   create_user_accounts({ "alice"_n });
   auto r = fundclaim( "alice"_n, int64_t(1'000'000) );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio.dclaim" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( fundclaim_transfers_and_tracks_distributed, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   deploy_dclaim_for_signing();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const asset sysio_before  = get_wire_balance_paid( config::system_account_name );
   const asset dclaim_before = get_wire_balance_paid( "sysio.dclaim"_n );
   const int64_t distributed_before = get_t5_state()["total_distributed"].as<int64_t>();

   const int64_t amt = int64_t(50'000'000'000);   // 50 WIRE
   BOOST_REQUIRE_EQUAL( success(), fundclaim( "sysio.dclaim"_n, amt ) );

   const asset sysio_after  = get_wire_balance_paid( config::system_account_name );
   const asset dclaim_after = get_wire_balance_paid( "sysio.dclaim"_n );
   const auto state = get_t5_state();

   BOOST_REQUIRE_EQUAL( sysio_before.get_amount()  - sysio_after.get_amount(),  amt );
   BOOST_REQUIRE_EQUAL( dclaim_after.get_amount() - dclaim_before.get_amount(), amt );
   BOOST_REQUIRE_EQUAL( state["total_distributed"].as<int64_t>(), distributed_before + amt );
   BOOST_REQUIRE_EQUAL( state["capital_shortfall_total"].as<int64_t>(), 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( fundclaim_no_op_for_zero_or_negative, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   deploy_dclaim_for_signing();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   const asset dclaim_before = get_wire_balance_paid( "sysio.dclaim"_n );
   const int64_t distributed_before = get_t5_state()["total_distributed"].as<int64_t>();

   BOOST_REQUIRE_EQUAL( success(), fundclaim( "sysio.dclaim"_n, int64_t(0) ) );
   BOOST_REQUIRE_EQUAL( success(), fundclaim( "sysio.dclaim"_n, int64_t(-100) ) );

   BOOST_REQUIRE_EQUAL( get_wire_balance_paid( "sysio.dclaim"_n ).get_amount(),
                        dclaim_before.get_amount() );
   BOOST_REQUIRE_EQUAL( get_t5_state()["total_distributed"].as<int64_t>(),
                        distributed_before );
   BOOST_REQUIRE_EQUAL( get_t5_state()["capital_shortfall_total"].as<int64_t>(), 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( fundclaim_caps_to_remaining_pool_and_records_shortfall, sysio_emissions_tester ) try {
   // Squeeze t5_distributable down to a small headroom over t5_floor so the
   // next fundclaim has a hard cap. Verify the partial transfer and the
   // shortfall accumulator.
   create_t5_holding_accounts();
   deploy_dclaim_for_signing();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Shrink drainable headroom to exactly 1000 subunits via a fresh emitcfg.
   // The post-init guard requires t5_distributable >= t5_floor + total_distributed,
   // so set t5_floor to 0 and t5_distributable just above current total_distributed.
   const int64_t already = get_t5_state()["total_distributed"].as<int64_t>();
   const int64_t headroom = 1000;
   auto cfg = mvo()
      ("t1_allocation", T1_ALLOCATION.get_amount())
      ("t2_allocation", T2_ALLOCATION.get_amount())
      ("t3_allocation", T3_ALLOCATION.get_amount())
      ("t1_duration", T1_DURATION) ("t2_duration", T2_DURATION) ("t3_duration", T3_DURATION)
      ("min_claimable", MIN_CLAIMABLE_AMOUNT)
      ("t5_distributable", already + headroom)
      ("t5_floor", int64_t(0))
      ("target_annual_decay_bps", TARGET_ANNUAL_DECAY_BPS)
      ("annual_initial_emission", ANNUAL_INITIAL_EMISSION)
      ("annual_max_emission", ANNUAL_MAX_EMISSION)
      // annual_min_emission=0 so the post-init guard (per_epoch_min <= remaining)
      // doesn't reject the deliberately-tiny pool used by this test.
      ("annual_min_emission", int64_t(0))
      ("compute_bps", COMPUTE_BPS)
      ("capex_bps", CAPEX_BPS) ("governance_bps", GOV_BPS)
      ("producer_bps", PRODUCER_BPS) ("batch_op_bps", uint16_t(3000))
      ("standby_end_rank", T_STANDBY_END_RANK)("standby_bps", T_STANDBY_BPS)
      ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));
   BOOST_REQUIRE_EQUAL( success(), setemitcfg( config::system_account_name, cfg ) );

   const asset dclaim_before = get_wire_balance_paid( "sysio.dclaim"_n );

   // Request 3x the headroom; expect partial transfer of `headroom`, shortfall = 2x.
   const int64_t request   = headroom * 3;
   const int64_t shortfall = request - headroom;
   BOOST_REQUIRE_EQUAL( success(), fundclaim( "sysio.dclaim"_n, request ) );

   const asset dclaim_after = get_wire_balance_paid( "sysio.dclaim"_n );
   const auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( dclaim_after.get_amount() - dclaim_before.get_amount(), headroom );
   BOOST_REQUIRE_EQUAL( state["total_distributed"].as<int64_t>(), already + headroom );
   BOOST_REQUIRE_EQUAL( state["capital_shortfall_total"].as<int64_t>(), shortfall );

   // A further request after pool is exhausted is a full shortfall.
   BOOST_REQUIRE_EQUAL( success(), fundclaim( "sysio.dclaim"_n, int64_t(500) ) );
   const auto state2 = get_t5_state();
   BOOST_REQUIRE_EQUAL( get_wire_balance_paid( "sysio.dclaim"_n ).get_amount(),
                        dclaim_after.get_amount() );
   BOOST_REQUIRE_EQUAL( state2["total_distributed"].as<int64_t>(), already + headroom );
   BOOST_REQUIRE_EQUAL( state2["capital_shortfall_total"].as<int64_t>(), shortfall + 500 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( fundclaim_silent_when_t5state_missing, sysio_emissions_tester ) try {
   // initt5 NOT called, so t5state.exists() is false. fundclaim must absorb
   // the call without throwing or mutating any state.
   create_t5_holding_accounts();
   deploy_dclaim_for_signing();
   const asset dclaim_before = get_wire_balance_paid( "sysio.dclaim"_n );

   BOOST_REQUIRE_EQUAL( success(), fundclaim( "sysio.dclaim"_n, int64_t(1'000'000) ) );

   BOOST_REQUIRE_EQUAL( get_wire_balance_paid( "sysio.dclaim"_n ).get_amount(),
                        dclaim_before.get_amount() );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END() // t5_emissions_tests

// ===========================================================================
// Producer scheduling eligibility
//
// update_ranked_producers must schedule only producers that are ACTIVE
// OPERATOR_TYPE_PRODUCER operators in sysio.opreg, backfill vacated active slots
// from standbys, and never shrink the schedule (with its lock-step finalizer
// policy) below the BFT safety floor (min_schedule_size). The gate treats every
// non-ACTIVE status identically (UNKNOWN from a collateral withdrawal, SLASHED,
// TERMINATED all fail is_op_active), so slash / terminate exercise the same
// decision the withdrawal path reaches once flushwtdw flips status to UNKNOWN
// (that transition itself is covered by the opreg / epoch suites).
//
// Scheduling is asserted through the last-proposed finalizer policy, which
// update_ranked_producers writes in the same loop as the producer schedule, so
// its key_ids are exactly the scheduled producers -- and reading it does not
// depend on the (unsignable, test-key) policy ever activating.
// ===========================================================================

struct producer_eligibility_tester : public sysio_emissions_tester {

   /// producera, producerb, ... for the first `count` indices.
   std::vector<account_name> producer_names(uint32_t count) {
      std::vector<account_name> names;
      names.reserve(count);
      for (uint32_t i = 0; i < count; ++i) names.push_back(producer_name_at(i));
      return names;
   }

   /// Create producer accounts with enough RAM to store a finalizer key. Uses the
   /// system setacctram action (a direct native limit set) rather than the ROA /
   /// RAM market, which this fixture does not activate.
   void create_producer_accounts(const std::vector<account_name>& names) {
      create_accounts(names, false, false, false, true);
      produce_blocks(1);
      for (const auto& p : names) {
         BOOST_REQUIRE_EQUAL(success(), push_system_action(config::system_account_name, "setacctram"_n,
            mvo()("account", p)("ram_bytes", int64_t(1'000'000))));
      }
      produce_blocks(1);
   }

   action_result terminate_operator(account_name account, const std::string& reason = "test terminate") {
      return push_opreg_action(OPREG, "terminate"_n, mvo()("account", account)("reason", reason));
   }

   /// Registers `count` producers, each an ACTIVE bootstrapped PRODUCER operator
   /// with an active finalizer key.
   ///
   /// Rank is POSITION in the score-ordered "prodrank" index, not a stored ordinal, so nothing
   /// assigns it here. Every producer registered by this fixture is bootstrapped and holds no
   /// collateral, so they all land in the bootstrapped tier with an identical composite score --
   /// and equal keys fall back to primary-key order, which is account-name order. `producer_name_at`
   /// yields ascending names, so positions 1..count follow the index order the caller expects.
   /// The producer/finalizer rows are populated but no schedule is published
   /// until the caller triggers update_ranked_producers via trigger_reschedule().
   std::vector<account_name> setup_ranked_producers(uint32_t count) {
      auto names = producer_names(count);
      create_producer_accounts(names);
      for (auto& p : names) {
         BOOST_REQUIRE_EQUAL(success(), push_system_action(p, "regproducer"_n, mvo()
            ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)));
      }
      for (auto& p : names) {
         BOOST_REQUIRE_EQUAL(success(), register_operator(p, OperatorType::OPERATOR_TYPE_PRODUCER, true));
      }
      register_finalizer_keys(names, count);
      produce_blocks(1);
      return names;
   }

   /// Fire onblock's update_ranked_producers rebuild (throttled to once per 120
   /// slots). Advance 130 blocks (~65s) so the threshold is crossed gradually --
   /// a single large time jump would expire subsequently pushed transactions.
   void trigger_reschedule() {
      produce_blocks(130);
   }

   /// True iff `producer`'s active finalizer key appears in the finalizer policy
   /// that update_ranked_producers last proposed -- i.e. the producer is scheduled.
   /// Finalizer key_ids are zero-based, so row presence (not a non-zero id) is the
   /// "producer has an active key" test.
   bool is_scheduled(account_name producer) {
      auto data = get_row_by_account(config::system_account_name, config::system_account_name,
                                     "finalizers"_n, producer);
      if (data.empty()) return false;
      auto v = sysio_abi_ser.binary_to_variant("finalizer_info", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      auto ids = sysio_test::get_last_prop_fin_ids(*this, sysio_abi_ser);
      return ids.count(v["active_key_id"].as_uint64()) > 0;
   }
};

BOOST_AUTO_TEST_SUITE(sysio_producer_eligibility_tests)

// Baseline: collateralized, finalizer-keyed producers are all scheduled.
BOOST_FIXTURE_TEST_CASE( active_producers_scheduled, producer_eligibility_tester ) try {
   auto names = setup_ranked_producers(5);
   trigger_reschedule();

   for (const auto& p : names) {
      BOOST_REQUIRE_MESSAGE(is_scheduled(p), "expected " << p.to_string() << " scheduled");
   }
} FC_LOG_AND_RETHROW()

// A slashed producer (status SLASHED) is dropped from the schedule; the others
// remain (4 eligible >= min_schedule_size).
BOOST_FIXTURE_TEST_CASE( slashed_producer_removed, producer_eligibility_tester ) try {
   auto names = setup_ranked_producers(5);
   trigger_reschedule();
   BOOST_REQUIRE( is_scheduled(names[4]) );

   BOOST_REQUIRE_EQUAL( success(), slash_operator(names[4]) );
   trigger_reschedule();

   BOOST_REQUIRE( !is_scheduled(names[4]) );
   BOOST_REQUIRE(  is_scheduled(names[0]) );
   BOOST_REQUIRE(  is_scheduled(names[3]) );
} FC_LOG_AND_RETHROW()

// A terminated producer (status TERMINATED) is dropped from the schedule.
BOOST_FIXTURE_TEST_CASE( terminated_producer_removed, producer_eligibility_tester ) try {
   auto names = setup_ranked_producers(5);
   trigger_reschedule();
   BOOST_REQUIRE( is_scheduled(names[2]) );

   BOOST_REQUIRE_EQUAL( success(), terminate_operator(names[2]) );
   trigger_reschedule();

   BOOST_REQUIRE( !is_scheduled(names[2]) );
   BOOST_REQUIRE(  is_scheduled(names[0]) );
} FC_LOG_AND_RETHROW()

// Re-collateralization (register a fresh ACTIVE producer operator) restores
// schedulability: an account whose operator row is absent is not scheduled, and
// once it becomes an ACTIVE PRODUCER operator it is picked up on the next rebuild.
BOOST_FIXTURE_TEST_CASE( noncollateralized_producer_not_scheduled_then_restored, producer_eligibility_tester ) try {
   // Four collateralized producers plus one that is registered + finalizer-keyed
   // + ranked but has NO opreg operator row yet.
   auto names = producer_names(5);
   create_producer_accounts(names);
   for (auto& p : names) {
      BOOST_REQUIRE_EQUAL( success(), push_system_action(p, "regproducer"_n, mvo()
         ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)) );
   }
   for (uint32_t i = 0; i < 4; ++i) {
      BOOST_REQUIRE_EQUAL( success(), register_operator(names[i], OperatorType::OPERATOR_TYPE_PRODUCER, true) );
   }
   register_finalizer_keys(names, 5);
   produce_blocks(1);
   trigger_reschedule();

   BOOST_REQUIRE(  is_scheduled(names[0]) );
   BOOST_REQUIRE( !is_scheduled(names[4]) );   // no operator row -> not scheduled

   BOOST_REQUIRE_EQUAL( success(), register_operator(names[4], OperatorType::OPERATOR_TYPE_PRODUCER, true) );
   trigger_reschedule();

   BOOST_REQUIRE( is_scheduled(names[4]) );     // now ACTIVE PRODUCER -> scheduled
} FC_LOG_AND_RETHROW()

// An account that is ACTIVE only as a BATCH operator (different collateral) must
// not be scheduled as a producer even though its opreg status is ACTIVE.
BOOST_FIXTURE_TEST_CASE( active_batch_operator_not_scheduled_as_producer, producer_eligibility_tester ) try {
   auto names = producer_names(5);
   create_producer_accounts(names);
   for (auto& p : names) {
      BOOST_REQUIRE_EQUAL( success(), push_system_action(p, "regproducer"_n, mvo()
         ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)) );
   }
   for (uint32_t i = 0; i < 4; ++i) {
      BOOST_REQUIRE_EQUAL( success(), register_operator(names[i], OperatorType::OPERATOR_TYPE_PRODUCER, true) );
   }
   // names[4] is ACTIVE, but as a BATCH operator -- wrong type for producing.
   BOOST_REQUIRE_EQUAL( success(), register_operator(names[4], OperatorType::OPERATOR_TYPE_BATCH, true) );
   register_finalizer_keys(names, 5);
   produce_blocks(1);
   trigger_reschedule();

   BOOST_REQUIRE(  is_scheduled(names[0]) );
   BOOST_REQUIRE( !is_scheduled(names[4]) );
} FC_LOG_AND_RETHROW()

// When an active-rank producer becomes ineligible and a standby is available,
// the standby backfills the vacated slot (schedule stays full).
BOOST_FIXTURE_TEST_CASE( standby_backfills_ineligible_active, producer_eligibility_tester ) try {
   // emitcfg must exist for standby_end_rank (else backfill is disabled and the
   // schedule caps at max_producers with no standbys considered).
   BOOST_REQUIRE_EQUAL( success(), setemitcfg_defaults( config::system_account_name ) );
   produce_blocks(1);

   // 21 active-rank (1..21) + one standby at rank 22.
   auto names = setup_ranked_producers(22);
   trigger_reschedule();

   BOOST_REQUIRE(  is_scheduled(names[0])  );   // rank 1 active
   BOOST_REQUIRE( !is_scheduled(names[21]) );   // rank 22 standby: not scheduled while actives are full

   BOOST_REQUIRE_EQUAL( success(), slash_operator(names[0]) );
   trigger_reschedule();

   BOOST_REQUIRE( !is_scheduled(names[0])  );   // slashed active removed
   BOOST_REQUIRE(  is_scheduled(names[21]) );   // standby backfilled into the schedule
} FC_LOG_AND_RETHROW()

// The schedule (and its lock-step finalizer policy) never shrinks below the BFT
// safety floor: with min_schedule_size producers and no standbys, making one
// ineligible leaves fewer than the floor eligible, so the last good schedule is
// retained rather than published smaller -- the ineligible producer stays.
BOOST_FIXTURE_TEST_CASE( schedule_not_shrunk_below_floor, producer_eligibility_tester ) try {
   auto names = setup_ranked_producers(4);      // min_schedule_size == 4
   trigger_reschedule();
   for (const auto& p : names) BOOST_REQUIRE( is_scheduled(p) );

   BOOST_REQUIRE_EQUAL( success(), slash_operator(names[3]) );
   trigger_reschedule();

   // Only 3 eligible (< floor). update_ranked_producers returns early, so the
   // last good 4-member schedule -- including the slashed producer -- is kept.
   BOOST_REQUIRE( is_scheduled(names[3]) );
   BOOST_REQUIRE( is_scheduled(names[0]) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END() // sysio_producer_eligibility_tests

// ===========================================================================
// Producer SCORE, tiers and demotion (sysio_producer_score_tests)
//
// The suite above asserts WHO is schedulable. This one asserts the ORDER they
// are schedulable in, and the demotion model that can take a producer out of
// the schedule with no governance action at all.
//
// Nothing here reads a `rank` field, because none exists: rank is POSITION in
// the "prodrank" index among schedulable producers. What IS stored is
// `rank_score`, the packed key that index sorts on --
// `tier << 62 | (composite_max - composite)`. Two consequences drive every
// assertion below: a HIGHER composite is a NUMERICALLY LOWER key, and a higher
// tier outweighs any composite whatsoever, which is what makes the uncapped
// collateral term safe.
//
// A collateral-backed producer needs an opreg `opconfig` row to exist at all --
// with none, `req_prod_collat` reads empty and `meets_role_min` refuses every
// non-bootstrapped operator by design (SEC-22). That is why the eligibility
// fixture above registers everything bootstrapped, and why this fixture's
// collateral helpers install a config first.
// ===========================================================================

struct producer_score_tester : public producer_eligibility_tester {

   /// Packed-key tier values, mirroring `producer_tier` in producer_rank.hpp.
   static constexpr uint64_t tier_healthy      = 0;
   static constexpr uint64_t tier_bootstrapped = 1;
   static constexpr uint64_t tier_demoted      = 2;

   /// Bits the packed key gives the composite; the tier occupies the two above them.
   static constexpr unsigned composite_bits = 62;

   /// Slots one producer holds before the round-robin rotates (config::producer_repetitions).
   static constexpr uint32_t slots_per_producer = 12;

   /// Chain/token pair the collateral helpers use. Any pair works -- opreg stores slug names
   /// opaquely -- so these name a plausible outpost rather than carrying meaning.
   static constexpr std::string_view collateral_chain = "ETH";
   static constexpr std::string_view collateral_token = "ETH";

   /// A second pair, for the "minimum across pairs" case.
   static constexpr std::string_view second_chain = "SOL";
   static constexpr std::string_view second_token = "SOL";

   /// The minimum bond every collateral test measures its ratios against.
   static constexpr uint64_t base_min_bond = 1'000'000;

   /// The tier packed into a `rank_score`, mirroring `producer_rank::tier_of`.
   static uint64_t tier_of(uint64_t rank_score) { return rank_score >> composite_bits; }

   /// A `slug_name` in the shape the ABI serializes it: a single `value` field.
   static fc::mutable_variant_object slug_mvo(std::string_view code) {
      return mvo()("value", fc::slug_name{code}.value);
   }

   /// One `(chain, token, min_bond)` entry for opreg's `req_*_collat` vectors. The
   /// `config_timestamp_ms` supplied here is ignored -- `setconfig` overwrites it with on-chain
   /// time so consumers never trust the caller's clock.
   static fc::variant min_bond_mvo(std::string_view chain, std::string_view token, uint64_t min_bond) {
      return fc::variant(mvo()
         ("chain_code",          slug_mvo(chain))
         ("token_code",          slug_mvo(token))
         ("min_bond",            min_bond)
         ("config_timestamp_ms", uint64_t{0}));
   }

   /// Install an opreg configuration carrying `req_prod_collat`.
   ///
   /// @param req_prod_collat producer collateral requirement, built from `min_bond_mvo`.
   /// @return the action result.
   action_result set_producer_collateral(const fc::variants& req_prod_collat) {
      return push_opreg_action(OPREG, "setconfig"_n, mvo()
         ("max_available_producers",          uint32_t{21})
         ("max_available_batch_ops",          uint32_t{63})
         ("max_available_underwriters",       uint32_t{21})
         ("terminate_prune_delay_ms",         uint64_t{600'000})
         ("terminate_max_consecutive_misses", uint32_t{5})
         ("terminate_max_pct_misses_24h",     uint32_t{5})
         ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
         ("req_prod_collat",                  req_prod_collat)
         ("req_batchop_collat",               fc::variants{})
         ("req_uw_collat",                    fc::variants{}));
   }

   /// The single-pair requirement most tests use.
   action_result set_single_pair_collateral(uint64_t min_bond = base_min_bond) {
      return set_producer_collateral(fc::variants{
         min_bond_mvo(collateral_chain, collateral_token, min_bond)});
   }

   /// Credit an outpost-side collateral row the way `sysio.msgch` does when it dispatches an
   /// inbound DEPOSIT_REQUEST. Signing as sysio.opreg satisfies `depositinle`'s
   /// `require_auth(get_self())`.
   ///
   /// This is the seam the score hangs off: a credit runs `reevaluate_eligibility`, which
   /// dispatches `processprod` for producers on EVERY balance change, whose notification
   /// sysio.system turns into a rescore.
   action_result credit_collateral(account_name account, uint64_t amount,
                                   std::string_view chain = collateral_chain,
                                   std::string_view token = collateral_token) {
      return push_opreg_action(OPREG, "depositinle"_n, mvo()
         ("account",             account)
         ("chain_code",          slug_mvo(chain))
         ("token_code",          slug_mvo(token))
         ("amount",              amount)
         ("actor_chain",         ChainKind::CHAIN_KIND_EVM)
         ("actor_address",       std::vector<char>(20, '\x06'))
         ("original_message_id", fc::sha256()));
   }

   /// Push `setscorecfg`. Defaults mirror the contract's own so a test names only the weight it
   /// is exercising.
   action_result set_score_config(uint32_t collateral_weight    = 10'000,
                                  uint32_t participation_weight = 10'000,
                                  uint32_t snapshot_weight      = 10'000,
                                  uint32_t max_consecutive_missed_rounds = 3,
                                  uint32_t snapshot_target_attestations  = 1) {
      return push_system_action(config::system_account_name, "setscorecfg"_n, mvo()
         ("weights", mvo()
            ("collateral_weight",             collateral_weight)
            ("participation_weight",          participation_weight)
            ("snapshot_weight",               snapshot_weight)
            ("relay_weight",                  uint32_t{0})
            ("api_weight",                    uint32_t{0})
            ("benchmark_weight",              uint32_t{0})
            ("max_consecutive_missed_rounds", max_consecutive_missed_rounds)
            ("snapshot_target_attestations",  snapshot_target_attestations)));
   }

   /// The packed sort key stored on a producer.
   uint64_t rank_score_of(account_name producer) {
      auto info = get_producer_info(producer);
      BOOST_REQUIRE_MESSAGE(!info.is_null(), "no producers row for " << producer.to_string());
      return info["rank_score"].as<uint64_t>();
   }

   uint32_t missed_rounds_of(account_name producer) {
      auto info = get_producer_info(producer);
      BOOST_REQUIRE_MESSAGE(!info.is_null(), "no producers row for " << producer.to_string());
      return info["consecutive_missed_rounds"].as<uint32_t>();
   }

   bool demoted(account_name producer) {
      auto info = get_producer_info(producer);
      BOOST_REQUIRE_MESSAGE(!info.is_null(), "no producers row for " << producer.to_string());
      return info["is_demoted"].as<bool>();
   }

   /// The sysio.system global singleton, which carries the rescore cursor.
   fc::variant get_global_state() {
      auto data = get_row_by_account(config::system_account_name, config::system_account_name,
                                     "global"_n, "global"_n);
      if (data.empty()) return fc::variant();
      return sysio_abi_ser.binary_to_variant("sysio_global_state", data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   bool rescore_pending() {
      auto g = get_global_state();
      BOOST_REQUIRE(!g.is_null());
      return g["rescore_pending"].as<bool>();
   }

   uint32_t unpaid_blocks_of(account_name producer) {
      auto info = get_producer_info(producer);
      BOOST_REQUIRE_MESSAGE(!info.is_null(), "no producers row for " << producer.to_string());
      return info["unpaid_blocks"].as<uint32_t>();
   }

   /// Register `count` producers as NON-bootstrapped operators, each bonded at `deposit` on the
   /// single required pair, with an active finalizer key.
   ///
   /// Order is load-bearing: `regproducer` must precede the deposit, because a rescore is a no-op
   /// while no producers row exists, and the deposit's `processprod` notification is what writes
   /// the first real score.
   ///
   /// @param count   how many producers to create, from the fixture's roster.
   /// @param deposit the bond credited to each, in the same units as the configured minimum.
   /// @return the producer names, in roster (and therefore name) order.
   std::vector<account_name> setup_collateralized_producers(uint32_t count,
                                                            uint64_t deposit = base_min_bond) {
      BOOST_REQUIRE_EQUAL(success(), set_single_pair_collateral());
      produce_blocks(1);

      auto names = producer_names(count);
      create_producer_accounts(names);
      for (auto& p : names) {
         BOOST_REQUIRE_EQUAL(success(), push_system_action(p, "regproducer"_n, mvo()
            ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)));
      }
      for (auto& p : names) {
         BOOST_REQUIRE_EQUAL(success(), register_operator(p, OperatorType::OPERATOR_TYPE_PRODUCER, false));
      }
      for (auto& p : names) {
         BOOST_REQUIRE_EQUAL(success(), credit_collateral(p, deposit));
      }
      produce_blocks(1);
      for (auto& p : names) {
         auto op = get_opreg_operator(p);
         BOOST_REQUIRE_MESSAGE(!op.is_null(), "no opreg row for " << p.to_string());
         BOOST_REQUIRE_EQUAL("OPERATOR_STATUS_ACTIVE", op["status"].as_string());
      }
      register_finalizer_keys(names, count);
      produce_blocks(1);
      return names;
   }

   /// The active producer schedule, in schedule order.
   std::vector<account_name> active_schedule_names() {
      std::vector<account_name> names;
      for (const auto& p : control->active_producers().producers) {
         names.push_back(p.producer_name);
      }
      return names;
   }

   /// Produce until `expected` is in the ACTIVE schedule -- not merely proposed.
   ///
   /// Miss attribution reads the live schedule, so these tests need the proposal to have gone
   /// final and activated, which the eligibility suite's `is_scheduled` deliberately does not
   /// wait for.
   void wait_for_active_schedule(account_name expected, uint32_t max_blocks = 400) {
      for (uint32_t produced = 0; produced < max_blocks; ++produced) {
         const auto schedule = active_schedule_names();
         if (std::find(schedule.begin(), schedule.end(), expected) != schedule.end()) return;
         produce_blocks(1);
      }
      BOOST_FAIL("producer " << expected.to_string() << " never entered the active schedule");
   }

   /// Skip `target`'s entire slot window so it produces nothing and is charged a missed round.
   ///
   /// A tester produces every scheduled block, so a miss has to be manufactured: advance one
   /// window at a time until the producer immediately BEFORE the target holds the head block,
   /// then jump the rest of that window plus the target's whole window in one step. The next
   /// block therefore belongs to the producer AFTER the target, and the contract's walk from the
   /// previous producer to this one finds exactly the target in between.
   ///
   /// @param target the producer whose round should go unproduced.
   void skip_round_of(account_name target) {
      const auto schedule = active_schedule_names();
      BOOST_REQUIRE_MESSAGE(schedule.size() >= 3,
         "skipping a round needs at least three scheduled producers");

      const auto target_it = std::find(schedule.begin(), schedule.end(), target);
      BOOST_REQUIRE_MESSAGE(target_it != schedule.end(),
         target.to_string() << " is not in the active schedule");
      const size_t target_index = static_cast<size_t>(std::distance(schedule.begin(), target_it));
      const size_t before_index = (target_index + schedule.size() - 1) % schedule.size();

      // Producer for a slot, exactly as the chain assigns it.
      const auto index_at = [&](uint32_t slot) {
         return (slot % (schedule.size() * slots_per_producer)) / slots_per_producer;
      };

      // Advance to the window immediately before the target's. Bounded by one full rotation plus
      // a window, so a schedule change mid-walk fails loudly rather than spinning.
      const uint32_t walk_limit = static_cast<uint32_t>(schedule.size() + 1) * slots_per_producer;
      uint32_t walked = 0;
      while (index_at(control->head().header().timestamp.slot) != before_index) {
         produce_blocks(1);
         BOOST_REQUIRE_MESSAGE(++walked < walk_limit,
            "never reached the window before " << target.to_string());
      }

      const uint32_t slot        = control->head().header().timestamp.slot;
      const uint32_t into_window = slot % slots_per_producer;
      const uint32_t jump        = (slots_per_producer - into_window) + slots_per_producer;
      produce_block(fc::milliseconds(int64_t(config::block_interval_ms) * jump));

      BOOST_REQUIRE_MESSAGE(control->head().header().producer != target,
         "the jump landed on " << target.to_string() << " instead of skipping it");
   }
};

BOOST_AUTO_TEST_SUITE(sysio_producer_score_tests)

// ---------------------------------------------------------------------------
// Composite score
// ---------------------------------------------------------------------------

// Collateral is linear and uncapped, so a top-up strictly improves the score and moves the
// producer up the index -- the "producers compete for rank by posting more" property. It also
// covers the top-up seam: a deposit while already ACTIVE changes no status, and only reaches
// sysio.system because producers dispatch `processprod` on every balance change.
BOOST_FIXTURE_TEST_CASE( collateral_topup_raises_score_and_position, producer_score_tester ) try {
   auto names = setup_collateralized_producers(5);

   // Equal bonds, so the composite is equal and the index falls through to account-name order.
   const uint64_t before = rank_score_of(names[4]);
   BOOST_REQUIRE_EQUAL( before, rank_score_of(names[0]) );
   BOOST_REQUIRE_EQUAL( 5u, producer_rank_position(names[4]) );

   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[4], base_min_bond * 4) );
   produce_blocks(1);

   // A higher composite is a numerically LOWER key, and the last-by-name producer is now first.
   BOOST_REQUIRE_LT( rank_score_of(names[4]), before );
   BOOST_REQUIRE_EQUAL( 1u, producer_rank_position(names[4]) );
   BOOST_REQUIRE_EQUAL( 2u, producer_rank_position(names[0]) );
} FC_LOG_AND_RETHROW()

// The collateral factor is the MINIMUM across the required pairs, with no sum term: posting extra
// on the cheapest chain must do nothing at all, so raising the score requires lifting EVERY pair.
BOOST_FIXTURE_TEST_CASE( collateral_is_minimum_across_required_pairs, producer_score_tester ) try {
   BOOST_REQUIRE_EQUAL( success(), set_producer_collateral(fc::variants{
      min_bond_mvo(collateral_chain, collateral_token, base_min_bond),
      min_bond_mvo(second_chain,     second_token,     base_min_bond)}) );
   produce_blocks(1);

   auto names = producer_names(2);
   create_producer_accounts(names);
   for (auto& p : names) {
      BOOST_REQUIRE_EQUAL( success(), push_system_action(p, "regproducer"_n, mvo()
         ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)) );
      BOOST_REQUIRE_EQUAL( success(), register_operator(p, OperatorType::OPERATOR_TYPE_PRODUCER, false) );
      BOOST_REQUIRE_EQUAL( success(), credit_collateral(p, base_min_bond) );
      BOOST_REQUIRE_EQUAL( success(), credit_collateral(p, base_min_bond, second_chain, second_token) );
   }
   register_finalizer_keys(names, 2);
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( rank_score_of(names[0]), rank_score_of(names[1]) );

   // Ten times the bond on ONE pair leaves the minimum -- and so the score -- untouched.
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[1], base_min_bond * 9) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( rank_score_of(names[0]), rank_score_of(names[1]) );

   // Lifting the OTHER pair moves the minimum, and only then does the score improve.
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[1], base_min_bond, second_chain, second_token) );
   produce_blocks(1);
   BOOST_REQUIRE_LT( rank_score_of(names[1]), rank_score_of(names[0]) );
} FC_LOG_AND_RETHROW()

// A weight of zero removes its factor's influence entirely -- the property that lets a new factor
// ship at weight 0 without disturbing any existing ordering.
BOOST_FIXTURE_TEST_CASE( zero_weight_removes_factor_influence, producer_score_tester ) try {
   auto names = setup_collateralized_producers(3);
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[2], base_min_bond * 20) );
   produce_blocks(1);
   BOOST_REQUIRE_LT( rank_score_of(names[2]), rank_score_of(names[0]) );

   BOOST_REQUIRE_EQUAL( success(), set_score_config(/*collateral_weight=*/0) );
   trigger_reschedule();

   // Twenty times the bond now buys nothing: every producer carries the same composite.
   BOOST_REQUIRE_EQUAL( rank_score_of(names[0]), rank_score_of(names[2]) );
   BOOST_REQUIRE_EQUAL( rank_score_of(names[1]), rank_score_of(names[2]) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Rescore sweep
// ---------------------------------------------------------------------------

// A weight change invalidates every stored score at once, and `producers` is unbounded because
// `regproducer` is permissionless -- so the rewrite is a cursor `onblock` drains a bounded slice
// of per schedule-rebuild tick. With more producers than one slice holds, the sweep must survive
// across ticks: still in progress after the first, finished after the second.
BOOST_FIXTURE_TEST_CASE( rescore_sweep_drains_across_ticks, producer_score_tester ) try {
   constexpr uint32_t max_rescore_per_tick = 32;
   constexpr uint32_t producer_count       = max_rescore_per_tick + 2;

   auto names = setup_ranked_producers(producer_count);
   trigger_reschedule();
   BOOST_REQUIRE( !rescore_pending() );

   BOOST_REQUIRE_EQUAL( success(), set_score_config(/*collateral_weight=*/5'000) );
   BOOST_REQUIRE( rescore_pending() );

   trigger_reschedule();
   BOOST_REQUIRE_MESSAGE( rescore_pending(),
      "a sweep of " << producer_count << " rows must not finish in one "
      << max_rescore_per_tick << "-row tick" );

   trigger_reschedule();
   BOOST_REQUIRE( !rescore_pending() );
} FC_LOG_AND_RETHROW()

// The collateral minimums live on sysio.opreg, whose `setconfig` notifies sysio.system on the
// same channel `processprod` uses. The notification opens the sweep at once -- no throttle tick
// has to notice a stamp -- so two changes inside one second cannot lose the second one.
BOOST_FIXTURE_TEST_CASE( collateral_minimum_change_opens_rescore_sweep, producer_score_tester ) try {
   auto names = setup_collateralized_producers(3);
   trigger_reschedule();
   BOOST_REQUIRE( !rescore_pending() );
   const uint64_t before = rank_score_of(names[0]);

   // Halving the minimum doubles every ratio, so every stored score is now wrong -- and the
   // sweep is pending the moment setconfig lands, before any tick.
   BOOST_REQUIRE_EQUAL( success(), set_single_pair_collateral(base_min_bond / 2) );
   BOOST_REQUIRE( rescore_pending() );
   trigger_reschedule();

   BOOST_REQUIRE( !rescore_pending() );   // three rows drain in one tick
   const uint64_t halved = rank_score_of(names[0]);
   BOOST_REQUIRE_LT( halved, before );

   // Every change opens a sweep, the second as surely as the first; the drain scores against the
   // config that is live when it runs.
   BOOST_REQUIRE_EQUAL( success(), set_single_pair_collateral(base_min_bond / 4) );
   BOOST_REQUIRE( rescore_pending() );
   trigger_reschedule();
   BOOST_REQUIRE( !rescore_pending() );
   BOOST_REQUIRE_LT( rank_score_of(names[0]), halved );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Tiers
// ---------------------------------------------------------------------------

// A bootstrap is the foundation-run backstop, so every healthy producer outranks it -- and does so
// on the TIER, not on the composite: the bootstrap holds no collateral and could not close the gap
// by posting any, because the tier sits above the composite in the packed key.
BOOST_FIXTURE_TEST_CASE( healthy_tier_outranks_the_bootstrap_backstop, producer_score_tester ) try {
   auto collateralized = setup_collateralized_producers(2);

   // A bootstrapped producer: ACTIVE by fiat, holding no collateral at all.
   const auto bootstrap = producer_name_at(5);
   create_producer_accounts({bootstrap});
   BOOST_REQUIRE_EQUAL( success(), push_system_action(bootstrap, "regproducer"_n, mvo()
      ("producer", bootstrap)("producer_key", get_public_key(bootstrap, "active"))("url", "")("location", 0)) );
   BOOST_REQUIRE_EQUAL( success(), register_operator(bootstrap, OperatorType::OPERATOR_TYPE_PRODUCER, true) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( tier_healthy,      tier_of(rank_score_of(collateralized[0])) );
   BOOST_REQUIRE_EQUAL( tier_bootstrapped, tier_of(rank_score_of(bootstrap)) );

   // Every healthy producer outranks the bootstrap backstop, whatever the composites are.
   BOOST_REQUIRE_LT( rank_score_of(collateralized[0]), rank_score_of(bootstrap) );
   BOOST_REQUIRE_LT( rank_score_of(collateralized[1]), rank_score_of(bootstrap) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Missed rounds and demotion
// ---------------------------------------------------------------------------

// Attribution is exact: skipping one producer's window charges that producer and nobody else, and
// producing clears the streak.
BOOST_FIXTURE_TEST_CASE( missed_round_is_charged_only_to_the_skipped_producer, producer_score_tester ) try {
   auto names = setup_ranked_producers(5);
   trigger_reschedule();
   wait_for_active_schedule(names[2]);

   for (const auto& p : names) BOOST_REQUIRE_EQUAL( 0u, missed_rounds_of(p) );
   const uint64_t before = rank_score_of(names[2]);

   skip_round_of(names[2]);

   BOOST_REQUIRE_EQUAL( 1u, missed_rounds_of(names[2]) );
   // One miss is not a demotion, but it IS a worse participation factor: the key moves (a
   // higher key sorts later) while the tier stays put.
   BOOST_REQUIRE_GT( rank_score_of(names[2]), before );
   BOOST_REQUIRE_EQUAL( tier_of(before), tier_of(rank_score_of(names[2])) );
   for (const auto& p : names) {
      if (p == names[2]) continue;
      BOOST_REQUIRE_MESSAGE( missed_rounds_of(p) == 0u,
         p.to_string() << " was charged a miss it did not earn" );
   }

   // One full rotation returns the skipped producer to its slot; producing resets the streak.
   produce_blocks(names.size() * slots_per_producer + slots_per_producer);
   BOOST_REQUIRE_EQUAL( 0u, missed_rounds_of(names[2]) );
   BOOST_REQUIRE_EQUAL( before, rank_score_of(names[2]) );   // and the key comes back with it
} FC_LOG_AND_RETHROW()

// Demotion fires at EXACTLY the configured threshold -- not before -- and no amount of money
// survives it. The target below carries twenty times every other producer's bond, which buys it
// rank 1 while it is healthy and buys it nothing at all once it is demoted: the tier sits above
// the composite in the packed key, which is precisely what makes the uncapped collateral term safe.
BOOST_FIXTURE_TEST_CASE( demotion_fires_at_threshold_and_outweighs_collateral, producer_score_tester ) try {
   auto names = setup_collateralized_producers(5);
   const auto target = names[2];
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(target, base_min_bond * 19) );
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL( 1u, producer_rank_position(target) );

   trigger_reschedule();
   wait_for_active_schedule(target);

   // One full rotation, so the target has produced its own window and holds pay counters to
   // lose -- which is what the reclaim assertions after the demotion need.
   produce_blocks(names.size() * slots_per_producer);
   BOOST_REQUIRE_GT( unpaid_blocks_of(target), 0u );

   for (uint32_t miss = 1; miss <= 3; ++miss) {
      skip_round_of(target);
      BOOST_REQUIRE_EQUAL( miss, missed_rounds_of(target) );
      BOOST_REQUIRE_MESSAGE( demoted(target) == (miss == 3),
         "demotion at miss " << miss << " should be " << (miss == 3) );
   }

   BOOST_REQUIRE_EQUAL( tier_demoted, tier_of(rank_score_of(target)) );
   for (const auto& p : names) {
      if (p == target) continue;
      BOOST_REQUIRE_LT( rank_score_of(p), rank_score_of(target) );
   }
   BOOST_REQUIRE_EQUAL( names.size(), producer_rank_position(target) );   // last, despite the bond

   // Demotion does NOT touch the block count: the producer is paid for the blocks it made at the
   // first payepoch after regproducer brings it back into the pay walk.
   BOOST_REQUIRE_GT( unpaid_blocks_of(target), 0u );
} FC_LOG_AND_RETHROW()

// `regproducer` is the door back for a producer the schedule has DROPPED, from an involuntary
// demotion as much as from a voluntary park. There is no cooldown and no expiry.
BOOST_FIXTURE_TEST_CASE( regproducer_clears_demotion_immediately, producer_score_tester ) try {
   auto names = setup_ranked_producers(5);
   trigger_reschedule();
   wait_for_active_schedule(names[2]);

   const auto target = names[2];
   for (uint32_t miss = 0; miss < 3; ++miss) skip_round_of(target);
   BOOST_REQUIRE( demoted(target) );

   BOOST_REQUIRE_EQUAL( success(), push_system_action(target, "regproducer"_n, mvo()
      ("producer", target)("producer_key", get_public_key(target, "active"))("url", "")("location", 0)) );
   produce_blocks(1);

   BOOST_REQUIRE( !demoted(target) );
   BOOST_REQUIRE_EQUAL( 0u, missed_rounds_of(target) );
   BOOST_REQUIRE_EQUAL( tier_bootstrapped, tier_of(rank_score_of(target)) );
} FC_LOG_AND_RETHROW()

// The OTHER door back, and the one no operator has to walk through: a demoted producer that is
// still in the active schedule recovers by producing a block.
//
// Demotion and rescheduling are separate events, and the schedule-size floor can hold the gap
// between them open indefinitely. Four producers here, one demoted, leaves three schedulable --
// below `min_schedule_size` -- so `update_ranked_producers` retains the last good schedule rather
// than publish a short one, and the demoted producer keeps its slot. That is the shape a mass
// outage takes: without this, those producers would produce indefinitely while `payepoch` skipped
// them, earning nothing until every operator pushed `regproducer` by hand.
BOOST_FIXTURE_TEST_CASE( producing_while_still_scheduled_clears_a_demotion, producer_score_tester ) try {
   auto names = setup_ranked_producers(4);
   trigger_reschedule();
   wait_for_active_schedule(names[2]);

   const auto target = names[2];
   for (uint32_t miss = 0; miss < 3; ++miss) skip_round_of(target);
   BOOST_REQUIRE( demoted(target) );
   BOOST_REQUIRE_EQUAL( tier_demoted, tier_of(rank_score_of(target)) );

   // The floor kept it in the schedule: three schedulable producers cannot replace four.
   trigger_reschedule();
   const auto schedule = active_schedule_names();
   BOOST_REQUIRE_MESSAGE(
      std::find(schedule.begin(), schedule.end(), target) != schedule.end(),
      "the demoted producer should still hold its slot under the schedule-size floor" );

   // Its window comes round and it produces. One block is proof of life, so the demotion and the
   // streak both clear and the key returns to the tier its standing earns.
   produce_blocks(names.size() * slots_per_producer + slots_per_producer);

   BOOST_REQUIRE( !demoted(target) );
   BOOST_REQUIRE_EQUAL( 0u, missed_rounds_of(target) );
   BOOST_REQUIRE_EQUAL( tier_bootstrapped, tier_of(rank_score_of(target)) );
} FC_LOG_AND_RETHROW()

// A voluntary park costs the producer its schedule slot and its rank position, but nothing else:
// its opreg status and bond are untouched, so it returns at the position its collateral earns.
BOOST_FIXTURE_TEST_CASE( unregprod_parks_without_touching_the_bond, producer_score_tester ) try {
   auto names = setup_collateralized_producers(5);
   trigger_reschedule();
   BOOST_REQUIRE( is_scheduled(names[0]) );
   const uint64_t before = rank_score_of(names[0]);

   BOOST_REQUIRE_EQUAL( success(),
      push_system_action(names[0], "unregprod"_n, mvo()("producer", names[0])) );
   // The park rescores at once: a parked row is not a live producer, so its key sinks to the
   // demoted tier where no walk visits it -- not left in the healthy tier until some unrelated
   // event happened to rescore it.
   BOOST_REQUIRE_EQUAL( tier_demoted, tier_of(rank_score_of(names[0])) );
   trigger_reschedule();

   BOOST_REQUIRE( !is_scheduled(names[0]) );
   BOOST_REQUIRE_EQUAL( 0u, producer_rank_position(names[0]) );   // consumes no position
   {
      const auto op = get_opreg_operator(names[0]);
      BOOST_REQUIRE_EQUAL( "OPERATOR_STATUS_ACTIVE", op["status"].as_string() );
   }

   BOOST_REQUIRE_EQUAL( success(), push_system_action(names[0], "regproducer"_n, mvo()
      ("producer", names[0])("producer_key", get_public_key(names[0], "active"))("url", "")("location", 0)) );
   BOOST_REQUIRE_EQUAL( tier_healthy, tier_of(rank_score_of(names[0])) );
   trigger_reschedule();

   BOOST_REQUIRE( is_scheduled(names[0]) );
   BOOST_REQUIRE_EQUAL( before, rank_score_of(names[0]) );        // same bond, same position
   BOOST_REQUIRE_EQUAL( 1u, producer_rank_position(names[0]) );
} FC_LOG_AND_RETHROW()

// A slash or a termination ends a producer's standing, and its rank key has to say so at once:
// opreg dispatches the same `processprod` notification it uses for balance changes, and the
// rescore sinks the key to the demoted tier in the same transaction. Before this, a slashed
// position-1 producer stayed first in the index -- skipped by every walk, but visited by every
// one of them -- until an unrelated event rescored it.
BOOST_FIXTURE_TEST_CASE( slash_and_termination_sink_the_key_at_once, producer_score_tester ) try {
   auto names = setup_collateralized_producers(5);
   for (const auto& p : names) BOOST_REQUIRE_EQUAL( tier_healthy, tier_of(rank_score_of(p)) );

   BOOST_REQUIRE_EQUAL( success(), slash_operator(names[4]) );
   BOOST_REQUIRE_EQUAL( tier_demoted, tier_of(rank_score_of(names[4])) );

   BOOST_REQUIRE_EQUAL( success(), terminate_operator(names[3]) );
   BOOST_REQUIRE_EQUAL( tier_demoted, tier_of(rank_score_of(names[3])) );

   // The others are untouched, and the two sunk rows now sort BEHIND every one of them in index
   // order (equal demoted keys fall back to name order), so a walk stops before reaching either.
   for (uint32_t i = 0; i < 3; ++i) BOOST_REQUIRE_EQUAL( tier_healthy, tier_of(rank_score_of(names[i])) );
   BOOST_REQUIRE_EQUAL( 3u, producer_rank_position(names[2]) );
   BOOST_REQUIRE_EQUAL( 4u, producer_rank_position(names[3]) );
   BOOST_REQUIRE_EQUAL( 5u, producer_rank_position(names[4]) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Position versus index slot
// ---------------------------------------------------------------------------

// A permissionless `regproducer` with no bond behind it occupies an index slot but must consume no
// rank POSITION: it sinks to the demoted tier, so it sorts behind every real producer and every
// consumer's walk stops before reaching it.
BOOST_FIXTURE_TEST_CASE( unbonded_registrant_consumes_no_rank_position, producer_score_tester ) try {
   // Five producers, not three: below min_schedule_size (4) update_ranked_producers retains the
   // last good schedule instead of publishing, so the scheduling assertions below would be vacuous.
   auto names = setup_collateralized_producers(5);

   // A registrant that never bonded: a producers row, no operator row at all.
   const auto squatter = producer_name_at(5);
   create_producer_accounts({squatter});
   BOOST_REQUIRE_EQUAL( success(), push_system_action(squatter, "regproducer"_n, mvo()
      ("producer", squatter)("producer_key", get_public_key(squatter, "active"))("url", "")("location", 0)) );
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL( tier_demoted, tier_of(rank_score_of(squatter)) );
   for (uint32_t i = 0; i < names.size(); ++i) {
      BOOST_REQUIRE_EQUAL( i + 1, producer_rank_position(names[i]) );
   }

   trigger_reschedule();
   BOOST_REQUIRE( !is_scheduled(squatter) );
   BOOST_REQUIRE(  is_scheduled(names[0]) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Collateralising a bootstrap
// ---------------------------------------------------------------------------

// A bootstrapped operator is ACTIVE by fiat and bypasses `meets_role_min` entirely, so collateral
// credited to one could never affect its eligibility -- the deposit would land in a balance that
// does nothing. `depositinle` already refused it; the WIRE-direct `deposit` now does too. There is
// deliberately no way to collateralise a bootstrap: an operator who wants one registers a new
// account.
BOOST_FIXTURE_TEST_CASE( deposit_rejects_a_bootstrapped_operator, producer_score_tester ) try {
   auto names = setup_ranked_producers(1);

   BOOST_REQUIRE_EQUAL( wasm_assert_msg("bootstrapped operators cannot deposit collateral"),
      push_opreg_action(names[0], "deposit"_n, mvo()("account", names[0])("amount", uint64_t{1'000})) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Saturation
// ---------------------------------------------------------------------------

// The collateral factor is UNCAPPED by policy, so a large enough bond runs it to the composite's
// bit budget; the weight multiply and the composite sum then saturate rather than wrap. A wrap
// would drop the best-bonded producer to the bottom of the index -- exactly what the uint128
// intermediate exists to prevent. Two producers past the ceiling must tie, and both must still
// outrank a producer at exactly the minimum.
BOOST_FIXTURE_TEST_CASE( collateral_factor_saturates_at_the_bit_budget, producer_score_tester ) try {
   // A minimum bond of 1 makes the ratio the raw deposit times score_scale, so a deposit of 1e15
   // is a factor of 1e19 -- past the 2^62 composite ceiling (~4.6e18) before any weight applies.
   constexpr uint64_t unit_min_bond   = 1;
   constexpr uint64_t saturating_bond = 1'000'000'000'000'000ULL;

   BOOST_REQUIRE_EQUAL( success(), set_single_pair_collateral(unit_min_bond) );
   produce_blocks(1);

   auto names = producer_names(3);
   create_producer_accounts(names);
   for (auto& p : names) {
      BOOST_REQUIRE_EQUAL( success(), push_system_action(p, "regproducer"_n, mvo()
         ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)) );
      BOOST_REQUIRE_EQUAL( success(), register_operator(p, OperatorType::OPERATOR_TYPE_PRODUCER, false) );
   }
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[0], saturating_bond) );
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[1], saturating_bond * 2) );
   BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[2], unit_min_bond) );
   produce_blocks(1);
   register_finalizer_keys(names, 3);
   produce_blocks(1);

   for (const auto& p : names) {
      BOOST_REQUIRE_EQUAL( tier_healthy, tier_of(rank_score_of(p)) );
   }
   // Past the ceiling, twice the bond buys nothing: the two saturated keys are identical...
   BOOST_REQUIRE_EQUAL( rank_score_of(names[0]), rank_score_of(names[1]) );
   // ...and neither wrapped: both still sort ahead of the producer at exactly the minimum.
   BOOST_REQUIRE_LT( rank_score_of(names[0]), rank_score_of(names[2]) );
   BOOST_REQUIRE_EQUAL( 1u, producer_rank_position(names[0]) );   // equal keys: name order
   BOOST_REQUIRE_EQUAL( 2u, producer_rank_position(names[1]) );
   BOOST_REQUIRE_EQUAL( 3u, producer_rank_position(names[2]) );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Displacement at the schedule boundary
// ---------------------------------------------------------------------------

// The bootstrapped tier is a BACKSTOP: with max_producers collateralised producers ahead of it a
// bootstrap holds position 22 and no schedule slot, yet stays ACTIVE and eligible. The moment a
// collateralised producer leaves it moves into the schedule, and the moment one returns it yields
// the slot again -- with no governance action anywhere.
BOOST_FIXTURE_TEST_CASE( collateralised_producers_displace_the_bootstrap_at_the_boundary, producer_score_tester ) try {
   constexpr uint32_t collateralised_count = 21;   // max_producers
   static_assert( collateralised_count == 21, "this test pins the 21/22 schedule boundary" );

   BOOST_REQUIRE_EQUAL( success(), set_single_pair_collateral() );
   produce_blocks(1);

   // One roster: 21 collateralised producers plus the bootstrap, keyed in ONE call so the node
   // holds every finalizer key any proposed policy can carry.
   auto names = producer_names(collateralised_count + 1);
   const auto bootstrap = names.back();
   create_producer_accounts(names);
   for (auto& p : names) {
      BOOST_REQUIRE_EQUAL( success(), push_system_action(p, "regproducer"_n, mvo()
         ("producer", p)("producer_key", get_public_key(p, "active"))("url", "")("location", 0)) );
   }
   for (uint32_t i = 0; i < collateralised_count; ++i) {
      BOOST_REQUIRE_EQUAL( success(), register_operator(names[i], OperatorType::OPERATOR_TYPE_PRODUCER, false) );
      BOOST_REQUIRE_EQUAL( success(), credit_collateral(names[i], base_min_bond) );
   }
   BOOST_REQUIRE_EQUAL( success(), register_operator(bootstrap, OperatorType::OPERATOR_TYPE_PRODUCER, true) );
   produce_blocks(1);
   register_finalizer_keys(names, collateralised_count + 1);
   produce_blocks(1);

   // Position 22: outside the schedule, but ACTIVE and holding a rank position.
   trigger_reschedule();
   BOOST_REQUIRE_EQUAL( tier_bootstrapped, tier_of(rank_score_of(bootstrap)) );
   BOOST_REQUIRE_EQUAL( collateralised_count + 1, producer_rank_position(bootstrap) );
   BOOST_REQUIRE( !is_scheduled(bootstrap) );
   BOOST_REQUIRE(  is_scheduled(names[0]) );
   BOOST_REQUIRE_EQUAL( "OPERATOR_STATUS_ACTIVE", get_opreg_operator(bootstrap)["status"].as_string() );

   // A collateralised producer parks: the bootstrap moves up into the schedule.
   BOOST_REQUIRE_EQUAL( success(),
      push_system_action(names[0], "unregprod"_n, mvo()("producer", names[0])) );
   trigger_reschedule();
   BOOST_REQUIRE_EQUAL( collateralised_count, producer_rank_position(bootstrap) );
   BOOST_REQUIRE(  is_scheduled(bootstrap) );
   BOOST_REQUIRE( !is_scheduled(names[0]) );

   // It returns: the bootstrap yields the slot again, and is still ACTIVE for the next time.
   BOOST_REQUIRE_EQUAL( success(), push_system_action(names[0], "regproducer"_n, mvo()
      ("producer", names[0])("producer_key", get_public_key(names[0], "active"))("url", "")("location", 0)) );
   trigger_reschedule();
   BOOST_REQUIRE_EQUAL( collateralised_count + 1, producer_rank_position(bootstrap) );
   BOOST_REQUIRE( !is_scheduled(bootstrap) );
   BOOST_REQUIRE(  is_scheduled(names[0]) );
   BOOST_REQUIRE_EQUAL( "OPERATOR_STATUS_ACTIVE", get_opreg_operator(bootstrap)["status"].as_string() );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END() // sysio_producer_score_tests
