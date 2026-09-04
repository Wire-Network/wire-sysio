#include <test_contracts.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include "sysio.system_tester.hpp"
#include <contracts.hpp>
#include <sysio/opp/opp.hpp>
#include <fc/variant_object.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <string>
#include <tuple>
#include <type_traits>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;
using namespace std;

using mvo = fc::mutable_variant_object;

using sysio::chain::make_kv_scoped_key;
using sysio::chain::compute_table_id;

constexpr account_name ROA = "sysio.roa"_n;
constexpr uint64_t NETWORK_GEN = 0;

class sysio_roa_tester : public tester {
public:

   sysio_roa_tester() {
      produce_blocks( 2 );

      create_accounts( { "alice"_n, "bob"_n, "carol"_n, "darcy"_n }, false, false, false, false );
      produce_blocks( 2 );

      produce_blocks();

      const auto* accnt = control->find_account_metadata( ROA );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));

      // Deploy sysio.system, init, and set default emission config
      set_code( config::system_account_name, test_contracts::sysio_system_wasm() );
      set_abi ( config::system_account_name, test_contracts::sysio_system_abi() );
      produce_blocks(1);

      base_tester::push_action(config::system_account_name, "init"_n,
                               config::system_account_name, mutable_variant_object()
                               ("version", 0)
                               ("core", symbol(CORE_SYMBOL).to_string()));
      produce_blocks(1);

      // Load system ABI serializer for setemitcfg
      {
         const auto* sys_accnt = control->find_account_metadata( config::system_account_name );
         BOOST_REQUIRE( sys_accnt != nullptr );
         abi_def sys_abi;
         BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(sys_accnt->abi, sys_abi), true);
         sys_abi_ser.set_abi(sys_abi, abi_serializer::create_yield_function(abi_serializer_max_time));
      }

      setup_emission_config();
   }

   void setup_emission_config() {
      auto cfg = mvo()
         ("t1_allocation",          int64_t(7500000000000000))
         ("t2_allocation",          int64_t(1000000000000000))
         ("t3_allocation",          int64_t(100000000000000))
         ("t1_duration",            uint32_t(12u * 30u * 24u * 3600u))
         ("t2_duration",            uint32_t(24u * 30u * 24u * 3600u))
         ("t3_duration",            uint32_t(36u * 30u * 24u * 3600u))
         ("min_claimable",          int64_t(10000000000))
         ("t5_distributable",       int64_t(375000000000000000LL))
         ("t5_floor",               int64_t(125000000000000000LL))
         ("target_annual_decay_bps", uint16_t(6940))
         ("annual_initial_emission", int64_t(563150000000000LL * 365))
         ("annual_max_emission",     int64_t(3000000000000000LL * 365))
         ("annual_min_emission",     int64_t(100000000000000LL * 365))
         ("compute_bps",            uint16_t(4000))
         ("capex_bps",              uint16_t(2000))
         ("governance_bps",         uint16_t(1000))
         ("producer_bps",           uint16_t(7000))
         ("batch_op_bps",           uint16_t(3000))
         ("standby_end_rank",       uint32_t(28))
         ("epoch_log_retention_count", uint32_t(8640))("pay_cadence_epochs", uint16_t(1));

      auto act_type = sys_abi_ser.get_action_type("setemitcfg"_n);
      action act;
      act.account = config::system_account_name;
      act.name = "setemitcfg"_n;
      act.authorization = {{config::system_account_name, config::active_name}};
      act.data = sys_abi_ser.variant_to_binary(act_type, mvo()("cfg", cfg),
         abi_serializer::create_yield_function(abi_serializer_max_time));

      signed_transaction trx;
      trx.actions.push_back(std::move(act));
      set_transaction_headers(trx);
      trx.sign(get_private_key(config::system_account_name, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
   }

   action_result push_action( const account_name& signer, const action_name &name, const variant_object &data ) {
      string action_type_name = abi_ser.get_action_type(name);

      action act;
      act.account = ROA;
      act.name    = name;
      act.data    = abi_ser.variant_to_binary( action_type_name, data, abi_serializer::create_yield_function(abi_serializer_max_time) );

      return base_tester::push_action( std::move(act), signer.to_uint64_t() );
   }

   transaction_trace_ptr push_paid_action( const account_name& signer, const action_name &name, const variant_object &data ) {
      return base_tester::push_action( ROA, name,
         vector<permission_level>{{signer, "sysio.payer"_n},{signer, "active"_n}},
         data);
   }

   /// Push a sysio.roa action WITHOUT an explicit billed CPU time, so
   /// transaction_context::verify_init_subjective_billing() actually runs. The ordinary tester
   /// helpers pass DEFAULT_BILLED_CPU_TIME_US, which sets explicit_billed_cpu_time and makes that
   /// check return on its first line -- meaning the subjective admission path is invisible to them.
   transaction_trace_ptr push_subjective_action( const account_name& signer, const action_name& name,
                                                 const variant_object& data ) {
      signed_transaction trx;
      trx.actions.emplace_back( get_action( ROA, name,
                                            vector<permission_level>{{signer, config::active_name}},
                                            data ) );
      set_transaction_headers( trx );
      trx.sign( get_private_key( signer, "active" ), control->get_chain_id() );
      return push_transaction( trx, fc::time_point::maximum(), 0 /* no explicit billed cpu */ );
   }

   fc::variant get_nodeowner( account_name acc )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(static_cast<uint64_t>(NETWORK_GEN), acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id(name("nodeowners").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "nodeowners", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   // Read the nodeownerreg audit row (status/reason/tier) written by nodeownreg's soft-fail path.
   fc::variant get_nodeownerreg( account_name acc )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(static_cast<uint64_t>(NETWORK_GEN), acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id(name("nodeownerreg").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "nodeownerreg", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   fc::variant get_sponsorship( account_name acc, account_name nonce)
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(acc, nonce.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id("sponsors"_n.to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "sponsor", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   uint64_t get_sponsor_count( account_name acc )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(static_cast<uint64_t>(NETWORK_GEN), acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id(name("sponsorcount").to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            auto record = abi_ser.binary_to_variant("sponsorcount", data, abi_serializer::create_yield_function(abi_serializer_max_time));
            return record["count"].as<uint64_t>();
         }
      }
      return 0;
   }

   fc::variant get_reslimit( account_name acc )
   {
      // reslimit is kv::table (unscoped) — key is [owner:8B BE]
      const auto& db = control->db();
      char key_buf[chain::kv_pri_key_size];
      chain::kv_encode_be64(key_buf, acc.to_uint64_t());
      std::string_view key_sv(key_buf, chain::kv_pri_key_size);
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id("reslimit"_n.to_uint64_t()), key_sv));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "reslimit", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   fc::variant get_policy( account_name acc, account_name owner )
   {
      const auto& db = control->db();
      auto key = make_kv_scoped_key(owner, acc.to_uint64_t());
      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      auto it = kv_idx.find(boost::make_tuple(ROA, compute_table_id("policies"_n.to_uint64_t()), key.to_string_view()));
      if (it != kv_idx.end()) {
         const vector<char> data(it->value.data(), it->value.data() + it->value.size());
         if (!data.empty()) {
            return abi_ser.binary_to_variant( "policies", data, abi_serializer::create_yield_function(abi_serializer_max_time) );
         }
      }
      return fc::variant();
   }

   transaction_trace_ptr newuser( account_name creator, name nonce, fc::crypto::public_key pubkey)
   {
      return push_paid_action( creator, "newuser"_n, mvo()
           ( "creator", creator)
           ( "nonce", nonce )
           ( "pubkey", pubkey )
      );
   }

   account_name create_newuser(account_name creator) {
      static name nonce{1u};
      nonce = name{nonce.to_uint64_t() + 1};

      auto newuser_result = newuser(creator, nonce, get_public_key(creator, "active"));
      BOOST_REQUIRE(newuser_result && newuser_result->action_traces.size() > 0);
      auto newuser_action_trace = newuser_result->action_traces[0];
      auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
      return new_name;
   }

   action_result regnodeowner( account_name owner, uint8_t tier )
   {
      return push_action(ROA, "forcereg"_n, mvo()
           ("owner", owner)
           ("tier", tier)
      );
   }

   action_result extend_policy( account_name owner, account_name issuer, uint32_t new_time_block ) {
       return push_action(issuer, "extendpolicy"_n, mvo()
            ("owner", owner)
            ("issuer", issuer)
            ("new_time_block", new_time_block)
       );
   }

   abi_serializer abi_ser;
   abi_serializer sys_abi_ser;
};

BOOST_AUTO_TEST_SUITE(sysio_roa_tests)

BOOST_FIXTURE_TEST_CASE( newuser_happycase_test, sysio_roa_tester ) try {

   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);

   auto empty = get_sponsorship("alice"_n, "nonce1"_n);
   BOOST_REQUIRE_EQUAL(empty.is_null(), true);
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice"_n));

   auto newuser_result = newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   auto newuser_action_trace = newuser_result->action_traces[0];
   BOOST_REQUIRE_EQUAL(newuser_action_trace.act.name, "newuser"_n);
   BOOST_REQUIRE_EQUAL(newuser_action_trace.receiver, ROA);
   BOOST_REQUIRE_EQUAL(newuser_action_trace.act.account, ROA);
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   BOOST_REQUIRE_NE(""_n, new_name);

   auto newaccount_action_trace = newuser_result->action_traces[1];
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.act.name, "newaccount"_n);
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.receiver, "sysio"_n);
   BOOST_REQUIRE_EQUAL(newaccount_action_trace.act.account, "sysio"_n);
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));
   auto sponsorship = get_sponsorship("alice"_n, "nonce1"_n);
   BOOST_REQUIRE_EQUAL(sponsorship.is_null(), false);
   BOOST_REQUIRE_NE(""_n, sponsorship["username"].as<name>());
   BOOST_REQUIRE_EQUAL(sponsorship["username"].as<name>(), new_name);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_twice_test, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice"_n));

   auto newuser_result = newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active"));
   BOOST_REQUIRE_EQUAL(2, newuser_result->action_traces.size());
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));

   // second attempt with same nonce should fail
   BOOST_REQUIRE_EXCEPTION(
      newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active")),
      sysio_assert_message_exception, sysio_assert_message_is("Sponsor entry for this nonce already exists"));

   // but with a different nonce, it creates a new unique account
   auto newuser_result2 = newuser("alice"_n, "nonce2"_n, get_public_key("alice"_n, "active"));
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(2, get_sponsor_count("alice"_n));

   auto newuser_action_trace = newuser_result->action_traces[0];
   auto newuser_action_trace2 = newuser_result2->action_traces[0];
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   auto new_name2 = fc::raw::unpack<name>(newuser_action_trace2.return_value);
   BOOST_REQUIRE_NE(new_name, new_name2);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_creator_permission_test, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 2);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   // bob is not a node owner, so he cannot create users
   BOOST_REQUIRE_EXCEPTION(
           newuser("bob"_n, "nonce1"_n, get_public_key("alice"_n, "active")),
           sysio_assert_message_exception, sysio_assert_message_is("Creator is not a registered tier-1 node owner"));

   // alice is only a tier 2 owner, so she cannot create users
   BOOST_REQUIRE_EXCEPTION(
           newuser("alice"_n, "nonce1"_n, get_public_key("alice"_n, "active")),
           sysio_assert_message_exception, sysio_assert_message_is("Creator is not a registered tier-1 node owner"));

   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("alice"_n));
   BOOST_REQUIRE_EQUAL(0, get_sponsor_count("bob"_n));

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( newuser_multiple_creators_test, sysio_roa_tester ) try {
   auto result = regnodeowner("alice"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   result = regnodeowner("bob"_n, 1);
   BOOST_REQUIRE_EQUAL(success(), result);
   produce_blocks(1);

   auto alice_owner = get_nodeowner("alice"_n);
   BOOST_REQUIRE_EQUAL(alice_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(alice_owner["tier"].as<uint32_t>(), 1);

   auto bob_owner = get_nodeowner("bob"_n);
   BOOST_REQUIRE_EQUAL(bob_owner.is_null(), false);
   BOOST_REQUIRE_EQUAL(bob_owner["tier"].as<uint32_t>(), 1);

   auto same_nonce = "anynonce"_n;
   auto newuser_result = newuser("alice"_n, same_nonce, get_public_key("alice"_n, "active"));
   auto newuser_result2 = newuser("bob"_n, same_nonce, get_public_key("bob"_n, "active"));
   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("bob"_n));

   auto newuser_action_trace = newuser_result->action_traces[0];
   auto newuser_action_trace2 = newuser_result2->action_traces[0];
   auto new_name = fc::raw::unpack<name>(newuser_action_trace.return_value);
   auto new_name2 = fc::raw::unpack<name>(newuser_action_trace2.return_value);
   BOOST_REQUIRE_NE(new_name, new_name2);

} FC_LOG_AND_RETHROW()

// With the <prefix>.<random> model the prefix is the creator, so the same nonce across DIFFERENT
// creators no longer collides — each name differs by prefix. All four succeed with distinct names.
BOOST_FIXTURE_TEST_CASE( newuser_same_nonce_distinct_creators, sysio_roa_tester ) try {
   for (auto owner : {"alice"_n, "bob"_n, "carol"_n, "darcy"_n})
      BOOST_REQUIRE_EQUAL(success(), regnodeowner(owner, 1));
   produce_blocks(1);

   auto same_nonce = "inauspicious"_n;
   auto ra = newuser("alice"_n, same_nonce, get_public_key("alice"_n, "active"));
   auto rb = newuser("bob"_n,   same_nonce, get_public_key("bob"_n,   "active"));
   auto rc = newuser("carol"_n, same_nonce, get_public_key("carol"_n, "active"));
   auto rd = newuser("darcy"_n, same_nonce, get_public_key("darcy"_n, "active"));
   produce_blocks(1);

   auto na = fc::raw::unpack<name>(ra->action_traces[0].return_value);
   auto nb = fc::raw::unpack<name>(rb->action_traces[0].return_value);
   auto nc = fc::raw::unpack<name>(rc->action_traces[0].return_value);
   auto nd = fc::raw::unpack<name>(rd->action_traces[0].return_value);

   // names carry the creator prefix and are all distinct
   BOOST_REQUIRE_EQUAL(na.to_string().substr(0, 6), std::string("alice."));
   BOOST_REQUIRE_NE(na, nb); BOOST_REQUIRE_NE(na, nc); BOOST_REQUIRE_NE(na, nd);
   BOOST_REQUIRE_NE(nb, nc); BOOST_REQUIRE_NE(nb, nd); BOOST_REQUIRE_NE(nc, nd);

   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("alice"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("bob"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("carol"_n));
   BOOST_REQUIRE_EQUAL(1, get_sponsor_count("darcy"_n));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( verify_ram, sysio_roa_tester ) try {
   // system contract + init + emission config already done in base constructor

   // roa has been activated with NODE_DADDY as a node owner
   // Accounts already created with ROA policy { "alice"_n, "bob"_n, "carol"_n, "darcy"_n }
   int64_t ram; int64_t net; int64_t cpu;
   control->get_resource_limits_manager().get_account_limits( "sysio"_n, ram, net, cpu );
   const int64_t initial_sysio_ram = ram;
   auto r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == ram);
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // just itself
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram); // sysio.acct itself

   // create all node owners
   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1 ; i < node_owners.size() ; i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
      control->get_resource_limits_manager().get_account_limits( node_owners[i], ram, net, cpu );
      BOOST_TEST(ram == newaccount_ram + 80l*104); // creation gift (newaccount_ram) kept + personal (80 units * 104 bytes_per_unit)
      BOOST_TEST(net == 500);
      BOOST_TEST(cpu == 500);
   }
   produce_block();

   // verify initial conditions of ROA accounts
   control->get_resource_limits_manager().get_account_limits( "sysio"_n, ram, net, cpu );
   const int64_t roa_sysio_ram = initial_sysio_ram + 6281267200;
   BOOST_TEST(ram == roa_sysio_ram); // ram after all tier-1 nodeowners
   control->get_resource_limits_manager().get_account_limits( "sysio.roa"_n, ram, net, cpu );
   BOOST_TEST(ram == 157021280); // ram of roa itself
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // just itself

   // create a roa::newuser and verify resources
   auto newuser = create_newuser(node_owners[2]);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // newuser has gifted amount of ram
   BOOST_TEST(net == 0);
   BOOST_TEST(cpu == 0);
   r = get_reslimit(newuser);
   BOOST_TEST(r.is_null()); // no reslimit for newuser
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 2*newaccount_ram); // newuser and sysio.acct itself
   auto p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for newuser
   p = get_policy(newuser, node_owners[2]);
   BOOST_TEST(p.is_null()); // no policy for newuser
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-newaccount_ram); // sysio ram gifted to newuser; reflected in sysio.acct
   p = get_policy("sysio.acct"_n, "sysio"_n);
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0011 SYS");  // (newaccount_ram=1144 / bytes_per_unit=104) == 11

   // create another roa::newuser and verify resources
   auto newuser2 = create_newuser(node_owners[2]);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser2, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // newuser has gifted amount of ram
   BOOST_TEST(net == 0);
   BOOST_TEST(cpu == 0);
   r = get_reslimit(newuser2);
   BOOST_TEST(r.is_null()); // no reslimit for newuser
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser2, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for newuser
   p = get_policy(newuser2, node_owners[2]);
   BOOST_TEST(p.is_null()); // no policy for newuser
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram gifted to newuser & newuser2; reflected in sysio.acct
   p = get_policy("sysio.acct"_n, "sysio"_n);
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS");  // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22

   // Provide newuser a policy and verify resources
   add_roa_policy(node_owners[2], newuser, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000l*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000l);
   BOOST_TEST(cpu == 320000l);
   r = get_reslimit(newuser);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000l*104));
   BOOST_TEST(r["net_weight"].as_string() == "32.0000 SYS");
   BOOST_TEST(r["cpu_weight"].as_string() == "32.0000 SYS");
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself (nothing for policy)
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // policy provided by node_owners[2], not sysio
   p = get_policy(newuser, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "32.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "32.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "32.0000 SYS"); // new policy does not include the gifted RAM from sysio
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram not changed for a policy, same as before
   p = get_policy("sysio.acct"_n, "sysio"_n); // sysio.acct policy not changed for a user policy
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS"); // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22 (nothing for policy)

   // Expand policy and verify resources:     net           cpu            ram
   expand_roa_policy(node_owners[2], newuser, "5.0000 SYS", "10.0000 SYS", "15.0000 SYS", 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000l*104)+(150000l*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000+50000);
   BOOST_TEST(cpu == 320000+100000);
   r = get_reslimit(newuser);
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000l*104)+(150000l*104));
   BOOST_TEST(r["net_weight"].as_string() == "37.0000 SYS");
   BOOST_TEST(r["cpu_weight"].as_string() == "42.0000 SYS");
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself (nothing for policy)
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // policy provided by node_owners[2], not sysio
   p = get_policy(newuser, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "37.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "42.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "47.0000 SYS"); // policy does not include the gifted RAM from sysio
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram not changed for a policy, same as before
   p = get_policy("sysio.acct"_n, "sysio"_n); // sysio.acct policy not changed for a user policy
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS"); // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22 (nothing for policy)

   // Add policy from another node owner:     net           cpu            ram
   add_roa_policy(node_owners[3], newuser, "1.0000 SYS", "2.0000 SYS", "3.0000 SYS", 0, 0);
   produce_block();
   control->get_resource_limits_manager().get_account_limits( "sysio.acct"_n, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram); // resource_limits of sysio.acct is the same, ram is gifted to new account
   control->get_resource_limits_manager().get_account_limits( newuser, ram, net, cpu );
   BOOST_TEST(ram == newaccount_ram+(320000l*104)+(150000l*104)+(30000l*104)); // gifted ram plus the policy ram
   BOOST_TEST(net == 320000l+50000l+10000l);
   BOOST_TEST(cpu == 320000l+100000l+20000l);
   r = get_reslimit(newuser); // reflects both policies from node_owner 2 & 3
   BOOST_TEST(r["ram_bytes"].as_int64() == newaccount_ram+(320000l*104)+(150000l*104)+(30000l*104));
   BOOST_TEST(r["net_weight"].as_string() == "38.0000 SYS");
   BOOST_TEST(r["cpu_weight"].as_string() == "44.0000 SYS");
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == 3*newaccount_ram); // newuser, newuser2, and sysio.acct itself (nothing for policy)
   p = get_policy("sysio"_n, "sysio"_n);
   BOOST_TEST(p.is_null()); // no policy for sysio
   p = get_policy(newuser, "sysio"_n);
   BOOST_TEST(p.is_null()); // policy provided by node_owners[2], not sysio
   p = get_policy(newuser, node_owners[2]); // node owner 2
   BOOST_TEST(p["net_weight"].as_string() == "37.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "42.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "47.0000 SYS");
   p = get_policy(newuser, node_owners[3]); // node owner 3
   BOOST_TEST(p["net_weight"].as_string() == "1.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "2.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "3.0000 SYS");
   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == roa_sysio_ram-2*newaccount_ram); // sysio ram not changed for a policy, same as before
   p = get_policy("sysio.acct"_n, "sysio"_n); // sysio.acct policy not changed for a user policy
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "0.0022 SYS"); // 2*(newaccount_ram=1144 / bytes_per_unit=104) == 22 (nothing for policy)

   produce_block();

} FC_LOG_AND_RETHROW()

