// contracts/tests/emissions_tests.cpp
//
// Focus: emissions logic in sysio.system:
//  - setinittime singleton initialization and immutability
//  - addnodeowner authorization + input validation + row creation per tier
//  - viewnodedist functional behavior (claimable/can_claim) across time states
//  - claimnodedis authorization + gating rules + claimed accounting updates + inline token ftransfer
//  - sysio.roa::forcereg wiring: inline addnodeowner occurs and writes nodedist


#include <test_contracts.hpp>

#include <boost/test/unit_test.hpp>

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include "sysio.system_tester.hpp"

#include <fc/variant_object.hpp>
#include <fc/io/raw.hpp>
#include <fc/reflect/reflect.hpp>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

// sysio.roa is the authority expected by sysio.system::addnodeowner (require_auth("sysio.roa"_n))
static constexpr account_name ROA = "sysio.roa"_n;

// Keep these in sync with contracts/sysio.system/src/emissions.cpp
static constexpr uint32_t SECONDS_PER_MONTH = 30u * 24u * 60u * 60u;
static constexpr uint32_t T1_DURATION       = 12u * SECONDS_PER_MONTH;
static constexpr uint32_t T2_DURATION       = 24u * SECONDS_PER_MONTH;
static constexpr uint32_t T3_DURATION       = 36u * SECONDS_PER_MONTH;

// MIN_CLAIMABLE in emissions.cpp: asset(1000000000, WIRE_SYMBOL)
static constexpr int64_t MIN_CLAIMABLE_AMOUNT = 1'000'000'000;

// In unit tests we use sysio::chain::* types; chain::symbol is not constexpr.
static const symbol WIRE_SYMBOL = symbol(8, "WIRE");

// Node Owner total_claimable amounts (in WIRE subunits)
static const asset T1_ALLOCATION(750000000000000, WIRE_SYMBOL);
static const asset T2_ALLOCATION(100000000000000, WIRE_SYMBOL);
static const asset T3_ALLOCATION(10000000000000,   WIRE_SYMBOL);

static constexpr account_name TOKEN = "sysio.token"_n;
static constexpr uint8_t      NETWORK_GEN = 0;

// Keep as a string because sysio.token table helpers in tests use symbol::from_string("p,SYM")
static const std::string WIRE_SYM_STR = "8,WIRE";

// Fund sysio heavily so claims never fail due to insufficient token balance.
static const asset WIRE_MAX_SUPPLY = asset::from_string("1000000000.00000000 WIRE");
static const asset WIRE_ISSUE_TO_SYSIO = asset::from_string("1000000000.00000000 WIRE");

// Mirror the on-chain return struct layout for viewnodedist.
// sysio.system::viewnodedist returns a packed node_claim_result.
struct node_claim_result {
   asset total_allocation;
   asset claimed;
   asset claimable;
   bool  can_claim;
};
FC_REFLECT( node_claim_result, (total_allocation)(claimed)(claimable)(can_claim) )

static time_point_sec tpsec(uint32_t secs) {
   return time_point_sec{ secs };
}

// Quiet substring checker for action_result strings.
static void require_substr(const std::string& s, const std::string& needle) {
   BOOST_REQUIRE( s.find(needle) != std::string::npos );
}

