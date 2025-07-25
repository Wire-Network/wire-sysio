#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop
#include <boost/algorithm/string/predicate.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/generated_transaction_object.hpp>

#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

class currency_tester : public validating_tester {
   public:

      auto push_action(const account_name& signer, const action_name &name, const variant_object &data ) {
         string action_type_name = abi_ser.get_action_type(name);

         action act;
         act.account = "sysio.token"_n;
         act.name = name;
         act.authorization = vector<permission_level>{{signer, config::active_name}, {signer, config::sysio_payer_name}};
         act.data = abi_ser.variant_to_binary(action_type_name, data, abi_serializer::create_yield_function( abi_serializer_max_time ));

         signed_transaction trx;
         trx.actions.emplace_back(std::move(act));

         set_transaction_headers(trx);
         trx.sign(get_private_key(signer, "active"), control->get_chain_id());
         return push_transaction(trx);
      }

      asset get_balance(const account_name& account) const {
         return get_currency_balance("sysio.token"_n, symbol(SY(4,CUR)), account);
      }

      auto transfer(const account_name& from, const account_name& to, const std::string& quantity, const std::string& memo = "") {
         auto trace = push_action(from, "transfer"_n, mutable_variant_object()
                                  ("from",     from)
                                  ("to",       to)
                                  ("quantity", quantity)
                                  ("memo",     memo)
                                  );
         produce_block();
         return trace;
      }

      auto issue(const account_name& to, const std::string& quantity, const std::string& memo = "") {
         auto trace = push_action("sysio.token"_n, "issue"_n, mutable_variant_object()
                                  ("to",       to)
                                  ("quantity", quantity)
                                  ("memo",     memo)
                                  );
         produce_block();
         return trace;
      }

      currency_tester(setup_policy p = setup_policy::full)
         :validating_tester({}, nullptr, p), abi_ser(json::from_string(test_contracts::sysio_token_abi()).as<abi_def>(), abi_serializer::create_yield_function( abi_serializer_max_time ))
      {
         create_account( "sysio.token"_n);
         set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );

         auto result = push_action("sysio.token"_n, "create"_n, mutable_variant_object()
                 ("issuer",       sysio_token)
                 ("maximum_supply", "1000000000.0000 CUR")
                 ("can_freeze", 0)
                 ("can_recall", 0)
                 ("can_whitelist", 0)
         );
         wdump((result));

         result = push_action("sysio.token"_n, "issue"_n, mutable_variant_object()
                 ("to",       sysio_token)
                 ("quantity", "1000000.0000 CUR")
                 ("memo", "gggggggggggg")
         );
         wdump((result));
         produce_block();
      }

      abi_serializer abi_ser;
      static const name sysio_token;
};

class pre_disable_deferred_trx_currency_tester : public currency_tester {
   public:
      pre_disable_deferred_trx_currency_tester() : currency_tester(setup_policy::full_except_do_not_disable_deferred_trx) {}
};

const name currency_tester::sysio_token = "sysio.token"_n;

BOOST_AUTO_TEST_SUITE(currency_tests)

BOOST_AUTO_TEST_CASE( bootstrap ) try {
   auto expected = asset::from_string( "1000000.0000 CUR" );
   currency_tester t;
   auto actual = t.get_currency_balance("sysio.token"_n, expected.get_symbol(), "sysio.token"_n);
   BOOST_REQUIRE_EQUAL(expected, actual);
} FC_LOG_AND_RETHROW() /// test_api_bootstrap

BOOST_FIXTURE_TEST_CASE( test_transfer, currency_tester ) try {
   create_accounts( {"alice"_n} );

   // make a transfer from the contract to a user
   {
      auto trace = push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
         ("from", sysio_token)
         ("to",   "alice")
         ("quantity", "100.0000 CUR")
         ("memo", "fund Alice")
      );

      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "100.0000 CUR" ) );
   }
} FC_LOG_AND_RETHROW() /// test_transfer

BOOST_FIXTURE_TEST_CASE( test_duplicate_transfer, currency_tester ) {
   create_accounts( {"alice"_n} );

   auto trace = push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
      ("from", sysio_token)
      ("to",   "alice")
      ("quantity", "100.0000 CUR")
      ("memo", "fund Alice")
   );

   BOOST_REQUIRE_THROW(push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
                                    ("from", sysio_token)
                                    ("to",   "alice")
                                    ("quantity", "100.0000 CUR")
                                    ("memo", "fund Alice")),
                       tx_duplicate);

   produce_block();

   BOOST_CHECK_EQUAL(true, chain_has_transaction(trace->id));
   BOOST_CHECK_EQUAL(get_balance("alice"_n), asset::from_string( "100.0000 CUR" ) );
}