// The byte price must keep newaccount_ram evenly divisible: newuser/newnameduser convert the fixed
// newaccount_ram seed to policy units by integer division while moving the full newaccount_ram bytes,
// so an indivisible price under-records the sysio.acct policy. activateroa and setbyteprice share
// check_divisible_byte_price; exercise it here through setbyteprice (roa is already active, activated
// by the bootstrap with the divisible default 104).
BOOST_FIXTURE_TEST_CASE( byteprice_divisibility_guard, sysio_roa_tester ) try {
   BOOST_REQUIRE_EXCEPTION(
      base_tester::push_action(ROA, "setbyteprice"_n, ROA, mvo()("bytes_per_unit", newaccount_ram + 1)),
      sysio_assert_message_exception,
      sysio_assert_message_is("newaccount_ram needs to be evenly divisible to avoid dust"));

   // zero would divide-by-zero in the unit conversion; rejected up front.
   BOOST_REQUIRE_EXCEPTION(
      base_tester::push_action(ROA, "setbyteprice"_n, ROA, mvo()("bytes_per_unit", 0)),
      sysio_assert_message_exception,
      sysio_assert_message_is("bytes_per_unit must be positive"));

   // a divisible price is accepted (newaccount_ram % 104 == 0).
   base_tester::push_action(ROA, "setbyteprice"_n, ROA, mvo()("bytes_per_unit", 104));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( extend_policy_test, sysio_roa_tester ) try {
    auto result = regnodeowner("alice"_n, 1);
    BOOST_REQUIRE_EQUAL(success(), result);
    produce_blocks(1);

    result = regnodeowner("bob"_n, 1);
    BOOST_REQUIRE_EQUAL(success(), result);
    produce_blocks(1);

    auto p = get_policy("alice"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "alice");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["bytes_per_unit"].as_string() == "104");
    BOOST_TEST(p["time_block"].as_string() == "1");
    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "sysio");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["time_block"].as_string() == "4294967295");
    BOOST_TEST(p["ram_weight"].as_string() == "301.9840 SYS");

    // extend policy
    extend_policy("alice"_n, "alice"_n, 42);

    p = get_policy("alice"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "alice");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["time_block"].as_string() == "42");
    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["owner"].as_string() == "sysio");
    BOOST_TEST(p["issuer"].as_string() == "alice");
    BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["time_block"].as_string() == "4294967295");

    result = extend_policy("sysio"_n, "alice"_n, 42);
    BOOST_REQUIRE_EQUAL(error("assertion failure with message: Cannot reduce a policies existing time_block"), result);

    BOOST_CHECK_EXCEPTION(reduce_roa_policy("alice"_n, "sysio"_n, "0.0000 SYS", "0.0000 SYS", "500.0000 SYS", 0),
                          sysio_assert_message_exception,
                          sysio_assert_message_is("Cannot reduce policy before time_block"));

    expand_roa_policy("alice"_n, "sysio"_n, "0.0000 SYS", "0.0000 SYS", "500.0000 SYS", 0);

    p = get_policy("sysio"_n, "alice"_n);
    BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
    BOOST_TEST(p["ram_weight"].as_string() == "801.9840 SYS");

} FC_LOG_AND_RETHROW()

// Verifies that reducepolicy correctly decreases RAM bytes in the reslimit table.
BOOST_FIXTURE_TEST_CASE( reducepolicy_ram_accounting, sysio_roa_tester ) try {
   // system contract + init + emission config already done in base constructor

   // Register node owners (need 21 for ROA activation)
   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1; i < node_owners.size(); i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
   }
   produce_block();

   // Create a user and add a policy with time_block=0 (immediately reducible)
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "32.0000 SYS", "32.0000 SYS", "32.0000 SYS", 0, 0);
   produce_block();

   // Record pre-reduce state
   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   auto r = get_reslimit(user);
   int64_t pre_reslimit_ram = r["ram_bytes"].as_int64();

   // Verify initial state: newaccount_ram + policy RAM (320000 * 104 = 33,280,000)
   BOOST_TEST(pre_reslimit_ram == ram);
   BOOST_TEST(ram == (int64_t)(newaccount_ram + 320000l * 104));
   BOOST_TEST(net == 320000);
   BOOST_TEST(cpu == 320000);

   // Reduce the policy by half
   reduce_roa_policy(node_owners[2], user, "16.0000 SYS", "16.0000 SYS", "16.0000 SYS", 0);
   produce_block();

   // Verify post-reduce state
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   r = get_reslimit(user);
   int64_t post_reslimit_ram = r["ram_bytes"].as_int64();

   // C4 fix: reslimit ram_bytes should decrease, NOT nearly double
   // Expected: pre_reslimit_ram - (160000 * 104)
   int64_t expected_ram = pre_reslimit_ram - 160000l * 104;
   BOOST_TEST(post_reslimit_ram == expected_ram);
   BOOST_TEST(post_reslimit_ram == ram);  // reslimit table matches system limits
   BOOST_TEST(post_reslimit_ram < pre_reslimit_ram);

   // Verify NET/CPU decreased correctly
   BOOST_TEST(net == 160000);
   BOOST_TEST(cpu == 160000);

   // Verify policy updated
   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "16.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "16.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "16.0000 SYS");

} FC_LOG_AND_RETHROW()