class sysio_emissions_tester : public tester {
public:
   sysio_emissions_tester() {
      produce_blocks(2);

      // --- sysio.system (emissions lives here) ---
      set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
      set_abi ( config::system_account_name, test_contracts::sysio_system_abi() );

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
   // sysio.system action helpers
   // -----------------------------

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
   // viewnodedist return decoding
   // -----------------------------

   /// Calls sysio.system::viewnodedist and decodes the return_value into node_claim_result.
   /// We search action_traces for the sysio.system receiver trace for this action to avoid
   /// decoding the wrong trace in a nested/inline scenario.
   node_claim_result viewnodedist( account_name signer, account_name owner ) {
      auto trace = push_system_action_trace(
         signer,
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
      auto data0 = get_row_by_account(config::system_account_name,
                                  config::system_account_name,
                                  "emissionmngr"_n,
                                  account_name{0});
      if (!data0.empty()) {
         return sysio_abi_ser.binary_to_variant("emission_state", data0,
             abi_serializer::create_yield_function(abi_serializer_max_time));
      }

      vector<char> data1 = get_row_by_account(
         config::system_account_name,
         config::system_account_name,
         "emissionmngr"_n,
         "emissionmngr"_n
      );
      if (!data1.empty()) {
         return sysio_abi_ser.binary_to_variant(
            "emission_state",
            data1,
            abi_serializer::create_yield_function(abi_serializer_max_time)
         );
      }

      return fc::variant();
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
      // policies table is scoped by issuer; primary key is policy_owner
      // This is exactly how sysio.roa_tests.cpp does it. :contentReference[oaicite:2]{index=2}
      const auto& db = control->db();
      if (const auto* table = db.find<table_id_object, by_code_scope_table>(
             boost::make_tuple(ROA, issuer, "policies"_n))) {
         if (auto* obj = db.find<key_value_object, by_scope_primary>(
                boost::make_tuple(table->id, policy_owner.to_uint64_t()))) {
            const vector<char> data(obj->value.data(), obj->value.data() + obj->value.size());
            if (!data.empty()) {
               return roa_abi_ser.binary_to_variant(
                  "policies", data,
                  abi_serializer::create_yield_function(abi_serializer_max_time)
               );
            }
                }
             }
      return fc::variant();
   }

private:
   abi_serializer sysio_abi_ser;
   abi_serializer roa_abi_ser;
   abi_serializer token_abi_ser;
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
   require_substr( r, "emission table already exists" );
} FC_LOG_AND_RETHROW()

// -----------------------------------------------------------------------------
// addnodeowner
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( addnodeowner_requires_emission_initialized, sysio_emissions_tester ) try {
   create_user_accounts({ "nodeowner1"_n });

   auto r = addnodeowner( ROA, "nodeowner1"_n, 1 );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "emission state not initialized" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( addnodeowner_requires_sysio_roa_auth, sysio_emissions_tester ) try {
   create_user_accounts({ "alice"_n, "nodeowner2"_n });

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

   auto r = addnodeowner( "alice"_n, "nodeowner2"_n, 1 );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "missing authority of sysio.roa" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( addnodeowner_rejects_invalid_tier, sysio_emissions_tester ) try {
   // Tier must be 1..3
   create_user_accounts({ "nodeowner3"_n });

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

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

   BOOST_REQUIRE_EQUAL( success(), setinittime( config::system_account_name, tpsec(head_secs()) ) );

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
   require_substr( dup, "account already exists" );
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

   auto info = viewnodedist( "nodefuture"_n, "nodefuture"_n );
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

   auto info1 = viewnodedist( "nodemid"_n, "nodemid"_n );
   BOOST_REQUIRE_EQUAL(info1.total_allocation, T3_ALLOCATION);
   BOOST_REQUIRE_EQUAL( info1.claimed, asset(0, WIRE_SYMBOL) );
   BOOST_REQUIRE( info1.claimable.get_amount() > 0 );
   BOOST_REQUIRE( info1.claimable.get_amount() < MIN_CLAIMABLE_AMOUNT );
   BOOST_REQUIRE( !info1.can_claim );

   // Move time forward a bit; claimable should increase but still stay below MIN.
   produce_blocks(200);

   auto info2 = viewnodedist( "nodemid"_n, "nodemid"_n );
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

   auto before = viewnodedist( "nodefull"_n, "nodefull"_n );
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

   auto after = viewnodedist( "nodefull"_n, "nodefull"_n );
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
   auto info_pre = viewnodedist( "nodesmall"_n, "nodesmall"_n );
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
   auto info_final = viewnodedist( "nodesmall"_n, "nodesmall"_n );
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

BOOST_AUTO_TEST_SUITE_END()