BOOST_FIXTURE_TEST_CASE( test_addtransfer, currency_tester ) try {
   create_accounts( {"alice"_n} );

   // make a transfer from the contract to a user
   {
      auto trace = push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
         ("from", sysio_token)
         ("to",   "alice")
         ("quantity", "100.0000 CUR")
         ("memo", "fund Alice")
      );

      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "100.0000 CUR" ));
   }

   // make a transfer from the contract to a user
   {
      auto trace = push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
         ("from", sysio_token)
         ("to",   "alice")
         ("quantity", "10.0000 CUR")
         ("memo", "add Alice")
      );

      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "110.0000 CUR" ));
   }
} FC_LOG_AND_RETHROW() /// test_transfer


BOOST_FIXTURE_TEST_CASE( test_overspend, currency_tester ) try {
   create_accounts( {"alice"_n, "bob"_n} );

   // make a transfer from the contract to a user
   {
      auto trace = push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
         ("from", sysio_token)
         ("to",   "alice")
         ("quantity", "100.0000 CUR")
         ("memo", "fund Alice")
      );

      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "100.0000 CUR" ));
   }

   // Overspend!
   {
      variant_object data = mutable_variant_object()
         ("from", "alice")
         ("to",   "bob")
         ("quantity", "101.0000 CUR")
         ("memo", "overspend! Alice");

      BOOST_CHECK_EXCEPTION( push_action("alice"_n, "transfer"_n, data),
                             sysio_assert_message_exception, sysio_assert_message_is("overdrawn balance") );
      produce_block();

      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "100.0000 CUR" ));
      BOOST_REQUIRE_EQUAL(get_balance("bob"_n), asset::from_string( "0.0000 CUR" ));
   }
} FC_LOG_AND_RETHROW() /// test_overspend

BOOST_FIXTURE_TEST_CASE( test_fullspend, currency_tester ) try {
   create_accounts( {"alice"_n, "bob"_n} );

   // make a transfer from the contract to a user
   {
      auto trace = push_action("sysio.token"_n, "transfer"_n, mutable_variant_object()
         ("from", sysio_token)
         ("to",   "alice")
         ("quantity", "100.0000 CUR")
         ("memo", "fund Alice")
      );

      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "100.0000 CUR" ));
   }

   // Full spend
   {
      variant_object data = mutable_variant_object()
         ("from", "alice")
         ("to",   "bob")
         ("quantity", "100.0000 CUR")
         ("memo", "all in! Alice");

      auto trace = push_action("alice"_n, "transfer"_n, data);
      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_REQUIRE_EQUAL(get_balance("alice"_n), asset::from_string( "0.0000 CUR" ));
      BOOST_REQUIRE_EQUAL(get_balance("bob"_n), asset::from_string( "100.0000 CUR" ));
   }

} FC_LOG_AND_RETHROW() /// test_fullspend