// Verifies that reducing an entire policy to zero correctly removes it and
// that a second reducepolicy on a different policy also works correctly.
BOOST_FIXTURE_TEST_CASE( reducepolicy_full_then_second, sysio_roa_tester ) try {
   // system contract + init + emission config already done in base constructor

   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1; i < node_owners.size(); i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
   }
   produce_block();

   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Add two policies from different node owners
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   add_roa_policy(node_owners[3], user, "20.0000 SYS", "20.0000 SYS", "20.0000 SYS", 0, 0);
   produce_block();

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   // newaccount_ram + (100000+200000)*104
   BOOST_TEST(ram == (int64_t)(newaccount_ram + 100000l * 104 + 200000l * 104));
   BOOST_TEST(net == 100000 + 200000);
   BOOST_TEST(cpu == 100000 + 200000);

   // Fully reduce the first policy
   reduce_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   auto r = get_reslimit(user);

   // Only the second policy's RAM remains plus newaccount_ram
   BOOST_TEST(ram == (int64_t)(newaccount_ram + 200000l * 104));
   BOOST_TEST(r["ram_bytes"].as_int64() == ram);
   BOOST_TEST(net == 200000);
   BOOST_TEST(cpu == 200000);

   // First policy should be removed
   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p.is_null());

   // Second policy still intact
   p = get_policy(user, node_owners[3]);
   BOOST_TEST(p["net_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "20.0000 SYS");

   // Now reduce the second policy partially
   reduce_roa_policy(node_owners[3], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   r = get_reslimit(user);

   BOOST_TEST(ram == (int64_t)(newaccount_ram + 150000l * 104));
   BOOST_TEST(r["ram_bytes"].as_int64() == ram);
   BOOST_TEST(net == 150000);
   BOOST_TEST(cpu == 150000);

} FC_LOG_AND_RETHROW()

// Verifies that creating multiple users correctly decreases sysio's reslimit RAM.
BOOST_FIXTURE_TEST_CASE( newuser_sysio_ram_decreases, sysio_roa_tester ) try {
   // system contract + init + emission config already done in base constructor

   std::array<account_name, 21> node_owners = { NODE_DADDY };
   for (size_t i = 1; i < node_owners.size(); i++) {
      std::string n = std::string("nodeowner").append(1, 'a' + i);
      node_owners[i] = account_name(n);
      create_accounts({node_owners[i]}, false, false, false, false);
      register_node_owner(node_owners[i], 1);
   }
   produce_block();

   // Record sysio's initial RAM
   auto r = get_reslimit("sysio"_n);
   int64_t sysio_ram_before = r["ram_bytes"].as_int64();
   BOOST_TEST(sysio_ram_before > 0);

   // Create 5 users - each should subtract newaccount_ram from sysio
   for (int i = 0; i < 5; i++) {
      create_newuser(node_owners[2]);
      produce_block();
   }

   r = get_reslimit("sysio"_n);
   int64_t sysio_ram_after = r["ram_bytes"].as_int64();

   // Sysio's reslimit should have decreased by exactly 5 * newaccount_ram
   BOOST_TEST(sysio_ram_after == sysio_ram_before - 5 * (int64_t)newaccount_ram);
   BOOST_TEST(sysio_ram_after > 0);  // should still be positive

   // sysio.acct's reslimit should have increased by 5 * newaccount_ram (plus its own)
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == (int64_t)((5 + 1) * newaccount_ram));  // 5 users + sysio.acct itself

} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Helper fixture: sysio_roa_tester with system contract + 21 node owners
// ---------------------------------------------------------------------------
class sysio_roa_full_tester : public sysio_roa_tester {
public:
   std::array<account_name, 21> node_owners;

   sysio_roa_full_tester() {
      // system contract + init + emission config already done in base constructor

      node_owners[0] = NODE_DADDY;
      for (size_t i = 1; i < node_owners.size(); i++) {
         std::string n = std::string("nodeowner").append(1, 'a' + i);
         node_owners[i] = account_name(n);
         create_accounts({node_owners[i]}, false, false, false, false);
         register_node_owner(node_owners[i], 1);
      }
      produce_block();
   }

   /// Assert an onboarded session is actually usable: the linkauth resolves sysio::reqauth to
   /// exactly the session permission, and the session key alone satisfies the resulting
   /// authorization. Without this the onboarding cases would pass on a linkauth that had become a
   /// no-op or named the wrong requirement, since permission existence and hierarchy say nothing
   /// about the link.
   ///
   /// Built as a bare action rather than push_reqauth because sysio carries the system contract in
   /// this fixture, so reqauth is absent from its ABI; reqauth's payload is just the account name.
   void check_session_is_live( account_name user, account_name issuer ) {
      const auto& authmgr = control->get_authorization_manager();

      // Resolve the requirement explicitly. check_authorization alone is not sufficient: a link to
      // sysio.any makes lookup_minimum_permission return empty, which skips the relevance check
      // entirely, so the key check below would bless an overbroad requirement. Only a link naming a
      // permission the child does not satisfy -- active, say -- would throw on its own.
      const auto minimum = authmgr.lookup_minimum_permission( user, config::system_account_name,
                                                              "reqauth"_n );
      BOOST_REQUIRE( minimum.has_value() );
      BOOST_REQUIRE( *minimum == "session"_n );

      action reqauth_act;
      reqauth_act.account       = config::system_account_name;
      reqauth_act.name          = "reqauth"_n;
      reqauth_act.authorization = { permission_level{user, "session"_n} };
      reqauth_act.data          = fc::raw::pack( user );

      BOOST_CHECK_NO_THROW( authmgr.check_authorization(
         { reqauth_act }, { get_public_key( issuer, "session" ) } ) );
   }
};

// Session-key onboarding: what can and cannot share a transaction.
//
// RAM is verified once at transaction finalize rather than per action, so a ROA grant and the
// spending of that grant may ride in the same transaction -- nothing can land in between and
// consume it.
//
// Authorization, by contrast, is decided for every TOP-LEVEL action before any of them executes
// (controller checks the whole action list, then calls trx_context.exec()). Two top-level
// updateauth actions therefore cannot create a permission and its child together: the child's
// parent lookup runs while the parent does not yet exist.
//
// That restriction is specific to the top-level path. An INLINE action is authorized during
// execution, by which time the parent exists -- see session_onboarding_single_transaction_via_inline
// for the one-transaction shape a contract-managed onboarding can actually use.
BOOST_FIXTURE_TEST_CASE( session_onboarding_grant_and_setup, sysio_roa_full_tester ) try {
   const auto issuer = node_owners[2];
   const auto sessmgr = "sessmgr"_n;   // stands in for the dapp's reaper contract
   create_accounts( {sessmgr}, false, false, false, false );
   const auto user   = create_newuser( issuer );
   produce_block();

   // create_newuser seeds the account with the creator's active key, so one signature satisfies
   // both the issuer and the user authorizations below.
   const auto signing_key = get_private_key( issuer, "active" );
   const vector<permission_level> as_user{{user, config::active_name}};

   const authority gate_auth( 1, {}, {{{sessmgr, config::sysio_code_name}, 1}} );
   const authority session_auth( 1, {{get_public_key( issuer, "session" ), 1}}, {} );

   auto make_gate = [&]{ return updateauth{ .account = user, .permission = "sessgate"_n,
                                            .parent  = config::active_name, .auth = gate_auth }; };
   auto make_child = [&]{ return updateauth{ .account = user, .permission = "session"_n,
                                             .parent  = "sessgate"_n, .auth = session_auth }; };
   auto addpolicy_action = [&]{
      return get_action( config::roa_account_name, "addpolicy"_n,
                         vector<permission_level>{{issuer, config::active_name}},
                         mvo()( "owner", user )( "issuer", issuer )
                              ( "net_weight", "0.0000 SYS" )( "cpu_weight", "0.0000 SYS" )
                              ( "ram_weight", "0.0010 SYS" )   // 10 units of RAM
                              ( "time_block", 0 )( "network_gen", 0 ) );
   };
   auto sign_and_push = [&]( signed_transaction& trx ) {
      set_transaction_headers( trx );
      trx.sign( signing_key, control->get_chain_id() );
      return push_transaction( trx );
   };

   const auto& authmgr = control->get_authorization_manager();

   // Without a grant, even the code-only parent alone exceeds the creation gift.
   {
      signed_transaction trx;
      trx.actions.emplace_back( as_user, make_gate() );
      BOOST_CHECK_THROW( sign_and_push( trx ), ram_usage_exceeded );
   }
   produce_block();
   BOOST_REQUIRE( authmgr.find_permission( {user, "sessgate"_n} ) == nullptr );

   // Parent and child cannot be created together, even with the grant present: the child's
   // authorization check resolves its parent before the parent's action has run.
   {
      signed_transaction trx;
      trx.actions.emplace_back( addpolicy_action() );
      trx.actions.emplace_back( as_user, make_gate() );
      trx.actions.emplace_back( as_user, make_child() );
      BOOST_CHECK_THROW( sign_and_push( trx ), permission_query_exception );
   }
   produce_block();
   BOOST_REQUIRE( authmgr.find_permission( {user, "sessgate"_n} ) == nullptr ); // whole trx reverted

   // Grant and parent together: atomic, so the grant cannot be intercepted and spent elsewhere.
   {
      signed_transaction trx;
      trx.actions.emplace_back( addpolicy_action() );
      trx.actions.emplace_back( as_user, make_gate() );
      sign_and_push( trx );
   }
   produce_block();
   BOOST_REQUIRE( authmgr.find_permission( {user, "sessgate"_n} ) != nullptr );

   // Child and its link follow in a second transaction, paid for out of the same grant.
   {
      signed_transaction trx;
      trx.actions.emplace_back( as_user, make_child() );
      trx.actions.emplace_back( as_user, linkauth{ user, "sysio"_n, "reqauth"_n, "session"_n } );
      sign_and_push( trx );
   }
   produce_block();
   BOOST_REQUIRE( authmgr.find_permission( {user, "session"_n} ) != nullptr );

   // The permissions existing is not the onboarding result -- the point is a session key that can
   // actually act. Resolve the link and check the session key satisfies it, so a linkauth that
   // became a no-op or named the wrong requirement fails here rather than passing silently.
   // (push_reqauth is unavailable: sysio carries the system contract here, not bios.)
   check_session_is_live( user, issuer );
} FC_LOG_AND_RETHROW()

// The top-level restriction above is not a property of transactions, only of top-level actions.
// Inline actions are authorized during execution, after earlier actions have applied, so a contract
// holding a seat in the gate can create the child in the same transaction that creates the gate.
// The trailing top-level linkauth is fine too: its precheck resolves the currently linked
// permission (none), and only apply_sysio_linkauth requires the requirement to exist, by which
// point the inline has created it.
//
// This is the shape a contract-managed onboarding should use -- it never leaves an unspent grant
// exposed between transactions.
BOOST_FIXTURE_TEST_CASE( session_onboarding_single_transaction_via_inline, sysio_roa_full_tester ) try {
   const auto issuer  = node_owners[3];
   const auto sessmgr = "sessmgr2"_n;
   create_accounts( {sessmgr}, false, false, false, false );
   // the forwarder needs RAM to hold its code, and NET/CPU because in Wire the contract account
   // is billed for its own execution rather than the caller
   add_roa_policy( issuer, sessmgr, "8.0000 SYS", "8.0000 SYS", "8.0000 SYS", 0, 0 );
   produce_block();
   set_code( sessmgr, system_contracts::testing::test_contracts::sendinline_wasm() );
   set_abi( sessmgr, system_contracts::testing::test_contracts::sendinline_abi().data() );
   const auto user = create_newuser( issuer );
   produce_block();

   const auto signing_key = get_private_key( issuer, "active" );
   const authority gate_auth( 1, {}, {{{sessmgr, config::sysio_code_name}, 1}} );
   const authority session_auth( 1, {{get_public_key( issuer, "session" ), 1}}, {} );

   signed_transaction trx;
   // 1. grant
   trx.actions.emplace_back( get_action( config::roa_account_name, "addpolicy"_n,
                                         vector<permission_level>{{issuer, config::active_name}},
                                         mvo()( "owner", user )( "issuer", issuer )
                                              ( "net_weight", "0.0000 SYS" )( "cpu_weight", "0.0000 SYS" )
                                              ( "ram_weight", "0.0010 SYS" )
                                              ( "time_block", 0 )( "network_gen", 0 ) ) );
   // 2. top-level: the code-only gate
   trx.actions.emplace_back( vector<permission_level>{{user, config::active_name}},
                             updateauth{ .account = user, .permission = "sessgate"_n,
                                         .parent  = config::active_name, .auth = gate_auth } );
   // 3. inline: the contract creates the child under the gate, authorized at execution time
   trx.actions.emplace_back( get_action( sessmgr, "send"_n,
                                         vector<permission_level>{{issuer, config::active_name}},
                                         mvo()( "contract", config::system_account_name )
                                              ( "action_name", "updateauth" )
                                              ( "auths", std::vector<permission_level>{{user, "sessgate"_n}} )
                                              ( "payload", fc::raw::pack(
                                                   updateauth{ .account = user, .permission = "session"_n,
                                                               .parent  = "sessgate"_n, .auth = session_auth } ) ) ) );
   // 4. top-level: link the child that step 3 will have created
   trx.actions.emplace_back( vector<permission_level>{{user, config::active_name}},
                             linkauth{ user, "sysio"_n, "reqauth"_n, "session"_n } );

   set_transaction_headers( trx );
   trx.sign( signing_key, control->get_chain_id() );
   push_transaction( trx );
   produce_block();

   const auto& authmgr = control->get_authorization_manager();
   const auto* gate  = authmgr.find_permission( {user, "sessgate"_n} );
   const auto* child = authmgr.find_permission( {user, "session"_n} );
   BOOST_REQUIRE( gate != nullptr );
   BOOST_REQUIRE( child != nullptr );
   BOOST_REQUIRE( child->parent == gate->id );

   // As above, the onboarding result is a usable session key, not merely two permissions and a
   // hierarchy. The trailing linkauth is the part most easily broken without any of the structural
   // assertions noticing, so resolve it and check the session key satisfies it.
   check_session_is_live( user, issuer );
} FC_LOG_AND_RETHROW()

// The contract cannot pay for a user's permission RAM, in either of the two ways one might try.
//
// apply_sysio_updateauth and its siblings call add_ram_usage against the permission's OWNER with no
// payer parameter, so a well-funded contract driving the change via an inline action still leaves
// the charge on the user. And the sysio.payer mechanism cannot be pointed at these actions at all:
// the native auth actions require exactly one declared authorization, so a payer role cannot even
// be attached.
BOOST_FIXTURE_TEST_CASE( contract_cannot_pay_for_permission_ram, sysio_roa_full_tester ) try {
   const auto issuer  = node_owners[4];
   const auto sessmgr = "sessmgr3"_n;
   create_accounts( {sessmgr}, false, false, false, false );
   add_roa_policy( issuer, sessmgr, "8.0000 SYS", "8.0000 SYS", "8.0000 SYS", 0, 0 );
   produce_block();
   set_code( sessmgr, system_contracts::testing::test_contracts::sendinline_wasm() );
   set_abi( sessmgr, system_contracts::testing::test_contracts::sendinline_abi().data() );

   const auto user = create_newuser( issuer );
   // Room to spare, so any failure below is about who pays rather than affordability.
   add_roa_policy( issuer, user, "0.0000 SYS", "0.0000 SYS", "0.0100 SYS", 0, 0 );
   produce_block();

   const auto signing_key = get_private_key( issuer, "active" );
   const authority gate_auth( 1, {}, {{{sessmgr, config::sysio_code_name}, 1}} );

   // The gate, so the contract has a seat to act through.
   set_authority( user, "sessgate"_n, gate_auth, config::active_name,
                  { permission_level{user, config::active_name} }, { signing_key } );
   produce_block();

   auto& rm = control->get_mutable_resource_limits_manager();
   const int64_t user_before = rm.get_account_ram_usage( user );
   const int64_t mgr_before  = rm.get_account_ram_usage( sessmgr );

   // A richly funded contract creates the child through its seat. The charge still lands on the user.
   const authority session_auth( 1, {{get_public_key( issuer, "session" ), 1}}, {} );
   base_tester::push_action( sessmgr, "send"_n, issuer, mvo()
                   ( "contract", config::system_account_name )
                   ( "action_name", "updateauth" )
                   ( "auths", std::vector<permission_level>{{user, "sessgate"_n}} )
                   ( "payload", fc::raw::pack( updateauth{ .account = user, .permission = "session"_n,
                                                           .parent  = "sessgate"_n, .auth = session_auth } ) ) );
   produce_block();

   BOOST_TEST( rm.get_account_ram_usage( user ) > user_before );      // the owner pays
   BOOST_TEST( rm.get_account_ram_usage( sessmgr ) == mgr_before );   // the contract does not

   // Nor can a payer role be attached. Each of the four native auth actions carries its own
   // auths.size() == 1 guard, so exercising only updateauth would leave a regression in any of the
   // other three invisible while making sysio.payer attachable on that path. Two independent
   // barriers, checked across all four.
   const authority spare_auth( 1, {{get_public_key( issuer, "spare" ), 1}}, {} );

   // Pre-state, so each action below is otherwise valid and the rejection is about the payer rather
   // than about a missing permission or link.
   set_authority( user, "paydel"_n, spare_auth, config::active_name,
                  { permission_level{user, config::active_name} }, { signing_key } );
   set_authority( user, "payperm"_n, spare_auth, config::active_name,
                  { permission_level{user, config::active_name} }, { signing_key } );
   {
      // link_authority() takes no keys and would sign as get_private_key(user, "active");
      // create_newuser seeded this account with the creator's key, so sign explicitly.
      signed_transaction link_trx;
      link_trx.actions.emplace_back( vector<permission_level>{{user, config::active_name}},
                                     linkauth{ user, config::system_account_name,
                                               "reqauth"_n, "payperm"_n } );
      set_transaction_headers( link_trx );
      link_trx.sign( signing_key, control->get_chain_id() );
      push_transaction( link_trx );
   }
   produce_block();

   using add_action_fn = std::function<void( signed_transaction&, const vector<permission_level>& )>;
   const std::vector<std::pair<std::string, add_action_fn>> cases = {
      { "updateauth action should only have one declared authorization",
        [&]( signed_transaction& t, const vector<permission_level>& a ) {
           t.actions.emplace_back( a, updateauth{ .account = user, .permission = "paynew"_n,
                                                  .parent = config::active_name, .auth = spare_auth } ); } },
      { "deleteauth action should only have one declared authorization",
        [&]( signed_transaction& t, const vector<permission_level>& a ) {
           t.actions.emplace_back( a, deleteauth{ user, "paydel"_n } ); } },
      { "link action should only have one declared authorization",
        [&]( signed_transaction& t, const vector<permission_level>& a ) {
           t.actions.emplace_back( a, linkauth{ user, config::system_account_name,
                                                "reqauth2"_n, "payperm"_n } ); } },
      { "unlink action should only have one declared authorization",
        [&]( signed_transaction& t, const vector<permission_level>& a ) {
           t.actions.emplace_back( a, unlinkauth{ user, config::system_account_name, "reqauth"_n } ); } },
   };

   for( const auto& entry : cases ) {
      const std::string& expected   = entry.first;
      const add_action_fn& add_action = entry.second;

      // 1. validate_referenced_accounts requires the payer to also carry a real authorization on the
      //    same action, so naming the contract as payer alone is rejected outright.
      {
         signed_transaction trx;
         add_action( trx, { {sessmgr, config::sysio_payer_name}, {user, config::active_name} } );
         set_transaction_headers( trx );
         trx.sign( signing_key, control->get_chain_id() );
         trx.sign( get_private_key( sessmgr, "active" ), control->get_chain_id() );
         BOOST_CHECK_EXCEPTION( push_transaction( trx ), transaction_exception,
                                fc_exception_message_starts_with(
                                   "Payer 'sessmgr3' did not authorize this action" ) );
      }

      // 2. Giving the payer that real authorization pushes the action past the single declared
      //    authorization each of these accepts, so no arrangement satisfies both barriers.
      {
         signed_transaction trx;
         add_action( trx, { {sessmgr, config::sysio_payer_name},
                            {sessmgr, config::active_name},
                            {user, config::active_name} } );
         set_transaction_headers( trx );
         trx.sign( signing_key, control->get_chain_id() );
         trx.sign( get_private_key( sessmgr, "active" ), control->get_chain_id() );
         BOOST_CHECK_EXCEPTION( push_transaction( trx ), irrelevant_auth_exception,
                                fc_exception_message_starts_with( expected ) );
      }
   }
} FC_LOG_AND_RETHROW()

// ===== 1. addpolicy validation =====

// Non-node-owner cannot issue a policy
BOOST_FIXTURE_TEST_CASE( addpolicy_non_nodeowner, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // bob is not a node owner, so addpolicy should fail
   BOOST_CHECK_EXCEPTION(
      add_roa_policy("bob"_n, user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Only Node Owners can issue policies for this generation."));
} FC_LOG_AND_RETHROW()

// Duplicate policy from same issuer should fail
BOOST_FIXTURE_TEST_CASE( addpolicy_duplicate, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("A policy for this owner already exists from this issuer. Use expandpolicy instead."));
} FC_LOG_AND_RETHROW()

// All-zero allocation should fail
BOOST_FIXTURE_TEST_CASE( addpolicy_all_zero, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "0.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("At least one of NET, CPU, or RAM must be allocated."));
} FC_LOG_AND_RETHROW()

// Exceeding available SYS should fail
BOOST_FIXTURE_TEST_CASE( addpolicy_insufficient_sys, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Each T1 node owner gets ~4% of total SYS. Try to allocate way more than that.
   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "900000000.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Not enough unallocated SYS for this policy."));
} FC_LOG_AND_RETHROW()

