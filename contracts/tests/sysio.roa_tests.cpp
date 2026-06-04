#include <test_contracts.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include "sysio.system_tester.hpp"
#include <fc/variant_object.hpp>
#include <boost/test/unit_test.hpp>
#include <string>
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
      BOOST_TEST(ram == 8320); // values set by contract, verify they don't change for this test
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
      sysio_assert_message_is("newaccount_ram needs to be evenly divisable to avoid dust"));

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
};

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

// tier-1 owner names must be a 2-6 char prefix; tier 2/3 up to 12.
BOOST_FIXTURE_TEST_CASE( newnameduser_tier_name_rules, sysio_roa_tester ) try {
   auto pub = get_public_key("alice"_n, "owner");
   // tier-1 name longer than 6 chars rejected
   BOOST_REQUIRE_EQUAL(
      error("assertion failure with message: Tier-1 owner name must be a 2-6 character prefix"),
      push_action(ROA, "newnameduser"_n, mvo()("account","toolongt1")("pubkey",pub)("tier",1)) );
   // tier-1 short prefix accepted
   BOOST_REQUIRE_EQUAL( success(),
      push_action(ROA, "newnameduser"_n, mvo()("account","acme")("pubkey",pub)("tier",1)) );
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
