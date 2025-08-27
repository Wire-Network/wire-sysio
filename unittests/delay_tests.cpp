#include <sysio/chain/global_property_object.hpp>
#include <sysio/testing/tester_network.hpp>
#include <sysio/testing/tester.hpp>

#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

using mvo = fc::mutable_variant_object;

const std::string sysio_token = name("sysio.token"_n).to_string();

static void create_accounts(validating_tester& chain) {
   chain.produce_blocks();
   chain.create_accounts({"sysio.msig"_n, "sysio.token"_n});
   chain.produce_blocks(10);

   chain.push_action(config::system_account_name,
      "setpriv"_n,
      config::system_account_name,
      mvo()
         ("account", "sysio.msig")
         ("is_priv", 1) );

   chain.set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   chain.set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   chain.set_code("sysio.msig"_n, test_contracts::sysio_msig_wasm());
   chain.set_abi("sysio.msig"_n, test_contracts::sysio_msig_abi());

   chain.produce_blocks();
   chain.create_account("tester"_n);
   chain.create_account("tester2"_n);
   chain.produce_blocks(10);
}

static void propose_approve_msig_trx(validating_tester& chain, const name& proposal_name, const name& auth, const fc::variant& pretty_trx) {
   permission_level perm = { auth, config::active_name };
   vector<permission_level> requested_perm = { perm };
   vector<permission_level> perm_with_payer = { perm, {auth, config::sysio_payer_name} };
   transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, chain.get_resolver(), abi_serializer::create_yield_function(chain.abi_serializer_max_time));

   chain.push_action("sysio.msig"_n, "propose"_n, perm_with_payer,
      mvo()
         ("proposer",      "tester")
         ("proposal_name", proposal_name)
         ("trx",           trx)
         ("requested",     requested_perm)
   );
   chain.push_action("sysio.msig"_n, "approve"_n, perm_with_payer,
      mvo()
         ("proposer",      "tester")
         ("proposal_name", proposal_name)
         ("level",         perm)
   );
}

static void propose_approve_msig_token_transfer_trx(validating_tester& chain, const name& proposal_name, const name& auth, uint32_t delay_sec, const std::string& quantity) {
   fc::variant pretty_trx = mvo()
      ("expiration", "2026-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", delay_sec)
      ("actions", fc::variants({
         mvo()
            ("account", name("sysio.token"_n))
            ("name", "transfer")
            ("authorization", vector<permission_level>{
                {auth, config::active_name},
                {auth, config::sysio_payer_name}
            })
            ("data", fc::mutable_variant_object()
               ("from", name("tester"_n))
               ("to", name("tester2"_n))
               ("quantity", quantity)
               ("memo", "hi" )
            )
      })
   );
   propose_approve_msig_trx(chain, proposal_name, auth, pretty_trx);
}

static void propose_approve_msig_updateauth_trx(validating_tester& chain, const name& proposal_name, const name& auth, uint32_t delay_sec) {
   fc::variant pretty_trx = fc::mutable_variant_object()
      ("expiration", "2026-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", delay_sec)
      ("actions", fc::variants({
         mvo()
            ("account", config::system_account_name)
            ("name", updateauth::get_name())
            ("authorization", vector<permission_level> {{ "tester"_n, config::active_name }})
            ("data", fc::mutable_variant_object()
               ("account", "tester")
               ("permission", "first")
               ("parent", "active")
               ("auth",  authority(chain.get_public_key("tester"_n, "first")))
            )
      })
   );

   propose_approve_msig_trx(chain, proposal_name, auth, pretty_trx);
}