// Cannot allocate CPU/NET to sysio.* accounts
BOOST_FIXTURE_TEST_CASE( addpolicy_sysio_no_bw, sysio_roa_full_tester ) try {
   create_accounts({"sysio.test1"_n}, false, false, false, false);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], "sysio.test1"_n, "1.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot allocate CPU/NET to sysio accounts."));
} FC_LOG_AND_RETHROW()

// RAM-only policy for sysio.* accounts should succeed
BOOST_FIXTURE_TEST_CASE( addpolicy_sysio_ram_only, sysio_roa_full_tester ) try {
   create_accounts({"sysio.test2"_n}, false, false, false, false);
   produce_block();

   add_roa_policy(node_owners[2], "sysio.test2"_n, "0.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   auto p = get_policy("sysio.test2"_n, node_owners[2]);
   BOOST_TEST(!p.is_null());
   BOOST_TEST(p["ram_weight"].as_string() == "1.0000 SYS");
   BOOST_TEST(p["net_weight"].as_string() == "0.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "0.0000 SYS");
} FC_LOG_AND_RETHROW()

// ===== 1b. shared boundary guards: core-SYS symbol + owner existence =====
// These lock in the validation added across addpolicy/expandpolicy/reducepolicy. The symbol guard
// matters specifically for the same-but-wrong-symbol case (all three weights in one non-SYS symbol):
// asset::operator+ only checks operands share a symbol and the affordability gate compares raw
// amounts, so without check_core_symbol such a weight is silently accepted and mis-scales reserve
// accounting. Mixed symbols already fail at the asset addition; this is the case that did not.

// addpolicy rejects a wrong-symbol (here TST) set of weights.
BOOST_FIXTURE_TEST_CASE( addpolicy_wrong_symbol, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], user, "1.0000 TST", "1.0000 TST", "1.0000 TST", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("policy weights must be denominated in the core SYS symbol"));
} FC_LOG_AND_RETHROW()

// addpolicy rejects an owner account that does not exist (guards the reslimit / system-resource
// writes that follow -- they would otherwise run against a non-account).
BOOST_FIXTURE_TEST_CASE( addpolicy_nonexistent_owner, sysio_roa_full_tester ) try {
   BOOST_CHECK_EXCEPTION(
      add_roa_policy(node_owners[2], "ghostacct"_n, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("owner account does not exist"));
} FC_LOG_AND_RETHROW()

// expandpolicy rejects a wrong-symbol increment on an existing (SYS) policy.
BOOST_FIXTURE_TEST_CASE( expandpolicy_wrong_symbol, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();
   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], user, "1.0000 TST", "1.0000 TST", "1.0000 TST", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("policy weights must be denominated in the core SYS symbol"));
} FC_LOG_AND_RETHROW()

// reducepolicy rejects a wrong-symbol reduction on an existing (SYS) policy. The symbol guard fires
// before the time_block / below-zero checks, so a time_block of 0 is enough to reach it.
BOOST_FIXTURE_TEST_CASE( reducepolicy_wrong_symbol, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();
   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "1.0000 TST", "1.0000 TST", "1.0000 TST", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("policy weights must be denominated in the core SYS symbol"));
} FC_LOG_AND_RETHROW()

// ===== 2. expandpolicy validation =====

// Expand non-existent policy should fail
BOOST_FIXTURE_TEST_CASE( expandpolicy_no_policy, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // No policy exists from node_owners[3] for this user
   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[3], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("You have no policy for this owner."));
} FC_LOG_AND_RETHROW()

// Non-node-owner cannot expand
BOOST_FIXTURE_TEST_CASE( expandpolicy_non_nodeowner, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy("bob"_n, user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Only Node Owners can manage policies."));
} FC_LOG_AND_RETHROW()

// All-zero expand should fail
BOOST_FIXTURE_TEST_CASE( expandpolicy_all_zero, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], user, "0.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("At least one of NET, CPU, or RAM must be increased."));
} FC_LOG_AND_RETHROW()

// Expanding beyond available SYS should fail
BOOST_FIXTURE_TEST_CASE( expandpolicy_insufficient_sys, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], user, "900000000.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Issuer does not have enough unallocated SYS for this policy expansion."));
} FC_LOG_AND_RETHROW()