BOOST_FIXTURE_TEST_CASE(test_symbol, validating_tester) try {

   {
      symbol dollar(2, "DLLR");
      BOOST_REQUIRE_EQUAL(SY(2, DLLR), dollar.value());
      BOOST_REQUIRE_EQUAL(2, dollar.decimals());
      BOOST_REQUIRE_EQUAL(100, dollar.precision());
      BOOST_REQUIRE_EQUAL("DLLR", dollar.name());
      BOOST_REQUIRE_EQUAL(true, dollar.valid());
   }

   {
      symbol sys(4, "SYS");
      BOOST_REQUIRE_EQUAL(SY(4,SYS), sys.value());
      BOOST_REQUIRE_EQUAL("4,SYS", sys.to_string());
      BOOST_REQUIRE_EQUAL("SYS", sys.name());
      BOOST_REQUIRE_EQUAL(4, sys.decimals());
   }

   // default is "4,${CORE_SYMBOL_NAME}"
   {
      symbol def;
      BOOST_REQUIRE_EQUAL(4, def.decimals());
      BOOST_REQUIRE_EQUAL(CORE_SYMBOL_NAME, def.name());
   }
   // from string
   {
      symbol y = symbol::from_string("3,YEN");
      BOOST_REQUIRE_EQUAL(3, y.decimals());
      BOOST_REQUIRE_EQUAL("YEN", y.name());
   }

   // from empty string
   {
      BOOST_CHECK_EXCEPTION(symbol::from_string(""),
                            symbol_type_exception, fc_exception_message_is("creating symbol from empty string"));
   }

   // precision part missing
   {
      BOOST_CHECK_EXCEPTION(symbol::from_string("RND"),
                            symbol_type_exception, fc_exception_message_is("missing comma in symbol"));
   }

   // 0 decimals part
   {
      symbol sym = symbol::from_string("0,EURO");
      BOOST_REQUIRE_EQUAL(0, sym.decimals());
      BOOST_REQUIRE_EQUAL("EURO", sym.name());
   }

   // invalid - contains lower case characters, no validation
   {
      BOOST_CHECK_EXCEPTION(symbol malformed(SY(6,Sys)),
                            symbol_type_exception, fc_exception_message_is("invalid symbol: Sys"));
   }

   // invalid - contains lower case characters, exception thrown
   {
      BOOST_CHECK_EXCEPTION(symbol(5,"Sys"),
                            symbol_type_exception, fc_exception_message_is("invalid character in symbol name"));
   }

   // Missing decimal point, should create asset with 0 decimals
   {
      asset a = asset::from_string("10 CUR");
      BOOST_REQUIRE_EQUAL(a.get_amount(), 10);
      BOOST_REQUIRE_EQUAL(a.precision(), 1);
      BOOST_REQUIRE_EQUAL(a.decimals(), 0);
      BOOST_REQUIRE_EQUAL(a.symbol_name(), "CUR");
   }

   // Missing space
   {
      BOOST_CHECK_EXCEPTION(asset::from_string("10CUR"),
                            asset_type_exception, fc_exception_message_is("Asset's amount and symbol should be separated with space"));
   }

   // Precision is not specified when decimal separator is introduced
   {
      BOOST_CHECK_EXCEPTION(asset::from_string("10. CUR"),
                            asset_type_exception, fc_exception_message_is("Missing decimal fraction after decimal point"));
   }

   // Missing symbol
   {
      BOOST_CHECK_EXCEPTION(asset::from_string("10"),
                            asset_type_exception, fc_exception_message_is("Asset's amount and symbol should be separated with space"));
   }

   // Multiple spaces
   {
      asset a = asset::from_string("1000000000.00000  CUR");
      BOOST_REQUIRE_EQUAL(a.get_amount(), 100000000000000);
      BOOST_REQUIRE_EQUAL(a.decimals(), 5);
      BOOST_REQUIRE_EQUAL(a.symbol_name(), "CUR");
      BOOST_REQUIRE_EQUAL(a.to_string(), "1000000000.00000 CUR");
   }

   // Valid asset
   {
      asset a = asset::from_string("1000000000.00000 CUR");
      BOOST_REQUIRE_EQUAL(a.get_amount(), 100000000000000);
      BOOST_REQUIRE_EQUAL(a.decimals(), 5);
      BOOST_REQUIRE_EQUAL(a.symbol_name(), "CUR");
      BOOST_REQUIRE_EQUAL(a.to_string(), "1000000000.00000 CUR");
   }

   // Negative asset
   {
      asset a = asset::from_string("-001000000.00010 CUR");
      BOOST_REQUIRE_EQUAL(a.get_amount(), -100000000010);
      BOOST_REQUIRE_EQUAL(a.decimals(), 5);
      BOOST_REQUIRE_EQUAL(a.symbol_name(), "CUR");
      BOOST_REQUIRE_EQUAL(a.to_string(), "-1000000.00010 CUR");
   }

   // Negative asset below 1
   {
      asset a = asset::from_string("-000000000.00100 CUR");
      BOOST_REQUIRE_EQUAL(a.get_amount(), -100);
      BOOST_REQUIRE_EQUAL(a.decimals(), 5);
      BOOST_REQUIRE_EQUAL(a.symbol_name(), "CUR");
      BOOST_REQUIRE_EQUAL(a.to_string(), "-0.00100 CUR");
   }

   // Negative asset below 1
   {
      asset a = asset::from_string("-0.0001 PPP");
      BOOST_REQUIRE_EQUAL(a.get_amount(), -1);
      BOOST_REQUIRE_EQUAL(a.decimals(), 4);
      BOOST_REQUIRE_EQUAL(a.symbol_name(), "PPP");
      BOOST_REQUIRE_EQUAL(a.to_string(), "-0.0001 PPP");
   }

} FC_LOG_AND_RETHROW() /// test_symbol

BOOST_FIXTURE_TEST_CASE( test_input_quantity, currency_tester ) try {

   produce_blocks(2);

   create_accounts( {"alice"_n, "bob"_n, "carl"_n} );

   // transfer to alice using right precision
   {
      auto trace = transfer(sysio_token, "alice"_n, "100.0000 CUR");

      BOOST_CHECK_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_CHECK_EQUAL(asset::from_string( "100.0000 CUR"), get_balance("alice"_n));
      BOOST_CHECK_EQUAL(1000000, get_balance("alice"_n).get_amount());
   }

   // transfer using different symbol name fails
   {
      BOOST_REQUIRE_THROW(transfer("alice"_n, "carl"_n, "20.50 USD"), sysio_assert_message_exception);
   }

   // issue to alice using right precision
   {
      auto trace = issue("alice"_n, "25.0256 CUR");

      BOOST_CHECK_EQUAL(true, chain_has_transaction(trace->id));
      BOOST_CHECK_EQUAL(asset::from_string("125.0256 CUR"), get_balance("alice"_n));
   }


} FC_LOG_AND_RETHROW() /// test_currency

BOOST_AUTO_TEST_SUITE_END()