static void propose_approve_msig_linkauth_trx(validating_tester& chain, const name& proposal_name, const name& requirement, const name& auth, uint32_t delay_sec) {
   fc::variant pretty_trx = fc::mutable_variant_object()
      ("expiration", "2026-01-01T00:30")
      ("ref_block_num", 2)
      ("ref_block_prefix", 3)
      ("max_net_usage_words", 0)
      ("max_cpu_usage_ms", 0)
      ("delay_sec", delay_sec)
      ("actions", fc::variants({
         mvo()
            ("account", config::system_account_name)
            ("name", linkauth::get_name())
            ("authorization", vector<permission_level>{{ "tester"_n, config::active_name }})
            ("data", fc::mutable_variant_object()
               ("account", "tester")
               ("code", sysio_token)
               ("type", "transfer")
               ("requirement", requirement)
            )
      })
   );

   propose_approve_msig_trx(chain, proposal_name, auth, pretty_trx);
}

static void exec_msig_trx(validating_tester& chain, name proposal_name, const vector<permission_level>& perm) {
   chain.push_action("sysio.msig"_n, "exec"_n, perm,
      mvo()
         ("proposer",      "tester")
         ("proposal_name", proposal_name)
         ("executer",      "tester")
   );
}

static asset get_currency_balance(const validating_tester& chain, account_name account) {
   return chain.get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), account);
}


BOOST_AUTO_TEST_SUITE(delay_tests)