// Cannot expand CPU/NET for sysio.* accounts
BOOST_FIXTURE_TEST_CASE( expandpolicy_sysio_no_bw, sysio_roa_full_tester ) try {
   create_accounts({"sysio.test3"_n}, false, false, false, false);
   produce_block();

   // First add a RAM-only policy
   add_roa_policy(node_owners[2], "sysio.test3"_n, "0.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      expand_roa_policy(node_owners[2], "sysio.test3"_n, "1.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot allocate CPU/NET to sysio accounts."));
} FC_LOG_AND_RETHROW()

// ===== 3. reducepolicy validation =====

// Reduce non-existent policy should fail
BOOST_FIXTURE_TEST_CASE( reducepolicy_no_policy, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[3], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("You have no policy for this owner."));
} FC_LOG_AND_RETHROW()

// Non-node-owner cannot reduce
BOOST_FIXTURE_TEST_CASE( reducepolicy_non_nodeowner, sysio_roa_full_tester ) try {
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy("bob"_n, "alice"_n, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Only Node Owners can manage policies."));
} FC_LOG_AND_RETHROW()

// Cannot reduce NET below zero
BOOST_FIXTURE_TEST_CASE( reducepolicy_below_zero, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   // Try to reduce more NET than allocated
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "11.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot reduce NET below zero"));
} FC_LOG_AND_RETHROW()

// A negative reduction weight is rejected outright, per NET/CPU/RAM independently.
// reducepolicy applies each weight as a SUBTRACTION, so a negative amount ADDS quota while
// still satisfying the reduce-below-zero upper bounds (`w <= stored`, true for any negative
// w whenever the stored weight is positive). Left unguarded it inflates the account's
// resource limits past the issuer's ROA budget -- bypassing expandpolicy's free-allocation
// check -- and desynchronises the reslimit row and nodeowners accounting from the policy.
// The NET case is CertiK WNS-03's PoC verbatim: a 10.0000 SYS policy reduced by -5.0000 SYS
// used to end at 15.0000 SYS.
BOOST_FIXTURE_TEST_CASE( reducepolicy_negative_weight_rejected, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   int64_t ram_before, net_before, cpu_before;
   control->get_resource_limits_manager().get_account_limits(user, ram_before, net_before, cpu_before);

   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "-5.0000 SYS", "0.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("NET weight cannot be negative"));
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "0.0000 SYS", "-5.0000 SYS", "0.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("CPU weight cannot be negative"));
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "0.0000 SYS", "0.0000 SYS", "-5.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("RAM weight cannot be negative"));
   produce_block();

   // The policy is untouched by the rejected attempts...
   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "10.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "10.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "10.0000 SYS");

   // ...and no quota was minted onto the account.
   int64_t ram_after, net_after, cpu_after;
   control->get_resource_limits_manager().get_account_limits(user, ram_after, net_after, cpu_after);
   BOOST_TEST(net_after == net_before);
   BOOST_TEST(cpu_after == cpu_before);
   BOOST_TEST(ram_after == ram_before);
} FC_LOG_AND_RETHROW()

// Cannot reduce before time_block
BOOST_FIXTURE_TEST_CASE( reducepolicy_before_timeblock, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Set time_block far in the future
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 999999999, 0);
   produce_block();

   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot reduce policy before time_block"));
} FC_LOG_AND_RETHROW()

// ===== 8. Multi-node-owner policy interactions =====

// Expand after reduce on the same policy
BOOST_FIXTURE_TEST_CASE( expand_after_reduce, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "20.0000 SYS", "20.0000 SYS", "20.0000 SYS", 0, 0);
   produce_block();

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   int64_t ram_after_add = ram;

   // Reduce by half
   reduce_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 100000);
   BOOST_TEST(cpu == 100000);

   // Expand back
   expand_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(ram == ram_after_add);
   BOOST_TEST(net == 200000);
   BOOST_TEST(cpu == 200000);

   auto p = get_policy(user, node_owners[2]);
   BOOST_TEST(p["net_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "20.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "20.0000 SYS");
} FC_LOG_AND_RETHROW()

// Reduce one issuer's policy doesn't affect another issuer's policy
BOOST_FIXTURE_TEST_CASE( reduce_one_issuer_isolates_other, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // Two policies from different issuers
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   add_roa_policy(node_owners[3], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 200000);
   BOOST_TEST(cpu == 200000);

   // Reduce only node_owners[2]'s policy completely
   reduce_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0);
   produce_block();

   // node_owners[3]'s policy should be unchanged
   auto p = get_policy(user, node_owners[3]);
   BOOST_TEST(p["net_weight"].as_string() == "10.0000 SYS");
   BOOST_TEST(p["cpu_weight"].as_string() == "10.0000 SYS");
   BOOST_TEST(p["ram_weight"].as_string() == "10.0000 SYS");

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 100000);
   BOOST_TEST(cpu == 100000);

   // Can still expand node_owners[3]'s policy
   expand_roa_policy(node_owners[3], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0);
   produce_block();

   control->get_resource_limits_manager().get_account_limits(user, ram, net, cpu);
   BOOST_TEST(net == 150000);
   BOOST_TEST(cpu == 150000);
} FC_LOG_AND_RETHROW()

// ===== 9. newuser edge cases =====

// Creating many users depletes sysio RAM predictably
BOOST_FIXTURE_TEST_CASE( newuser_ram_depletion_tracking, sysio_roa_full_tester ) try {
   auto r = get_reslimit("sysio"_n);
   int64_t sysio_ram_start = r["ram_bytes"].as_int64();

   // Create users from multiple node owners and verify accounting
   auto user1 = create_newuser(node_owners[2]);
   auto user2 = create_newuser(node_owners[3]);
   auto user3 = create_newuser(node_owners[4]);
   produce_block();

   r = get_reslimit("sysio"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == sysio_ram_start - 3 * (int64_t)newaccount_ram);

   // Each new user should have exactly newaccount_ram
   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits(user1, ram, net, cpu);
   BOOST_TEST(ram == (int64_t)newaccount_ram);
   control->get_resource_limits_manager().get_account_limits(user2, ram, net, cpu);
   BOOST_TEST(ram == (int64_t)newaccount_ram);
   control->get_resource_limits_manager().get_account_limits(user3, ram, net, cpu);
   BOOST_TEST(ram == (int64_t)newaccount_ram);

   // sysio.acct reslimit tracks all gifted RAM
   r = get_reslimit("sysio.acct"_n);
   BOOST_TEST(r["ram_bytes"].as_int64() == (int64_t)((3 + 1) * newaccount_ram));
} FC_LOG_AND_RETHROW()

// Non-tier-1 node owner cannot create users
BOOST_FIXTURE_TEST_CASE( newuser_tier2_fails, sysio_roa_full_tester ) try {
   create_accounts({"tier2owner"_n}, false, false, false, false);
   register_node_owner("tier2owner"_n, 2);
   produce_block();

   BOOST_REQUIRE_EXCEPTION(
      newuser("tier2owner"_n, "nonce1"_n, get_public_key("tier2owner"_n, "active")),
      sysio_assert_message_exception,
      sysio_assert_message_is("Creator is not a registered tier-1 node owner"));
} FC_LOG_AND_RETHROW()

// Only tier 1 is provisioned a personal policy at registration -- it is the sole tier that can call
// newuser, whose sponsorship rows are the only writes billed to a node owner. Tiers 2 and 3 get the
// nodeowners budget, a reslimit row, and the 10% sysio RAM grant, but no allocation of their own,
// and can still self-issue afterwards because addpolicy costs the issuer nothing.
BOOST_FIXTURE_TEST_CASE( regnodeowner_personal_policy_is_tier1_only, sysio_roa_full_tester ) try {
   // The fixture already fills all 21 tier-1 slots, so reuse one rather than registering another.
   const auto t1owner = node_owners[1];
   create_accounts({"t2owner"_n, "t3owner"_n}, false, false, false, false);
   register_node_owner("t2owner"_n, 2);
   register_node_owner("t3owner"_n, 3);
   produce_block();

   // Tier 1 keeps its self-issued personal policy: 0.0500 SYS each of NET/CPU, 0.0080 SYS of RAM.
   auto t1_personal = get_policy(t1owner, t1owner);
   BOOST_REQUIRE(!t1_personal.is_null());
   BOOST_TEST(t1_personal["net_weight"].as<asset>().get_amount() == 500);
   BOOST_TEST(t1_personal["cpu_weight"].as<asset>().get_amount() == 500);
   BOOST_TEST(t1_personal["ram_weight"].as<asset>().get_amount() == 80);

   // Tiers 2 and 3 get none.
   BOOST_TEST(get_policy("t2owner"_n, "t2owner"_n).is_null());
   BOOST_TEST(get_policy("t3owner"_n, "t3owner"_n).is_null());

   // Every tier still contributes 10% of its allocation to the sysio RAM pool.
   for (auto owner : {t1owner, "t2owner"_n, "t3owner"_n}) {
      auto grant = get_policy("sysio"_n, owner);
      BOOST_REQUIRE(!grant.is_null());
      auto node = get_nodeowner(owner);
      BOOST_REQUIRE(!node.is_null());
      BOOST_TEST(grant["ram_weight"].as<asset>().get_amount()
                 == node["total_sys"].as<asset>().get_amount() / 10);
      BOOST_TEST(grant["net_weight"].as<asset>().get_amount() == 0);
      BOOST_TEST(grant["cpu_weight"].as<asset>().get_amount() == 0);
   }

   // Tier 2/3 hold no bandwidth, and their nodeowners accounting excludes the personal weights,
   // so the full remainder of the tier budget stays issuable.
   for (auto owner : {"t2owner"_n, "t3owner"_n}) {
      int64_t ram, net, cpu;
      control->get_resource_limits_manager().get_account_limits(owner, ram, net, cpu);
      BOOST_TEST(net == 0);
      BOOST_TEST(cpu == 0);

      auto node = get_nodeowner(owner);
      const int64_t total = node["total_sys"].as<asset>().get_amount();
      BOOST_TEST(node["allocated_bw"].as<asset>().get_amount() == 0);
      BOOST_TEST(node["allocated_ram"].as<asset>().get_amount() == total / 10);
      BOOST_TEST(node["allocated_sys"].as<asset>().get_amount() == total / 10);
   }

   // A tier-3 owner can still issue to itself; sysio.roa pays both the CPU/NET and the row RAM.
   add_roa_policy("t3owner"_n, "t3owner"_n, "0.0100 SYS", "0.0100 SYS", "0.0100 SYS", 0, 0);
   produce_block();

   auto t3_self = get_policy("t3owner"_n, "t3owner"_n);
   BOOST_REQUIRE(!t3_self.is_null());
   BOOST_TEST(t3_self["net_weight"].as<asset>().get_amount() == 100);
   BOOST_TEST(t3_self["cpu_weight"].as<asset>().get_amount() == 100);

   int64_t ram, net, cpu;
   control->get_resource_limits_manager().get_account_limits("t3owner"_n, ram, net, cpu);
   BOOST_TEST(net == 100);
   BOOST_TEST(cpu == 100);
} FC_LOG_AND_RETHROW()

// newuser correctly populates both policies table and reslimit for sysio.acct
BOOST_FIXTURE_TEST_CASE( newuser_sysio_acct_policy_tracking, sysio_roa_full_tester ) try {
   auto p = get_policy("sysio.acct"_n, "sysio"_n);
   int64_t initial_ram_weight = p["ram_weight"].as<asset>().get_amount();

   create_newuser(node_owners[2]);
   create_newuser(node_owners[2]);
   produce_block();

   p = get_policy("sysio.acct"_n, "sysio"_n);
   int64_t updated_ram_weight = p["ram_weight"].as<asset>().get_amount();

   // Each newuser adds newaccount_ram / bytes_per_unit to the sysio.acct policy ram_weight
   int64_t ram_weight_per_user = (int64_t)newaccount_ram / 104;  // bytes_per_unit = 104
   BOOST_TEST(updated_ram_weight == initial_ram_weight + 2 * ram_weight_per_user);
} FC_LOG_AND_RETHROW()

// setbyteprice after activation must not skew the sysio.acct bucket: newuser converts newaccount_ram
// at the bucket's frozen creation price (104), not the live one, so it still records
// newaccount_ram/104 units after a price change -- otherwise the policy ram_weight would no longer
// map to the bytes actually moved. (Comment-2 drift guard.)
BOOST_FIXTURE_TEST_CASE( sysio_acct_bucket_uses_frozen_price, sysio_roa_full_tester ) try {
   auto p = get_policy("sysio.acct"_n, "sysio"_n);
   int64_t initial_ram_weight = p["ram_weight"].as<asset>().get_amount();

   // Move the global price to another valid divisor of newaccount_ram (1144 = 2^3*11*13); 8 != 104.
   base_tester::push_action(ROA, "setbyteprice"_n, ROA, mvo()("bytes_per_unit", 8));
   produce_block();

   create_newuser(node_owners[2]);
   produce_block();

   p = get_policy("sysio.acct"_n, "sysio"_n);
   int64_t updated_ram_weight = p["ram_weight"].as<asset>().get_amount();

   // Frozen price (104) is used: +newaccount_ram/104 = +11, NOT the live-price +newaccount_ram/8 = +143.
   BOOST_TEST(updated_ram_weight == initial_ram_weight + (int64_t)newaccount_ram / 104);
} FC_LOG_AND_RETHROW()

// ===== 10. extendpolicy validation =====

// Extend non-existent policy should fail
BOOST_FIXTURE_TEST_CASE( extendpolicy_no_policy, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // No policy from node_owners[3] for this user
   auto result = extend_policy(user, node_owners[3], 100);
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: Policy does not exist under this issuer for this owner"), result);
} FC_LOG_AND_RETHROW()

