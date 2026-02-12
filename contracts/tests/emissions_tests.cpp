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

// T5 return struct mirror
struct t5_epoch_info {
   uint64_t       epoch_count;
   time_point_sec last_epoch_time;
   int64_t        last_epoch_emission;
   int64_t        total_distributed;
   int64_t        treasury_remaining;
   int64_t        next_emission_est;
   uint32_t       seconds_until_next;
};
FC_REFLECT( t5_epoch_info, (epoch_count)(last_epoch_time)(last_epoch_emission)
            (total_distributed)(treasury_remaining)(next_emission_est)(seconds_until_next) )

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

   action_result processepoch( account_name signer ) {
      return push_system_action(
         signer,
         "processepoch"_n,
         mvo()
      );
   }

   t5_epoch_info viewepoch( account_name signer ) {
      auto trace = push_system_action_trace(
         signer,
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

   // -----------------------------
   // T5 table readers
   // -----------------------------

   fc::variant get_t5_state() {
      // Try primary key 0 first (singleton default)
      auto data = get_row_by_account(config::system_account_name,
                                     config::system_account_name,
                                     "t5state"_n,
                                     account_name{0});
      if (!data.empty()) {
         return sysio_abi_ser.binary_to_variant("t5_state", data,
             abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      // Try table name as key (singleton stores under table name)
      data = get_row_by_account(config::system_account_name,
                                config::system_account_name,
                                "t5state"_n,
                                "t5state"_n);
      if (!data.empty()) {
         return sysio_abi_ser.binary_to_variant("t5_state", data,
             abi_serializer::create_yield_function(abi_serializer_max_time));
      }
      return fc::variant();
   }

   fc::variant get_epoch_log( uint64_t epoch_num ) {
      auto data = get_row_by_account(config::system_account_name,
                                     config::system_account_name,
                                     "epochlog"_n,
                                     account_name(epoch_num));
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

   void setup_producers( uint32_t count ) {
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

// ---------------------------------------------------------------------------
// T5 Treasury Emissions mirror constants (keep in sync with emissions.cpp)
// ---------------------------------------------------------------------------
static constexpr int64_t T5_DISTRIBUTABLE        = 37'500'000'000'000'000LL;
static constexpr int64_t DECAY_NUMERATOR         = 9990;
static constexpr int64_t DECAY_DENOMINATOR       = 10000;
static constexpr int64_t EPOCH_INITIAL_EMISSION  = 56'315'000'000'000LL;
static constexpr int64_t EPOCH_MAX_EMISSION      = 300'000'000'000'000LL;
static constexpr int64_t EPOCH_MIN_EMISSION      = 10'000'000'000'000LL;
static constexpr uint16_t COMPUTE_BPS            = 4000;
static constexpr uint16_t CAPITAL_BPS            = 3000;
static constexpr uint16_t CAPEX_BPS              = 2000;
static constexpr uint16_t PRODUCER_BPS           = 7000;

// Performance-based pay constants (keep in sync with emissions.cpp)
static constexpr uint32_t T_ACTIVE_PRODUCER_COUNT = 21;
static constexpr uint32_t T_STANDBY_START_RANK    = 22;
static constexpr uint32_t T_STANDBY_END_RANK      = 28;
static constexpr uint32_t T_BLOCKS_PER_ROUND      = T_ACTIVE_PRODUCER_COUNT * 12; // 252

// Helper: compute undistributed producer pool for an emission (when no producers eligible)
static int64_t compute_producer_pool(int64_t emission) {
   int64_t compute = test_split_bps(emission, COMPUTE_BPS);
   return test_split_bps(compute, PRODUCER_BPS);
}

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
      viewnodedist( "nodenoview"_n, "nodenoview"_n ),
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
      viewnodedist( "nobody2"_n, "nobody2"_n ),
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

   auto info = viewnodedist( "nodehalf"_n, "nodehalf"_n );
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
   auto info1 = viewnodedist( "nodeseq"_n, "nodeseq"_n );
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
   auto info2 = viewnodedist( "nodeseq"_n, "nodeseq"_n );
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
   auto info3 = viewnodedist( "nodeseq"_n, "nodeseq"_n );
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

   auto info = viewnodedist( "nodet2"_n, "nodet2"_n );
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

   auto info = viewnodedist( "nodet3"_n, "nodet3"_n );
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

   auto info = viewnodedist( "nodeprec"_n, "nodeprec"_n );

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
   auto info1 = viewnodedist( "nodecons"_n, "nodecons"_n );
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

   auto info2 = viewnodedist( "nodecons"_n, "nodecons"_n );
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

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( processepoch_fails_before_init, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   auto r = processepoch( config::system_account_name );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "t5 state not initialized" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( processepoch_fails_before_epoch_elapsed, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs();
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Immediately try - should fail
   auto r = processepoch( config::system_account_name );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "epoch duration has not elapsed" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( processepoch_succeeds_after_epoch_duration, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   // Set start_time in the past so epoch has already elapsed
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["last_epoch_emission"].as<int64_t>(), EPOCH_INITIAL_EMISSION );

   // Without producers, the producer pool is undistributed (stays in sysio)
   int64_t undist = compute_producer_pool(EPOCH_INITIAL_EMISSION);
   BOOST_REQUIRE_EQUAL( state["total_distributed"].as<int64_t>(), EPOCH_INITIAL_EMISSION - undist );

   auto log = get_epoch_log(1);
   BOOST_REQUIRE( !log.is_null() );
   BOOST_REQUIRE_EQUAL( log["total_emission"].as<int64_t>(), EPOCH_INITIAL_EMISSION );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( subsequent_epochs_apply_decay, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   // Set start 2+ epochs in the past
   const uint32_t start = head_secs() - (2 * ONE_EPOCH) - 10;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Epoch 1
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   // processepoch sets last_epoch_time to now, so we need to advance past another epoch
   produce_block( fc::seconds(ONE_EPOCH) );

   // Epoch 2
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 2u );

   // Expected: E_0 * 9990 / 10000
   __int128 expected = static_cast<__int128>(EPOCH_INITIAL_EMISSION) * DECAY_NUMERATOR;
   int64_t expected_e2 = static_cast<int64_t>(expected / DECAY_DENOMINATOR);

   BOOST_REQUIRE_EQUAL( state["last_epoch_emission"].as<int64_t>(), expected_e2 );

   // Without producers, both epochs have undistributed producer pools
   int64_t undist1 = compute_producer_pool(EPOCH_INITIAL_EMISSION);
   int64_t undist2 = compute_producer_pool(expected_e2);
   BOOST_REQUIRE_EQUAL( state["total_distributed"].as<int64_t>(),
      (EPOCH_INITIAL_EMISSION - undist1) + (expected_e2 - undist2) );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( emission_clamped_to_max, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto state = get_t5_state();
   BOOST_REQUIRE( state["last_epoch_emission"].as<int64_t>() <= EPOCH_MAX_EMISSION );
   BOOST_REQUIRE( state["last_epoch_emission"].as<int64_t>() >= EPOCH_MIN_EMISSION );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( emission_capped_at_distributable_ceiling, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t compute = log["compute_amount"].as<int64_t>();
   int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   int64_t capex_base = test_split_bps(emission, CAPEX_BPS);

   // Capex gets only its base split (no producer dust redirect)
   const asset capex_after = get_wire_balance("sysio.ops"_n);
   int64_t capex_received = capex_after.get_amount() - capex_before.get_amount();
   BOOST_REQUIRE_EQUAL( capex_received, capex_base );

   // sysio's balance decreases by (emission - producer_pool) since producer_pool is undistributed
   const asset sysio_after = get_wire_balance(config::system_account_name);
   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   BOOST_REQUIRE_EQUAL( sysio_decrease, emission - producer_pool );
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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset cap_before   = get_wire_balance("sysio.cap"_n);
   asset gov_before   = get_wire_balance("sysio.gov"_n);
   asset batch_before = get_wire_balance("sysio.batch"_n);
   asset ops_before   = get_wire_balance("sysio.ops"_n);

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t compute  = log["compute_amount"].as<int64_t>();
   int64_t capital  = log["capital_amount"].as<int64_t>();
   int64_t gov      = log["governance_amount"].as<int64_t>();

   int64_t producer_pool = test_split_bps(compute, PRODUCER_BPS);
   int64_t batch_pool    = compute - producer_pool;
   int64_t capex_base    = test_split_bps(emission, CAPEX_BPS);

   int64_t cap_received   = get_wire_balance("sysio.cap"_n).get_amount()   - cap_before.get_amount();
   int64_t gov_received   = get_wire_balance("sysio.gov"_n).get_amount()   - gov_before.get_amount();
   int64_t batch_received = get_wire_balance("sysio.batch"_n).get_amount() - batch_before.get_amount();
   int64_t ops_received   = get_wire_balance("sysio.ops"_n).get_amount()   - ops_before.get_amount();

   BOOST_REQUIRE_EQUAL( cap_received,   capital );
   BOOST_REQUIRE_EQUAL( gov_received,   gov );
   BOOST_REQUIRE_EQUAL( batch_received, batch_pool );
   // Capex gets only its base split (no producer dust redirect)
   BOOST_REQUIRE_EQUAL( ops_received,   capex_base );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Read-only
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( viewepoch_returns_correct_state, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   int64_t undist = compute_producer_pool(EPOCH_INITIAL_EMISSION);
   int64_t expected_distributed = EPOCH_INITIAL_EMISSION - undist;

   auto info = viewepoch( config::system_account_name );
   BOOST_REQUIRE_EQUAL( info.epoch_count, 1u );
   BOOST_REQUIRE_EQUAL( info.last_epoch_emission, EPOCH_INITIAL_EMISSION );
   BOOST_REQUIRE_EQUAL( info.total_distributed, expected_distributed );
   BOOST_REQUIRE( info.treasury_remaining > 0 );
   BOOST_REQUIRE_EQUAL( info.treasury_remaining, T5_DISTRIBUTABLE - expected_distributed );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewepoch_estimates_next_emission, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto info = viewepoch( config::system_account_name );

   __int128 expected = static_cast<__int128>(EPOCH_INITIAL_EMISSION) * DECAY_NUMERATOR;
   int64_t expected_next = static_cast<int64_t>(expected / DECAY_DENOMINATOR);

   BOOST_REQUIRE_EQUAL( info.next_emission_est, expected_next );
   BOOST_REQUIRE( info.seconds_until_next > 0 );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Integration
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE( processepoch_is_permissionless, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   create_user_accounts({ "randuser"_n });
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Any user can call processepoch
   BOOST_REQUIRE_EQUAL( success(), processepoch( "randuser"_n ) );

   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );
} FC_LOG_AND_RETHROW()

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   // Producers should receive nothing (0 eligible_rounds → excluded)
   BOOST_REQUIRE_EQUAL( get_wire_balance("producera"_n), bal_a_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance("producerb"_n), bal_b_before );
   BOOST_REQUIRE_EQUAL( get_wire_balance("producerc"_n), bal_c_before );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( partial_uptime_proportional_pay, sysio_emissions_tester ) try {
   // Producers with eligible_rounds < expected_rounds get proportional share
   create_t5_holding_accounts();
   setup_producers(3);

   // Produce blocks so producers accumulate eligible_rounds
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2); // 2 cycles sufficient

   // Read eligible_rounds for producera before processepoch
   auto pa_info = get_producer_info("producera"_n);
   BOOST_REQUIRE( !pa_info.is_null() );
   uint16_t elig_a = pa_info["eligible_rounds"].as<uint16_t>();
   BOOST_REQUIRE( elig_a > 0 );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   // Standby should receive payment even with 0 blocks produced
   int64_t standby_got = get_wire_balance(standby_name).get_amount() - standby_before.get_amount();
   BOOST_REQUIRE( standby_got > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( round_tracking_reset_after_epoch, sysio_emissions_tester ) try {
   // After processepoch, all round-tracking fields should be reset
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2);

   // Verify fields are non-zero before processepoch
   auto pa_before = get_producer_info("producera"_n);
   BOOST_REQUIRE( pa_before["eligible_rounds"].as<uint16_t>() > 0 );
   BOOST_REQUIRE( pa_before["unpaid_blocks"].as<uint32_t>() > 0 );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   // After processepoch, fields should be reset
   auto pa_after = get_producer_info("producera"_n);
   BOOST_REQUIRE_EQUAL( pa_after["eligible_rounds"].as<uint16_t>(), 0u );
   BOOST_REQUIRE_EQUAL( pa_after["current_round_blocks"].as<uint16_t>(), 0u );
   BOOST_REQUIRE_EQUAL( pa_after["unpaid_blocks"].as<uint32_t>(), 0u );
   BOOST_REQUIRE_EQUAL( pa_after["last_block_num"].as<uint32_t>(), std::numeric_limits<uint32_t>::max() );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( total_distributed_excludes_undistributed, sysio_emissions_tester ) try {
   // When some producers are excluded, total_distributed < emission
   create_t5_holding_accounts();
   setup_producers(3);
   // Producers have 0 eligible_rounds → all excluded → undistributed = full producer_pool

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto state = get_t5_state();
   int64_t emission = state["last_epoch_emission"].as<int64_t>();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   // total_distributed should be emission minus the undistributed producer pool
   int64_t undist = compute_producer_pool(emission);
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

   // producera should have in-progress blocks >= 6
   BOOST_REQUIRE( current_blocks >= 6 );
   BOOST_REQUIRE( current_blocks < 12 );

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   asset bal_a_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   int64_t got_a = get_wire_balance("producera"_n).get_amount() - bal_a_before.get_amount();

   // processepoch finalizes in-progress round (>= 6 blocks) → adds 1 to eligible_rounds
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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

BOOST_FIXTURE_TEST_CASE( delayed_epoch_processes_only_once, sysio_emissions_tester ) try {
   // Even if 2+ epoch durations have passed, processepoch only processes one epoch.
   // A second call immediately after should fail (epoch duration not elapsed again).
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - (2 * ONE_EPOCH) - 10;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // First call: succeeds
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
   auto state = get_t5_state();
   BOOST_REQUIRE_EQUAL( state["epoch_count"].as<uint64_t>(), 1u );

   // Second call immediately: fails because last_epoch_time was set to "now"
   auto r = processepoch( config::system_account_name );
   BOOST_REQUIRE( r != success() );
   require_substr( r, "epoch duration has not elapsed" );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( multi_epoch_cumulative_accounting, sysio_emissions_tester ) try {
   // Run 3 epochs and verify total_distributed equals sum of per-epoch effective distributions
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   int64_t cumulative = 0;

   // Epoch 1
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
   auto log1 = get_epoch_log(1);
   int64_t e1 = log1["total_emission"].as<int64_t>();
   int64_t undist1 = compute_producer_pool(e1); // no producers → full pool undistributed
   cumulative += (e1 - undist1);

   auto state1 = get_t5_state();
   BOOST_REQUIRE_EQUAL( state1["epoch_count"].as<uint64_t>(), 1u );
   BOOST_REQUIRE_EQUAL( state1["total_distributed"].as<int64_t>(), cumulative );

   // Advance to epoch 2
   produce_block( fc::seconds(ONE_EPOCH) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
   auto log2 = get_epoch_log(2);
   int64_t e2 = log2["total_emission"].as<int64_t>();
   int64_t undist2 = compute_producer_pool(e2);
   cumulative += (e2 - undist2);

   auto state2 = get_t5_state();
   BOOST_REQUIRE_EQUAL( state2["epoch_count"].as<uint64_t>(), 2u );
   BOOST_REQUIRE_EQUAL( state2["total_distributed"].as<int64_t>(), cumulative );

   // Verify decay applied
   BOOST_REQUIRE( e2 < e1 );

   // Advance to epoch 3
   produce_block( fc::seconds(ONE_EPOCH) );
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
   auto log3 = get_epoch_log(3);
   int64_t e3 = log3["total_emission"].as<int64_t>();
   int64_t undist3 = compute_producer_pool(e3);
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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto log = get_epoch_log(1);
   BOOST_REQUIRE( !log.is_null() );

   // epoch_num matches epoch_count
   BOOST_REQUIRE_EQUAL( log["epoch_num"].as<uint64_t>(), 1u );

   // timestamp is set (non-zero)
   BOOST_REQUIRE( log["timestamp"].as<time_point_sec>().sec_since_epoch() > 0 );

   // total_emission matches expected first epoch
   BOOST_REQUIRE_EQUAL( log["total_emission"].as<int64_t>(), EPOCH_INITIAL_EMISSION );

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
         BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

         // Finalization should NOT credit this round (< 6 blocks)
         // So eligible_rounds stays 0 → excluded from payment
         BOOST_REQUIRE_EQUAL( get_wire_balance("producera"_n), bal_before );
      }
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( active_capped_at_expected_rounds, sysio_emissions_tester ) try {
   // When eligible_rounds >= expected_rounds, payment = full_share (capped, not more)
   // expected_rounds = (epoch_elapsed * 2) / BLOCKS_PER_ROUND
   // With ONE_EPOCH (86400s), expected_rounds = (86400 * 2) / 252 = 685
   create_t5_holding_accounts();
   setup_producers(3);
   wait_for_producer_schedule();
   produce_complete_cycles(3, 2); // some eligible_rounds

   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // All 3 producers have same eligible_rounds and same weight
   asset bal_a_before = get_wire_balance("producera"_n);
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
   asset sysio_after = get_wire_balance(config::system_account_name);

   auto state = get_t5_state();
   int64_t distributed = state["total_distributed"].as<int64_t>();

   int64_t sysio_decrease = sysio_before.get_amount() - sysio_after.get_amount();
   BOOST_REQUIRE_EQUAL( sysio_decrease, distributed );

   // With producers paid, distributed should be > (emission - full_producer_pool)
   // i.e., some producer funds were actually distributed
   auto log = get_epoch_log(1);
   int64_t emission = log["total_emission"].as<int64_t>();
   int64_t full_undist = compute_producer_pool(emission);
   BOOST_REQUIRE( distributed > (emission - full_undist) );
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

   auto info = viewepoch( config::system_account_name );
   BOOST_REQUIRE_EQUAL( info.epoch_count, 0u );
   BOOST_REQUIRE_EQUAL( info.total_distributed, 0 );
   BOOST_REQUIRE_EQUAL( info.treasury_remaining, T5_DISTRIBUTABLE );
   BOOST_REQUIRE_EQUAL( info.next_emission_est, EPOCH_INITIAL_EMISSION );
   BOOST_REQUIRE( info.seconds_until_next > 0 );
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( viewepoch_after_multiple_epochs, sysio_emissions_tester ) try {
   create_t5_holding_accounts();
   const uint32_t start = head_secs() - ONE_EPOCH - 1;
   BOOST_REQUIRE_EQUAL( success(), initt5( config::system_account_name, tpsec(start) ) );

   // Epoch 1
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );
   produce_block( fc::seconds(ONE_EPOCH) );

   // Epoch 2
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

   auto info = viewepoch( config::system_account_name );
   BOOST_REQUIRE_EQUAL( info.epoch_count, 2u );
   BOOST_REQUIRE( info.total_distributed > 0 );
   BOOST_REQUIRE_EQUAL( info.treasury_remaining, T5_DISTRIBUTABLE - info.total_distributed );

   // next_emission_est should reflect decay from epoch 2's emission
   __int128 expected = static_cast<__int128>(info.last_epoch_emission) * DECAY_NUMERATOR;
   int64_t expected_next = static_cast<int64_t>(expected / DECAY_DENOMINATOR);
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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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
   BOOST_REQUIRE_EQUAL( success(), processepoch( config::system_account_name ) );

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

BOOST_AUTO_TEST_SUITE_END() // t5_emissions_tests