// test link to permission with delay directly on it
BOOST_AUTO_TEST_CASE( link_delay_direct_test ) { try {
   validating_tester chain;
   const auto& tester_account = "tester"_n;

   create_accounts(chain);

   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
      ("account", "tester")
      ("permission", "first")
      ("parent", "active")
      ("auth",  authority(chain.get_public_key(tester_account, "first")))
   );
   chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
      ("account", "tester")
      ("code", sysio_token)
      ("type", "transfer")
      ("requirement", "first")
   );
   chain.produce_blocks();
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", sysio_token)
      ("maximum_supply", "9000000.0000 CUR")
   );

   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       sysio_token)
           ("quantity", "1000000.0000 CUR")
           ("memo", "for stuff")
   );

   auto trace = chain.push_action("sysio.token"_n, name("transfer"), "sysio.token"_n, fc::mutable_variant_object()
       ("from", sysio_token)
       ("to", "tester")
       ("quantity", "100.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   auto liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);

   auto auth = vector<permission_level>{{"tester"_n, config::active_name},{"tester"_n, config::sysio_payer_name}};
   trace = chain.push_action("sysio.token"_n, name("transfer"), auth, fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "1.0000 CUR")
       ("memo", "hi" )
   );

   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   trace = chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first"), 10))
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   // propose and approve an msig trx that transfers "quantity" tokens
   // from tester to tester2 with a delay of "delay_seconds"
   constexpr name proposal_name = "prop1"_n;

   propose_approve_msig_token_transfer_trx(chain, proposal_name, "tester"_n, 10, "3.0000 CUR");

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   chain.produce_blocks(18);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   // executue after delay of 10 seconds
   exec_msig_trx(chain, proposal_name, {
       { "tester"_n, config::active_name },
       { "tester"_n, config::sysio_payer_name }});

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("96.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("4.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() }/// link_delay_direct_test


// test link to permission with delay on permission between min permission and authorizing permission it
BOOST_AUTO_TEST_CASE( link_delay_direct_walk_parent_permissions_test ) { try {
   validating_tester chain;
   const auto& tester_account = "tester"_n;

   create_accounts(chain);

   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first")))
   );
   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "second")
           ("parent", "first")
           ("auth",  authority(chain.get_public_key(tester_account, "second")))
   );
   chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("code", sysio_token)
           ("type", "transfer")
           ("requirement", "second"));

   chain.produce_blocks();
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
           ("issuer", sysio_token)
           ("maximum_supply", "9000000.0000 CUR")
   );

   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       sysio_token)
           ("quantity", "1000000.0000 CUR")
           ("memo", "for stuff")
   );

   auto trace = chain.push_action("sysio.token"_n, name("transfer"), "sysio.token"_n, fc::mutable_variant_object()
       ("from", sysio_token)
       ("to", "tester")
       ("quantity", "100.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   auto liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);

   auto auth = vector<permission_level>{{tester_account, config::active_name},{tester_account, config::sysio_payer_name}};
   trace = chain.push_action("sysio.token"_n, name("transfer"), auth, fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "1.0000 CUR")
       ("memo", "hi" )
   );

   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   trace = chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first"), 20))
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();


   // propose and approve an msig trx that transfers "quantity" tokens
   // from tester to tester2 with a delay of "delay_seconds"
   constexpr name proposal_name = "prop1"_n;
   propose_approve_msig_token_transfer_trx(chain, proposal_name, "tester"_n, 20, "3.0000 CUR");

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   chain.produce_blocks(38);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   // executue after delay
   exec_msig_trx(chain, proposal_name, {
       { "tester"_n, config::active_name },
       { "tester"_n, config::sysio_payer_name }});

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("96.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("4.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() }/// link_delay_direct_walk_parent_permissions_test

// test removing delay on permission
BOOST_AUTO_TEST_CASE( link_delay_permission_change_test ) { try {
   validating_tester chain;

   const auto& tester_account = "tester"_n;

   create_accounts(chain);

   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first"), 10))
   );
   chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("code", sysio_token)
           ("type", "transfer")
           ("requirement", "first"));

   chain.produce_blocks();
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
           ("issuer", sysio_token )
           ("maximum_supply", "9000000.0000 CUR" )
   );

   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       sysio_token)
           ("quantity", "1000000.0000 CUR")
           ("memo", "for stuff")
   );

   auto trace = chain.push_action("sysio.token"_n, name("transfer"), "sysio.token"_n, fc::mutable_variant_object()
       ("from", sysio_token)
       ("to", "tester")
       ("quantity", "100.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   auto liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name proposal_1_name     = "prop1"_n;
   constexpr uint32_t delay_seconds = 10;
   constexpr auto quantity          = "1.0000 CUR";
   propose_approve_msig_token_transfer_trx(chain, proposal_1_name, "tester"_n, delay_seconds, quantity);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name proposal_2_name     = "prop2"_n;
   constexpr uint32_t delay_seconds_2 = 10;
   propose_approve_msig_updateauth_trx(chain, proposal_2_name, "tester"_n, delay_seconds_2);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   chain.produce_blocks(16);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name proposal_3_name     = "prop3"_n;
   constexpr uint32_t delay_seconds_3 = 10;
   constexpr auto quantity_3          = "5.0000 CUR";
   propose_approve_msig_token_transfer_trx(chain, proposal_3_name, "tester"_n, delay_seconds_3, quantity_3);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // first transfer will finally be performed
   exec_msig_trx(chain, proposal_1_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   // delayed update auth removing the delay will finally execute
   exec_msig_trx(chain, proposal_2_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   // this transfer is performed right away since delay is removed
   trace = chain.push_action("sysio.token"_n, name("transfer"), "tester"_n, fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "10.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   chain.produce_blocks(15);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   // second transfer finally is performed
   exec_msig_trx(chain, proposal_3_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("84.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("16.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() }/// link_delay_permission_change_test

// test removing delay on permission based on heirarchy delay
BOOST_AUTO_TEST_CASE( link_delay_permission_change_with_delay_heirarchy_test ) { try {
   validating_tester chain;

   const auto& tester_account = "tester"_n;

   create_accounts(chain);

   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first"), 10))
   );
   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "second")
           ("parent", "first")
           ("auth",  authority(chain.get_public_key(tester_account, "second")))
   );
   chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("code", sysio_token)
           ("type", "transfer")
           ("requirement", "second"));

   chain.produce_blocks();
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
           ("issuer", sysio_token)
           ("maximum_supply", "9000000.0000 CUR" )
   );

   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       sysio_token)
           ("quantity", "1000000.0000 CUR")
           ("memo", "for stuff")
   );

   auto trace = chain.push_action("sysio.token"_n, name("transfer"), "sysio.token"_n, fc::mutable_variant_object()
       ("from", sysio_token)
       ("to", "tester")
       ("quantity", "100.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   auto liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name proposal_1_name     = "prop1"_n;
   constexpr uint32_t delay_seconds = 10;
   constexpr auto quantity          = "1.0000 CUR";
   propose_approve_msig_token_transfer_trx(chain, proposal_1_name, "tester"_n, delay_seconds, quantity);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name proposal_2_name     = "prop2"_n;
   constexpr uint32_t delay_seconds_2 = 10;
   propose_approve_msig_updateauth_trx(chain, proposal_2_name, "tester"_n, delay_seconds_2);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   chain.produce_blocks(16);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name proposal_3_name     = "prop3"_n;
   constexpr uint32_t delay_seconds_3 = 10;
   constexpr auto quantity_3          = "5.0000 CUR";
   propose_approve_msig_token_transfer_trx(chain, proposal_3_name, "tester"_n, delay_seconds_3, quantity_3);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // first transfer will finally be performed
   exec_msig_trx(chain, proposal_1_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   // delayed update auth removing the delay will finally execute
   exec_msig_trx(chain, proposal_2_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   // this transfer is performed right away since delay is removed
   trace = chain.push_action("sysio.token"_n, name("transfer"), "tester"_n, fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "10.0000 CUR")
       ("memo", "hi" )
   );

   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   chain.produce_blocks(14);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   // second transfer finally is performed
   exec_msig_trx(chain, proposal_3_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("84.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("16.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() }/// link_delay_permission_change_with_delay_heirarchy_test

// test moving link with delay on permission's parent
BOOST_AUTO_TEST_CASE( link_delay_link_change_heirarchy_test ) { try {
   validating_tester chain;
   const auto& tester_account = "tester"_n;

   create_accounts(chain);

   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "first")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "first"), 10))
   );
   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "second")
           ("parent", "first")
           ("auth",  authority(chain.get_public_key(tester_account, "first")))
   );
   chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("code", sysio_token)
           ("type", "transfer")
           ("requirement", "second"));
   chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
           ("account", "tester")
           ("permission", "third")
           ("parent", "active")
           ("auth",  authority(chain.get_public_key(tester_account, "third")))
   );

   chain.produce_blocks();
   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
           ("issuer", sysio_token)
           ("maximum_supply", "9000000.0000 CUR" )
   );

   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       sysio_token)
           ("quantity", "1000000.0000 CUR")
           ("memo", "for stuff")
   );

   auto trace = chain.push_action("sysio.token"_n, name("transfer"), "sysio.token"_n, fc::mutable_variant_object()
       ("from", sysio_token)
       ("to", "tester")
       ("quantity", "100.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   auto liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name first_trnsfr_propsal_name   = "prop1"_n;
   propose_approve_msig_token_transfer_trx(chain, first_trnsfr_propsal_name, "tester"_n, 10, "1.0000 CUR");

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("999900.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name linkauth_proposal_name  = "prop2"_n;
   propose_approve_msig_linkauth_trx(chain, linkauth_proposal_name, "third"_n, "tester"_n, 10);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   chain.produce_blocks(16);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // this transaction will be delayed 20 blocks
   constexpr name second_trnsfr_propsal_name      = "prop3"_n;
   propose_approve_msig_token_transfer_trx(chain, second_trnsfr_propsal_name, "tester"_n, 10, "5.0000 CUR");

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("100.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("0.0000 CUR"), liquid_balance);

   // first transfer will finally be performed
   exec_msig_trx(chain, first_trnsfr_propsal_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("99.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("1.0000 CUR"), liquid_balance);

   // delay on minimum permission of transfer is finally removed
   exec_msig_trx(chain, linkauth_proposal_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   // this transfer is performed right away since delay is removed
   trace = chain.push_action("sysio.token"_n, name("transfer"), "tester"_n, fc::mutable_variant_object()
       ("from", "tester")
       ("to", "tester2")
       ("quantity", "10.0000 CUR")
       ("memo", "hi" )
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   chain.produce_blocks(16);

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("89.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("11.0000 CUR"), liquid_balance);

   // second transfer finally is performed
   exec_msig_trx(chain, second_trnsfr_propsal_name, {{ "tester"_n, config::active_name }});
   chain.produce_blocks();

   liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("84.0000 CUR"), liquid_balance);
   liquid_balance = get_currency_balance(chain, "tester2"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("16.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() } /// link_delay_link_change_heirarchy_test

BOOST_AUTO_TEST_CASE( max_transaction_delay_execute ) { try {
   //assuming max transaction delay is 45 days (default in config.hpp)
   validating_tester chain;
   const auto& tester_account = "tester"_n;

   create_accounts(chain);

   chain.push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
           ("issuer", "sysio.token" )
           ("maximum_supply", "9000000.0000 CUR" )
   );
   chain.push_action("sysio.token"_n, name("issue"), "sysio.token"_n, fc::mutable_variant_object()
           ("to",       "tester")
           ("quantity", "100.0000 CUR")
           ("memo", "for stuff")
   );

   //create a permission level with delay 30 days and associate it with token transfer
   auto trace = chain.push_action(config::system_account_name, updateauth::get_name(), tester_account, fc::mutable_variant_object()
                     ("account", "tester")
                     ("permission", "first")
                     ("parent", "active")
                     ("auth",  authority(chain.get_public_key(tester_account, "first"), 30*86400)) // 30 days delay
   );
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   trace = chain.push_action(config::system_account_name, linkauth::get_name(), tester_account, fc::mutable_variant_object()
                     ("account", "tester")
                     ("code", "sysio.token")
                     ("type", "transfer")
                     ("requirement", "first"));
   BOOST_REQUIRE_EQUAL(transaction_receipt::executed, trace->receipt->status);

   chain.produce_blocks();

   //change max_transaction_delay to 60 sec
   auto params = chain.control->get_global_properties().configuration;
   params.max_transaction_delay = 60;
   chain.push_action( config::system_account_name, "setparams"_n, config::system_account_name, mutable_variant_object()
                        ("params", params) );

   chain.produce_blocks();
   //should be able to create a msig transaction with delay 60 sec, despite permission delay being 30 days, because max_transaction_delay is 60 sec
   constexpr name proposal_name = "prop1"_n;
   propose_approve_msig_token_transfer_trx(chain, proposal_name, "tester"_n, 60, "9.0000 CUR");

   //check that the delayed msig transaction can be executed after after 60 sec
   chain.produce_blocks(120);
   exec_msig_trx(chain, proposal_name, {{ "tester"_n, config::active_name },{"tester"_n, config::sysio_payer_name }});

   //check that the transfer really happened
   auto liquid_balance = get_currency_balance(chain, "tester"_n);
   BOOST_REQUIRE_EQUAL(asset::from_string("91.0000 CUR"), liquid_balance);

} FC_LOG_AND_RETHROW() } /// max_transaction_delay_execute

BOOST_AUTO_TEST_CASE( test_blockchain_params_enabled ) { try {
   //since validation_tester activates all features here we will test how setparams works without
   //blockchain_parameters enabled
   tester chain( setup_policy::preactivate_feature_and_new_bios );

   //change max_transaction_delay to 60 sec
   auto params = chain.control->get_global_properties().configuration;
   params.max_transaction_delay = 60;
   chain.push_action(config::system_account_name,
                     "setparams"_n,
                     config::system_account_name,
                     mutable_variant_object()("params", params) );

   BOOST_CHECK_EQUAL(chain.control->get_global_properties().configuration.max_transaction_delay, 60u);

   chain.produce_blocks();

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