// Cannot reduce a policy's time_block via extend
BOOST_FIXTURE_TEST_CASE( extendpolicy_cannot_reduce_timeblock, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 1000, 0);
   produce_block();

   // Try to set a lower time_block
   auto result = extend_policy(user, node_owners[2], 500);
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: Cannot reduce a policies existing time_block"), result);
} FC_LOG_AND_RETHROW()

// Cannot set time_block lower than current block
BOOST_FIXTURE_TEST_CASE( extendpolicy_past_timeblock, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   // time_block=0 means already expired
   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   // Try to extend to block 1 which is in the past
   auto result = extend_policy(user, node_owners[2], 1);
   BOOST_REQUIRE_EQUAL(error("assertion failure with message: You cannot set a time_block lower than the current block"), result);
} FC_LOG_AND_RETHROW()

// Successful extend followed by reduce respects new time_block
BOOST_FIXTURE_TEST_CASE( extendpolicy_blocks_reduce, sysio_roa_full_tester ) try {
   auto user = create_newuser(node_owners[2]);
   produce_block();

   add_roa_policy(node_owners[2], user, "10.0000 SYS", "10.0000 SYS", "10.0000 SYS", 0, 0);
   produce_block();

   // Extend to far future
   extend_policy(user, node_owners[2], 999999999);

   // Now reduce should fail because of the new time_block
   BOOST_CHECK_EXCEPTION(
      reduce_roa_policy(node_owners[2], user, "5.0000 SYS", "5.0000 SYS", "5.0000 SYS", 0),
      sysio_assert_message_exception,
      sysio_assert_message_is("Cannot reduce policy before time_block"));

   // But expand should still work
   BOOST_REQUIRE_NO_THROW(
      expand_roa_policy(node_owners[2], user, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0));
} FC_LOG_AND_RETHROW()

// ---- setsyscode / setsysabi: exact, conserving, bidirectional RAM from sysio ----

// setsyscode deploys code, makes the account privileged, and gifts exactly the RAM the code
// consumes out of sysio's pool (a conserving transfer, not a mint).
BOOST_FIXTURE_TEST_CASE( setsyscode_gifts_exact_from_sysio, sysio_roa_tester ) try {
   // System-contract targets always have a finite ROA quota in prod; give alice one.
   BOOST_REQUIRE_EQUAL( success(), regnodeowner("alice"_n, 1) );
   produce_blocks();

   auto& rlm = control->get_resource_limits_manager();
   int64_t n, cpu;
   int64_t sysio_q0;  rlm.get_account_limits("sysio"_n, sysio_q0, n, cpu);
   int64_t alice_q0;  rlm.get_account_limits("alice"_n, alice_q0, n, cpu);
   int64_t alice_u0 = rlm.get_account_ram_usage("alice"_n);
   int64_t sysio_res0 = get_reslimit("sysio"_n)["ram_bytes"].as<int64_t>();
   int64_t acct_res0  = get_reslimit("sysio.acct"_n)["ram_bytes"].as<int64_t>();

   auto wasm = test_contracts::sysio_token_wasm();
   BOOST_REQUIRE_EQUAL( success(),
      push_action(config::system_account_name, "setsyscode"_n, mvo()
         ("account","alice")("vmtype",0)("vmversion",0)("code", bytes(wasm.begin(), wasm.end()))) );
   produce_blocks();

   int64_t delta = rlm.get_account_ram_usage("alice"_n) - alice_u0;
   BOOST_REQUIRE_GT( delta, 0 );

   // account is now privileged
   const auto* meta = control->find_account_metadata("alice"_n);
   BOOST_REQUIRE( meta != nullptr && meta->is_privileged() );

   // exact gift to alice
   int64_t alice_q1;  rlm.get_account_limits("alice"_n, alice_q1, n, cpu);
   BOOST_REQUIRE_EQUAL( alice_q1 - alice_q0, delta );

   // conserving transfer out of sysio: chain quota + reslimit pool drop by delta, sysio.acct bucket rises
   int64_t sysio_q1;  rlm.get_account_limits("sysio"_n, sysio_q1, n, cpu);
   BOOST_REQUIRE_EQUAL( sysio_q0 - sysio_q1, delta );
   BOOST_REQUIRE_EQUAL( sysio_res0 - get_reslimit("sysio"_n)["ram_bytes"].as<int64_t>(), delta );
   BOOST_REQUIRE_EQUAL( get_reslimit("sysio.acct"_n)["ram_bytes"].as<int64_t>() - acct_res0, delta );
} FC_LOG_AND_RETHROW()

// Re-deploying a smaller contract reclaims the freed RAM back to sysio's pool (delta < 0).
BOOST_FIXTURE_TEST_CASE( setsyscode_redeploy_reclaims_to_sysio, sysio_roa_tester ) try {
   BOOST_REQUIRE_EQUAL( success(), regnodeowner("alice"_n, 1) );
   produce_blocks();
   auto& rlm = control->get_resource_limits_manager();
   int64_t n, cpu;

   auto big = test_contracts::sysio_system_wasm();
   BOOST_REQUIRE_EQUAL( success(),
      push_action(config::system_account_name, "setsyscode"_n, mvo()
         ("account","alice")("vmtype",0)("vmversion",0)("code", bytes(big.begin(), big.end()))) );
   produce_blocks();
   int64_t sysio_q_mid;  rlm.get_account_limits("sysio"_n, sysio_q_mid, n, cpu);
   int64_t alice_u_mid = rlm.get_account_ram_usage("alice"_n);

   auto small = test_contracts::noop_wasm();
   BOOST_REQUIRE_EQUAL( success(),
      push_action(config::system_account_name, "setsyscode"_n, mvo()
         ("account","alice")("vmtype",0)("vmversion",0)("code", bytes(small.begin(), small.end()))) );
   produce_blocks();

   int64_t reclaimed = alice_u_mid - rlm.get_account_ram_usage("alice"_n);
   BOOST_REQUIRE_GT( reclaimed, 0 );  // usage dropped
   int64_t sysio_q_end;  rlm.get_account_limits("sysio"_n, sysio_q_end, n, cpu);
   BOOST_REQUIRE_EQUAL( sysio_q_end - sysio_q_mid, reclaimed );  // sysio pool recovered exactly the freed bytes
} FC_LOG_AND_RETHROW()

// setsysabi gifts the exact abi RAM out of sysio's pool too.
BOOST_FIXTURE_TEST_CASE( setsysabi_gifts_exact_from_sysio, sysio_roa_tester ) try {
   BOOST_REQUIRE_EQUAL( success(), regnodeowner("alice"_n, 1) );
   produce_blocks();
   auto& rlm = control->get_resource_limits_manager();
   int64_t n, cpu;
   int64_t sysio_q0;  rlm.get_account_limits("sysio"_n, sysio_q0, n, cpu);
   int64_t alice_u0 = rlm.get_account_ram_usage("alice"_n);

   // setabi expects a packed abi_def, not the json text.
   abi_def def = fc::json::from_string(test_contracts::sysio_token_abi()).as<abi_def>();
   auto packed = fc::raw::pack(def);
   BOOST_REQUIRE_EQUAL( success(),
      push_action(config::system_account_name, "setsysabi"_n, mvo()
         ("account","alice")("abi", packed)) );
   produce_blocks();

   int64_t delta = rlm.get_account_ram_usage("alice"_n) - alice_u0;
   BOOST_REQUIRE_GT( delta, 0 );
   int64_t sysio_q1;  rlm.get_account_limits("sysio"_n, sysio_q1, n, cpu);
   BOOST_REQUIRE_EQUAL( sysio_q0 - sysio_q1, delta );  // conserving: gift came out of sysio's pool
} FC_LOG_AND_RETHROW()

// A target that was never brought under ROA management still has an unlimited (-1) RAM limit.
// setsyscode must reject it -- giftram cannot account an exact byte transfer against an unlimited
// limit -- rather than deploy the code and silently skip the funding. Prod avoids this by creating
// the account with a finite (0) quota first; this covers the raw system-account path.
BOOST_FIXTURE_TEST_CASE( setsyscode_rejects_unlimited_ram_target, sysio_roa_tester ) try {
   auto& rlm = control->get_resource_limits_manager();
   int64_t r, n, cpu;
   rlm.get_account_limits("alice"_n, r, n, cpu);
   BOOST_REQUIRE_LT( r, 0 );  // precondition: alice has unlimited RAM (no ROA quota yet)

   auto wasm = test_contracts::sysio_token_wasm();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: giftram target must have a finite RAM limit"),
      push_action(config::system_account_name, "setsyscode"_n, mvo()
         ("account","alice")("vmtype",0)("vmversion",0)("code", bytes(wasm.begin(), wasm.end()))) );
} FC_LOG_AND_RETHROW()

// ---- newnameduser: depot-created vanity-named account, funded from sysio ----

BOOST_FIXTURE_TEST_CASE( newnameduser_creates_funds_idempotent, sysio_roa_tester ) try {
   auto& rlm = control->get_resource_limits_manager();
   int64_t n, cpu;
   int64_t sysio_q0;  rlm.get_account_limits("sysio"_n, sysio_q0, n, cpu);

   auto pub = get_public_key("alice"_n, "owner");  // stand-in for the holder's K1 key
   BOOST_REQUIRE_EQUAL( success(),
      push_action(ROA, "newnameduser"_n, mvo()("account","vanityname")("pubkey",pub)("tier",2)) );
   produce_blocks();

   // account exists and was funded the fixed newaccount_ram out of sysio's pool (chain quota)
   BOOST_REQUIRE( rlm.get_account_ram_usage("vanityname"_n) >= 0 );
   int64_t sysio_q1;  rlm.get_account_limits("sysio"_n, sysio_q1, n, cpu);
   BOOST_REQUIRE_EQUAL( sysio_q0 - sysio_q1, (int64_t)newaccount_ram );

   // idempotent: re-calling on the existing account is a no-op (no error, no double-fund)
   BOOST_REQUIRE_EQUAL( success(),
      push_action(ROA, "newnameduser"_n, mvo()("account","vanityname")("pubkey",pub)("tier",2)) );
   produce_blocks();
   int64_t sysio_q2;  rlm.get_account_limits("sysio"_n, sysio_q2, n, cpu);
   BOOST_REQUIRE_EQUAL( sysio_q1, sysio_q2 );
} FC_LOG_AND_RETHROW()

// tier-1 owner names must be a 2-6 char prefix; an out-of-range name is soft-skipped (non-throwing,
// so the depot dispatch never aborts) -- the account is simply not created. Verified via sysio's RAM
// pool: a creation draws exactly newaccount_ram from sysio, a soft-skip draws nothing. (We avoid
// find_account_metadata here -- that host query lags an inline-created account by a block.)
BOOST_FIXTURE_TEST_CASE( newnameduser_tier_name_rules, sysio_roa_tester ) try {
   auto& rlm = control->get_resource_limits_manager();
   int64_t n, cpu, q0, q1, q2;
   auto pub = get_public_key("alice"_n, "owner");

   // tier-1 name longer than 6 chars: soft-skipped -> no account, nothing drawn from sysio.
   rlm.get_account_limits("sysio"_n, q0, n, cpu);
   BOOST_REQUIRE_EQUAL( success(),
      push_action(ROA, "newnameduser"_n, mvo()("account","toolongt1")("pubkey",pub)("tier",1)) );
   produce_blocks();
   rlm.get_account_limits("sysio"_n, q1, n, cpu);
   BOOST_REQUIRE_EQUAL( q0, q1 );

   // tier-1 short prefix accepted -> account created, funded newaccount_ram from sysio's pool.
   BOOST_REQUIRE_EQUAL( success(),
      push_action(ROA, "newnameduser"_n, mvo()("account","acme")("pubkey",pub)("tier",1)) );
   produce_blocks();
   rlm.get_account_limits("sysio"_n, q2, n, cpu);
   BOOST_REQUIRE_EQUAL( q1 - q2, (int64_t)newaccount_ram );
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// nodeownreg tests (OPP Node Owner NFT claim: create-in-flow + register + record ETH link)
//
// Under the create-in-flow model the depot (sysio.msgch) inline-sends newnameduser (creates the
// account with the claimed wire key) then nodeownreg (registers + records the depositor's ETH key
// via an inline sysio.authex::recordlink). These unit tests drive the two sysio.roa actions
// directly, signed by ROA, the same way the depot would. The fixture wires the
// sysio.authex.active <- sysio.roa@sysio.code delegation that authorizes the inline recordlink.
// ---------------------------------------------------------------------------

class sysio_roa_nodeownreg_tester : public sysio_roa_tester {
public:
   static constexpr auto AUTHEX = "sysio.authex"_n;
   static constexpr auto DCLAIM = "sysio.dclaim"_n;

   sysio_roa_nodeownreg_tester() {
      create_accounts({DCLAIM});

      // Deploy authex -- the node-owner flow inline-records the depositor's ETH link there.
      set_code( AUTHEX, contracts::authex_wasm() );
      set_abi( AUTHEX, contracts::authex_abi().data() );
      set_privileged( AUTHEX );
      produce_blocks();

      const auto* accnt = control->find_account_metadata( AUTHEX );
      BOOST_REQUIRE( accnt != nullptr );
      abi_def abi;
      BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt->abi, abi), true);
      authex_abi_ser.set_abi(abi, abi_serializer::create_yield_function(abi_serializer_max_time));

      // Delegate sysio.authex.active to sysio.roa@sysio.code so nodeownreg's inline recordlink
      // (declared {sysio.authex, active}) is authorized -- the same code-permission grant the
      // production bootstrap wires. Without it the inline send fails auth and aborts the claim.
      // A single co-signer is trivially sorted, so no accounts re-sort is needed.
      authority a( get_public_key( AUTHEX, "active" ) );
      a.accounts.push_back( permission_level_weight{ { AUTHEX, config::sysio_code_name }, 1 } );
      a.accounts.push_back( permission_level_weight{ { ROA, config::sysio_code_name }, 1 } );
      std::sort(a.accounts.begin(), a.accounts.end(),
         [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.permission.actor, lhs.permission.permission)
                 < std::tie(rhs.permission.actor, rhs.permission.permission);
         });
      set_authority( AUTHEX, config::active_name, a, config::owner_name );
      produce_blocks();
   }

   // Push nodeownreg the way the depot does: signed by ROA (which carries sysio.roa.active).
   action_result nodeownreg(const name& owner, uint8_t tier,
                            const fc::crypto::public_key& eth_pub_key,
                            const fc::crypto::public_key& wire_pub_key) {
      std::vector<char> eth_address(20, '\0');
      if (eth_pub_key.contains<fc::em::public_key_shim>()) {
         const auto address_bytes = fc::crypto::ethereum::address_to_bytes(eth_pub_key);
         eth_address.assign(address_bytes.begin(), address_bytes.end());
      }
      return push_action(ROA, "nodeownreg"_n, mvo()
         ("owner", owner)
         ("tier", tier)
         ("eth_pub_key", eth_pub_key)
         ("wire_pub_key", wire_pub_key)
         ("eth_address", eth_address));
   }

   // Serialize through the upgraded ABI while omitting its trailing extension. The resulting
   // four-field bytes are the exact pre-WIRE-352 action shape used by legacy callers.
   action_result nodeownreg_legacy(const name& owner, uint8_t tier,
                                   const fc::crypto::public_key& eth_pub_key,
                                   const fc::crypto::public_key& wire_pub_key) {
      return push_action(ROA, "nodeownreg"_n, mvo()
         ("owner", owner)
         ("tier", tier)
         ("eth_pub_key", eth_pub_key)
         ("wire_pub_key", wire_pub_key));
   }

   // Create the claim account in-flow (depot path) with `wire_pub_key` as owner/active.
   action_result newnameduser(const name& account, const fc::crypto::public_key& wire_pub_key, uint8_t tier) {
      return push_action(ROA, "newnameduser"_n, mvo()
         ("account", account)("pubkey", wire_pub_key)("tier", tier));
   }

   // Seed an EVM link directly via the depot-only recordlink, signed as sysio.authex. Used to set up
   // a pre-existing link with a chosen key before a (mismatched) claim.
   action_result recordlink(const fc::crypto::public_key& pub_key, const name& account) {
      const auto address_bytes = fc::crypto::ethereum::address_to_bytes(pub_key);
      const std::vector<char> native_address(address_bytes.begin(), address_bytes.end());
      action act;
      act.account       = AUTHEX;
      act.name          = "recordlink"_n;
      act.authorization = {{AUTHEX, config::active_name}};
      act.data          = authex_abi_ser.variant_to_binary("recordlink",
         mvo()("account", account)("chain_kind", opp::types::ChainKind::CHAIN_KIND_EVM)
              ("pub_key", pub_key)("native_address", native_address),
         abi_serializer::create_yield_function(abi_serializer_max_time));
      return base_tester::push_action(std::move(act), AUTHEX.to_uint64_t());
   }

   static fc::crypto::public_key gen_em_key() {
      return fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   }
   static fc::crypto::public_key gen_k1_key() {
      return fc::crypto::private_key::generate(fc::crypto::private_key::key_type::k1).get_public_key();
   }

   // nodeownerreg reg_status + reject_reason values (mirror sysio.roa.hpp).
   static constexpr uint64_t CONFIRMED = 0, REJECTED = 1;
   static constexpr uint64_t R_NAME_INVALID = 1, R_OWNER_NOT_ACCOUNT = 2,
                             R_ACCOUNT_KEY_MISMATCH = 3, R_DUPLICATE = 4, R_LINK_KEY_MISMATCH = 5;

   abi_serializer authex_abi_ser;
};

// Happy path: depot creates the account with the claimed wire key, then nodeownreg registers and
// records the depositor's ETH link.
BOOST_FIXTURE_TEST_CASE( nodeownreg_happy_path, sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "claimacct"_n;
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 2));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_pub, wire_pub));
   produce_blocks();

   // A CONFIRMED registration proves the account was created with wire_pub: nodeownreg's
   // active_key_matches requires the account to exist and be controlled by exactly that key, so
   // reaching CONFIRMED (rather than OWNER_NOT_ACCOUNT / ACCOUNT_KEY_MISMATCH) is the existence proof.
   auto reg = get_nodeowner(owner);
   BOOST_REQUIRE_EQUAL(reg.is_null(), false);
   BOOST_REQUIRE_EQUAL(reg["tier"].as<uint32_t>(), 2);
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit.is_null(), false);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), CONFIRMED);
   // nodeownreg returning success implies the inline recordlink ({sysio.authex, active}) was
   // authorized and ran -- an unauthorized inline send would have aborted the whole transaction.
   // recordlink's own table effects are covered by the sysio.authex unit tests.
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( nodeownreg_accepts_legacy_four_field_payload,
                         sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "claimacct"_n;
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 2));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), nodeownreg_legacy(owner, 2, eth_pub, wire_pub));
   produce_blocks();

   auto reg = get_nodeowner(owner);
   BOOST_REQUIRE(!reg.is_null());
   BOOST_REQUIRE_EQUAL(reg["tier"].as<uint32_t>(), 2);
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE(!audit.is_null());
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), CONFIRMED);
} FC_LOG_AND_RETHROW()

// Existing account controlled by a different key than the claim -> REJECTED/ACCOUNT_KEY_MISMATCH.
BOOST_FIXTURE_TEST_CASE( nodeownreg_account_key_mismatch, sysio_roa_nodeownreg_tester ) try {
   const auto owner   = "claimacct"_n;
   const auto real_k  = gen_k1_key();   // key the account is actually created with
   const auto claimed = gen_k1_key();   // a different key in the claim
   const auto eth_pub = gen_em_key();

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, real_k, 2));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_pub, claimed));
   produce_blocks();

   BOOST_REQUIRE(get_nodeowner(owner).is_null());
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), REJECTED);
   BOOST_REQUIRE_EQUAL(audit["reason"].as<uint64_t>(), R_ACCOUNT_KEY_MISMATCH);
} FC_LOG_AND_RETHROW()

// SEC-087: an account that already carries a reslimit row must NOT be blocked from registering. Such a
// row can be planted on any account by a node owner via addpolicy (no target consent), which previously
// forced the claim into a permanent OWNER_HAS_RESLIMIT soft-fail -- a denial-of-registration grief.
// nodeownreg now RECONCILES: regnodeowner stacks the node-owner allocation onto the existing row via
// increase_reslimit, the claim reaches CONFIRMED, and the planted policy is preserved (still the
// issuer's, reclaimable via reducepolicy). No value is stolen and the gift is not double-counted.
BOOST_FIXTURE_TEST_CASE( nodeownreg_reconciles_existing_reslimit, sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "claimacct"_n;
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();

   // Create the claim account in-flow, then have NODE_DADDY plant a RAM-only policy on it so it carries
   // a reslimit row before the claim is resolved (NODE_DADDY is the tier-1 issuer seeded by the fixture).
   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 2));
   produce_blocks();
   add_roa_policy(NODE_DADDY, owner, "0.0000 SYS", "0.0000 SYS", "1.0000 SYS", 0, 0);
   produce_blocks();

   // bytes_per_unit is a fixture constant (cf. verify_ram): the activation price is 104. The claim
   // registers at TIER 2, which regnodeowner provisions with no personal allocation -- the personal
   // policy is tier-1 only -- so the reconcile stacks nothing on top of the planted policy.
   const int64_t bytes_per_unit    = 104;
   const int64_t planted_ram_bytes = 10000 * bytes_per_unit;   // 1.0000 SYS RAM-only policy

   // Pre-state: the planted row exists with the one-time newaccount_ram gift folded in once (by the
   // addpolicy create-branch), and net/cpu still zero.
   auto r = get_reslimit(owner);
   BOOST_REQUIRE_EQUAL(r.is_null(), false);
   BOOST_REQUIRE_EQUAL(r["net_weight"].as_string(), "0.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["cpu_weight"].as_string(), "0.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["ram_bytes"].as_int64(), planted_ram_bytes + newaccount_ram);
   const auto daddy_before = get_nodeowner(NODE_DADDY);

   // The claim must commit AND register -- not soft-fail, not throw.
   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_pub, wire_pub));
   produce_blocks();

   // Registration completed (the SEC-087 outcome).
   auto reg = get_nodeowner(owner);
   BOOST_REQUIRE_EQUAL(reg.is_null(), false);
   BOOST_REQUIRE_EQUAL(reg["tier"].as<uint32_t>(), 2);
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), CONFIRMED);
   BOOST_REQUIRE_EQUAL(audit["reason"].as<uint64_t>(), 0); // NONE

   // reslimit reconciled: the planted row is preserved untouched and the gift is NOT re-added. A
   // tier-2 registration adds no personal weight, so the row is exactly what addpolicy left.
   const int64_t expected_ram = planted_ram_bytes + newaccount_ram;
   r = get_reslimit(owner);
   BOOST_REQUIRE_EQUAL(r["net_weight"].as_string(), "0.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["cpu_weight"].as_string(), "0.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["ram_bytes"].as_int64(), expected_ram);

   // On-chain quota synced to the reconciled row totals (absolute set in regnodeowner).
   int64_t ram = 0, net = 0, cpu = 0;
   control->get_resource_limits_manager().get_account_limits(owner, ram, net, cpu);
   BOOST_REQUIRE_EQUAL(ram, expected_ram);
   BOOST_REQUIRE_EQUAL(net, 0);
   BOOST_REQUIRE_EQUAL(cpu, 0);

   // The planted policy is untouched: still issued by NODE_DADDY, still reclaimable via reducepolicy.
   auto pol = get_policy(owner, NODE_DADDY);
   BOOST_REQUIRE_EQUAL(pol.is_null(), false);
   BOOST_REQUIRE_EQUAL(pol["net_weight"].as_string(), "0.0000 SYS");
   BOOST_REQUIRE_EQUAL(pol["cpu_weight"].as_string(), "0.0000 SYS");
   BOOST_REQUIRE_EQUAL(pol["ram_weight"].as_string(), "1.0000 SYS");

   // Conservation: nodeownreg does not touch the planted-policy issuer's allocations (the planted
   // resources stay backed by NODE_DADDY's reserve; the node-owner allocation is independently backed).
   auto daddy_after = get_nodeowner(NODE_DADDY);
   BOOST_REQUIRE_EQUAL(daddy_after["allocated_sys"].as_string(), daddy_before["allocated_sys"].as_string());
   BOOST_REQUIRE_EQUAL(daddy_after["allocated_bw"].as_string(),  daddy_before["allocated_bw"].as_string());
   BOOST_REQUIRE_EQUAL(daddy_after["allocated_ram"].as_string(), daddy_before["allocated_ram"].as_string());
} FC_LOG_AND_RETHROW()

// SEC-087 companion: a planted policy can carry NET/CPU as well as RAM. Verify the reconcile leaves
// them intact (the increase_reslimit MODIFY branch) and registration still reaches CONFIRMED.
BOOST_FIXTURE_TEST_CASE( nodeownreg_reconciles_existing_reslimit_net_cpu, sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "claimacct"_n;
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 2));
   produce_blocks();
   add_roa_policy(NODE_DADDY, owner, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_blocks();

   const int64_t bytes_per_unit    = 104;
   const int64_t planted_ram_bytes = 10000 * bytes_per_unit;

   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_pub, wire_pub));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(get_nodeowner(owner).is_null(), false);
   BOOST_REQUIRE_EQUAL(get_nodeownerreg(owner)["status"].as<uint64_t>(), CONFIRMED);

   // A tier-2 registration adds no personal weight, so the planted 1.0000 SYS of net/cpu and the
   // planted RAM (plus the one-time gift) survive the reconcile unchanged.
   const int64_t expected_ram = planted_ram_bytes + newaccount_ram;
   auto r = get_reslimit(owner);
   BOOST_REQUIRE_EQUAL(r["net_weight"].as_string(), "1.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["cpu_weight"].as_string(), "1.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["ram_bytes"].as_int64(), expected_ram);

   int64_t ram = 0, net = 0, cpu = 0;
   control->get_resource_limits_manager().get_account_limits(owner, ram, net, cpu);
   BOOST_REQUIRE_EQUAL(ram, expected_ram);
   BOOST_REQUIRE_EQUAL(net, 10000); // 1.0000 SYS at precision 4
   BOOST_REQUIRE_EQUAL(cpu, 10000);
} FC_LOG_AND_RETHROW()

// SEC-087 at TIER 1, which is the only tier regnodeowner still provisions a personal allocation for.
// The two tier-2 companions above now pass zero weights into increase_reslimit, so this is the case
// that exercises its MODIFY branch with non-zero deltas: the planted policy's weights and the
// node-owner personal weights must stack, and the one-time newaccount_ram gift must be counted once
// rather than re-added. Without this the arithmetic that reconcile depends on would go untested on
// the registration path.
BOOST_FIXTURE_TEST_CASE( nodeownreg_tier1_reconcile_stacks_personal_weights, sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "t1own"_n;   // tier-1 name rule: 2-6 characters
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 1));
   produce_blocks();
   add_roa_policy(NODE_DADDY, owner, "1.0000 SYS", "1.0000 SYS", "1.0000 SYS", 0, 0);
   produce_blocks();

   const int64_t bytes_per_unit     = 104;
   const int64_t planted_ram_bytes  = 10000 * bytes_per_unit;   // 1.0000 SYS policy
   const int64_t personal_ram_bytes = 80    * bytes_per_unit;   // tier-1 personal RAM (0.0080 SYS)

   // Pre-state: planted policy only, gift folded in once by addpolicy's create branch.
   auto r = get_reslimit(owner);
   BOOST_REQUIRE_EQUAL(r.is_null(), false);
   BOOST_REQUIRE_EQUAL(r["net_weight"].as_string(), "1.0000 SYS");
   BOOST_REQUIRE_EQUAL(r["ram_bytes"].as_int64(), planted_ram_bytes + newaccount_ram);

   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 1, eth_pub, wire_pub));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(get_nodeowner(owner)["tier"].as<uint32_t>(), 1);
   BOOST_REQUIRE_EQUAL(get_nodeownerreg(owner)["status"].as<uint64_t>(), CONFIRMED);

   // MODIFY branch with non-zero deltas: planted 1.0000 + personal 0.0500 = 1.0500 SYS of net/cpu,
   // ram = planted + gift + personal, with the gift NOT re-added.
   const int64_t expected_ram = planted_ram_bytes + newaccount_ram + personal_ram_bytes;
   r = get_reslimit(owner);
   BOOST_REQUIRE_EQUAL(r["net_weight"].as_string(), "1.0500 SYS");
   BOOST_REQUIRE_EQUAL(r["cpu_weight"].as_string(), "1.0500 SYS");
   BOOST_REQUIRE_EQUAL(r["ram_bytes"].as_int64(), expected_ram);

   int64_t ram = 0, net = 0, cpu = 0;
   control->get_resource_limits_manager().get_account_limits(owner, ram, net, cpu);
   BOOST_REQUIRE_EQUAL(ram, expected_ram);
   BOOST_REQUIRE_EQUAL(net, 10500);
   BOOST_REQUIRE_EQUAL(cpu, 10500);

   // Planted policy untouched; the tier-1 personal policy now sits alongside it.
   auto planted = get_policy(owner, NODE_DADDY);
   BOOST_REQUIRE_EQUAL(planted.is_null(), false);
   BOOST_REQUIRE_EQUAL(planted["net_weight"].as_string(), "1.0000 SYS");
   auto personal = get_policy(owner, owner);
   BOOST_REQUIRE_EQUAL(personal.is_null(), false);
   BOOST_REQUIRE_EQUAL(personal["net_weight"].as_string(), "0.0500 SYS");
   BOOST_REQUIRE_EQUAL(personal["ram_weight"].as_string(), "0.0080 SYS");
} FC_LOG_AND_RETHROW()

// A tier-2 owner holds no CPU after this change, so it reaches
// transaction_context::verify_init_subjective_billing() with an objective limit of zero and is
// admitted purely on the subjective allowance. Objective billing is not the whole story here:
// addpolicy's payer is sysio.roa, but the subjective check looks at first-authorizers that are NOT
// payers, which is the issuer. Pushed without an explicit billed CPU so that check actually runs.
BOOST_FIXTURE_TEST_CASE( zero_cpu_owner_issues_under_subjective_billing, sysio_roa_full_tester ) try {
   create_accounts({"t2owner"_n, "targetacct"_n}, false, false, false, false);
   register_node_owner("t2owner"_n, 2);
   produce_block();

   auto& rlm = control->get_resource_limits_manager();
   int64_t ram = 0, net = 0, cpu = 0;
   rlm.get_account_limits("t2owner"_n, ram, net, cpu);
   BOOST_REQUIRE_EQUAL(net, 0);
   BOOST_REQUIRE_EQUAL(cpu, 0);

   auto& sub_bill = control->get_mutable_subjective_billing();
   sub_bill.set_disabled(false);
   BOOST_REQUIRE_GT(sub_bill.get_subjective_account_cpu_allowed().count(), 0);

   auto trace = push_subjective_action("t2owner"_n, "addpolicy"_n, mvo()
      ("owner", "targetacct")("issuer", "t2owner")
      ("net_weight", "0.0100 SYS")("cpu_weight", "0.0100 SYS")("ram_weight", "0.0100 SYS")
      ("time_block", 0)("network_gen", 0));
   BOOST_REQUIRE(!trace->except);
   produce_block();

   // The grant landed, and the issuer still holds nothing of its own.
   rlm.get_account_limits("targetacct"_n, ram, net, cpu);
   BOOST_REQUIRE_EQUAL(net, 100);
   BOOST_REQUIRE_EQUAL(cpu, 100);
   rlm.get_account_limits("t2owner"_n, ram, net, cpu);
   BOOST_REQUIRE_EQUAL(cpu, 0);

   // Prove the admission gate was actually reached rather than skipped: with the allowance driven
   // to zero the identical push is rejected by it. Without this, the assertions above would pass
   // just as happily if explicit_billed_cpu_time had short-circuited the check.
   sub_bill.set_subjective_account_cpu_allowed(fc::microseconds(0));
   BOOST_REQUIRE_EXCEPTION(
      push_subjective_action("t2owner"_n, "addpolicy"_n, mvo()
         ("owner", "targetacct")("issuer", "t2owner")
         ("net_weight", "0.0100 SYS")("cpu_weight", "0.0000 SYS")("ram_weight", "0.0000 SYS")
         ("time_block", 0)("network_gen", 0)),
      tx_cpu_usage_exceeded,
      fc_exception_message_contains("Subjectively terminated trx"));
} FC_LOG_AND_RETHROW()

// Account already carries a DIFFERENT EVM link (e.g. an operator createlink or an earlier deposit
// key) -> nodeownreg soft-fails LINK_KEY_MISMATCH rather than recording CONFIRMED against a stale
// link or silently keeping the old key.
BOOST_FIXTURE_TEST_CASE( nodeownreg_link_key_mismatch, sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "claimacct"_n;
   const auto wire_pub = gen_k1_key();
   const auto eth_a    = gen_em_key();   // key already linked to the account
   const auto eth_b    = gen_em_key();   // depositor key in the new claim

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 2));
   produce_blocks();
   // Pre-existing EVM link with eth_a; the account is not yet a node owner.
   BOOST_REQUIRE_EQUAL(success(), recordlink(eth_a, owner));
   produce_blocks();

   // Claim with a different depositor key -> soft-fail, not registered, link untouched.
   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_b, wire_pub));
   produce_blocks();

   BOOST_REQUIRE(get_nodeowner(owner).is_null());
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), REJECTED);
   BOOST_REQUIRE_EQUAL(audit["reason"].as<uint64_t>(), R_LINK_KEY_MISMATCH);
} FC_LOG_AND_RETHROW()

// Name invalid for the tier (tier-1 name > 6 chars) -> REJECTED/NAME_INVALID (account need not exist).
BOOST_FIXTURE_TEST_CASE( nodeownreg_name_invalid, sysio_roa_nodeownreg_tester ) try {
   const auto owner = "toolongname"_n;   // 11 chars: valid charset, too long for tier 1
   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 1, gen_em_key(), gen_k1_key()));
   produce_blocks();

   BOOST_REQUIRE(get_nodeowner(owner).is_null());
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), REJECTED);
   BOOST_REQUIRE_EQUAL(audit["reason"].as<uint64_t>(), R_NAME_INVALID);
} FC_LOG_AND_RETHROW()

// Valid-for-tier name that is not an account -> REJECTED/OWNER_NOT_ACCOUNT.
BOOST_FIXTURE_TEST_CASE( nodeownreg_owner_not_account, sysio_roa_nodeownreg_tester ) try {
   const auto owner = "ghost"_n;   // 5 chars: valid for tier 1, but no account was created
   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 1, gen_em_key(), gen_k1_key()));
   produce_blocks();

   BOOST_REQUIRE(get_nodeowner(owner).is_null());
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), REJECTED);
   BOOST_REQUIRE_EQUAL(audit["reason"].as<uint64_t>(), R_OWNER_NOT_ACCOUNT);
} FC_LOG_AND_RETHROW()

// Replay after a successful claim -> REJECTED/DUPLICATE, no abort.
BOOST_FIXTURE_TEST_CASE( nodeownreg_already_registered, sysio_roa_nodeownreg_tester ) try {
   const auto owner    = "claimacct"_n;
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();

   BOOST_REQUIRE_EQUAL(success(), newnameduser(owner, wire_pub, 2));
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_pub, wire_pub));
   produce_blocks();

   BOOST_REQUIRE_EQUAL(success(), nodeownreg(owner, 2, eth_pub, wire_pub));
   produce_blocks();
   auto audit = get_nodeownerreg(owner);
   BOOST_REQUIRE_EQUAL(audit["status"].as<uint64_t>(), REJECTED);
   BOOST_REQUIRE_EQUAL(audit["reason"].as<uint64_t>(), R_DUPLICATE);
} FC_LOG_AND_RETHROW()

// Invalid tier is a depot/system invariant -> hard abort (not a soft-fail).
BOOST_FIXTURE_TEST_CASE( nodeownreg_invalid_tier, sysio_roa_nodeownreg_tester ) try {
   const auto wire_pub = gen_k1_key();
   const auto eth_pub  = gen_em_key();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: Tier level must be between 1 and 3"),
      nodeownreg("someacct"_n, 0, eth_pub, wire_pub));
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: Tier level must be between 1 and 3"),
      nodeownreg("someacct"_n, 4, eth_pub, wire_pub));
} FC_LOG_AND_RETHROW()

// Non-EM depositor key is a depot invariant -> hard abort.
BOOST_FIXTURE_TEST_CASE( nodeownreg_non_em_key, sysio_roa_nodeownreg_tester ) try {
   const auto wire_pub = gen_k1_key();
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: eth_pub_key must be an EM (secp256k1) public key"),
      nodeownreg("someacct"_n, 1, gen_k1_key(), wire_pub));   // K1 depositor key -> reject
} FC_LOG_AND_RETHROW()

// ---- activateroa supply validation ----

// Deploys sysio.roa privileged but does NOT call activateroa (the standard fixtures auto-activate via
// init_roa with a fixed 75496 SYS supply), so activateroa's own input validation can be exercised
// directly with arbitrary supplies.
class roa_unactivated_tester : public tester {
public:
   roa_unactivated_tester() : tester(setup_policy::full_except_do_not_set_finalizers) {
      create_account(ROA,              config::system_account_name, false, true,  false, false);
      create_account("sysio.acct"_n,   config::system_account_name, false, false, false, false);
      create_account("sysio.authex"_n, config::system_account_name, false, false, false, false);
      set_contract(ROA, contracts::roa_wasm(), contracts::roa_abi().data());
      push_action(config::system_account_name, "setpriv"_n, config::system_account_name,
                  mvo()("account", ROA)("is_priv", 1));
      produce_block();
   }

   transaction_trace_ptr activate(const std::string& total_sys, uint64_t bytes_per_unit) {
      return base_tester::push_action(ROA, "activateroa"_n, ROA,
                                      mvo()("total_sys", total_sys)("bytes_per_unit", bytes_per_unit));
   }
};

// A supply so small that tier rounding makes the node-owner reserve exceed it (total 13 -> reserve 21,
// leftover -8) must be rejected, not silently underflow the signed->unsigned byte conversion and
// activate ROA with garbage reslimits.
BOOST_FIXTURE_TEST_CASE( activateroa_rejects_tiny_supply, roa_unactivated_tester ) try {
   BOOST_REQUIRE_EXCEPTION(
      activate("0.0013 SYS", 104),  // 13 smallest units; reserve rounds to 21 > 13
      sysio_assert_message_exception,
      sysio_assert_message_is("Total SYS too small: node-owner reserve exceeds supply"));
} FC_LOG_AND_RETHROW()

// A normal supply still activates cleanly through the same guards.
BOOST_FIXTURE_TEST_CASE( activateroa_accepts_normal_supply, roa_unactivated_tester ) try {
   BOOST_REQUIRE_NO_THROW( activate("75496.0000 SYS", 104) );
} FC_LOG_AND_RETHROW()

// A supply above the bound is rejected before the tier math (total_amount * 15, leftover *
// bytes_per_unit) could overflow int64. Defense in depth -- activateroa is a one-time governance call
// and real supplies are ~1e6x smaller.
BOOST_FIXTURE_TEST_CASE( activateroa_rejects_oversized_supply, roa_unactivated_tester ) try {
   BOOST_REQUIRE_EXCEPTION(
      activate("200000000000.0000 SYS", 104),  // 2e15 units, above the 1e15 bound
      sysio_assert_message_exception,
      sysio_assert_message_is("Total SYS out of range"));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
