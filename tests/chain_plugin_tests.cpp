#include <boost/test/unit_test.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/wasm_sysio_constraints.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>

#include <test_contracts.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/io/fstream.hpp>

#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

static auto get_account_full = [](chain_apis::read_only& plugin,
                                  chain_apis::read_only::get_account_params& params,
                                  const fc::time_point& deadline) -> chain_apis::read_only::get_account_results {
   auto res =  plugin.get_account(params, deadline)();
   BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(res));
   return std::get<chain_apis::read_only::get_account_results>(std::move(res));
};

BOOST_AUTO_TEST_SUITE(chain_plugin_tests)

BOOST_FIXTURE_TEST_CASE( get_block_with_invalid_abi, validating_tester ) try {
   produce_block();

   create_accounts( {"asserter"_n} );
   produce_block();

   // setup contract and abi
   set_code( "asserter"_n, test_contracts::asserter_wasm() );
   set_abi( "asserter"_n, test_contracts::asserter_abi() );
   produce_block();

   auto resolver = [&,this]( const account_name& name ) -> std::optional<abi_serializer> {
      try {
         if (const auto* accnt = this->control->find_account_metadata( name ); accnt != nullptr) {
            if (abi_def abi; abi_serializer::to_abi(accnt->abi, abi)) {
               return abi_serializer(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
            }
         }
         return std::optional<abi_serializer>();
      } FC_RETHROW_EXCEPTIONS(error, "resolver failed at chain_plugin_tests::abi_invalid_type");
   };

   // abi should be resolved
   BOOST_REQUIRE_EQUAL(true, resolver("asserter"_n).has_value());

   // make an action using the valid contract & abi
   fc::variant pretty_trx = mutable_variant_object()
      ("actions", variants({
         mutable_variant_object()
            ("account", "asserter")
            ("name", "procassert")
            ("authorization", variants({
               mutable_variant_object()
                  ("actor", "asserter")
                  ("permission", name(config::active_name).to_string())
            }))
            ("data", mutable_variant_object()
               ("condition", 1)
               ("message", "Should Not Assert!")
            )
         })
      );
   signed_transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
   set_transaction_headers(trx);
   trx.sign( get_private_key( "asserter"_n, "active" ), control->get_chain_id() );
   push_transaction( trx );
   produce_block();

   // retrieve block num
   uint32_t headnum = this->control->head().block_num();

   char headnumstr[20];
   sprintf(headnumstr, "%d", headnum);
   chain_apis::read_only::get_raw_block_params param{headnumstr};
   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // block should be decoded successfully
   auto block = plugin.get_raw_block(param, fc::time_point::maximum());
   auto abi_cache = plugin.get_block_serializers(block, fc::microseconds::maximum());
   std::string block_str = json::to_pretty_string(plugin.convert_block(block, abi_cache));
   BOOST_TEST(block_str.find("procassert") != std::string::npos);
   BOOST_TEST(block_str.find("condition") != std::string::npos);
   BOOST_TEST(block_str.find("Should Not Assert!") != std::string::npos);
   BOOST_TEST(block_str.find("011253686f756c64204e6f742041737365727421") != std::string::npos); //action data

   // set an invalid abi (int8->xxxx)
   std::string abi2 = test_contracts::asserter_abi();
   auto pos = abi2.find("int8");
   BOOST_TEST(pos != std::string::npos);
   abi2.replace(pos, 4, "xxxx");
   set_abi("asserter"_n, abi2.c_str());
   produce_block();

   // resolving the invalid abi result in exception
   BOOST_CHECK_THROW(resolver("asserter"_n), invalid_type_inside_abi);

   // get the same block as string, results in decode failed(invalid abi) but not exception
   auto block2 = plugin.get_raw_block(param, fc::time_point::maximum());
   auto abi_cache2 = plugin.get_block_serializers(block2, fc::microseconds::maximum());
   std::string block_str2 = json::to_pretty_string(plugin.convert_block(block2, abi_cache2));
   BOOST_TEST(block_str2.find("procassert") != std::string::npos);
   BOOST_TEST(block_str2.find("condition") == std::string::npos); // decode failed
   BOOST_TEST(block_str2.find("Should Not Assert!") == std::string::npos); // decode failed
   BOOST_TEST(block_str2.find("011253686f756c64204e6f742041737365727421") != std::string::npos); //action data


   chain_apis::read_only::get_block_header_params bh_param{headnumstr, false};
   auto get_bh_result = plugin.get_block_header(bh_param, fc::time_point::maximum());

   BOOST_TEST(get_bh_result.id == block->calculate_id());
   BOOST_TEST(json::to_string(get_bh_result.signed_block_header, fc::time_point::maximum()) ==
              json::to_string(fc::variant{static_cast<const signed_block_header&>(*block)}, fc::time_point::maximum()));

} FC_LOG_AND_RETHROW() /// get_block_with_invalid_abi

// A block_num_or_id that is a decimal integer names a block by number. One that does not fit uint32_t names no
// block: it is an unknown block, never the block its low 32 bits name.
BOOST_FIXTURE_TEST_CASE( block_num_string_outside_uint32_is_unknown_block, validating_tester ) try {
   produce_blocks(5);
   const uint32_t    head_num = control->head().block_num();
   const std::string head_id  = control->head().id().str();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*control, {}, {}, _tracked_votes, fc::microseconds::maximum(),
                                fc::microseconds::maximum(), nullptr);
   const fc::time_point deadline = fc::time_point::maximum();

   // Every endpoint that takes a block_num_or_id string, reduced to the number of the block it returns.
   using lookup = std::function<uint32_t(const std::string&)>;
   const std::vector<std::pair<std::string, lookup>> endpoints{
      {"get_raw_block", [&](const std::string& block_num_or_id) {
         const chain_apis::read_only::get_raw_block_params params{block_num_or_id};
         return plugin.get_raw_block(params, deadline)->block_num();
      }},
      {"get_block", [&](const std::string& block_num_or_id) {
         const chain_apis::read_only::get_block_params params{block_num_or_id};
         auto result = plugin.get_block(params, deadline)();
         BOOST_REQUIRE(std::holds_alternative<fc::variant>(result));
         return std::get<fc::variant>(result)["block_num"].as<uint32_t>();
      }},
      {"get_block_header", [&](const std::string& block_num_or_id) {
         const chain_apis::read_only::get_block_header_params params{block_num_or_id, false};
         return block_header::num_from_id(plugin.get_block_header(params, deadline).id);
      }},
      {"get_block_header with extensions", [&](const std::string& block_num_or_id) {
         const chain_apis::read_only::get_block_header_params params{block_num_or_id, true};
         return block_header::num_from_id(plugin.get_block_header(params, deadline).id);
      }},
      {"get_block_header_state", [&](const std::string& block_num_or_id) {
         const chain_apis::read_only::get_block_header_state_params params{block_num_or_id};
         return plugin.get_block_header_state(params, deadline)["block_num"].as<uint32_t>();
      }},
   };

   const uint64_t uint32_max = std::numeric_limits<uint32_t>::max();
   const uint64_t uint64_max = std::numeric_limits<uint64_t>::max();

   // {block_num_or_id, number of the block it names; none when it names no block}
   const std::vector<std::pair<std::string, std::optional<uint32_t>>> cases{
      {"1", 1},                                                  // in range, exists
      {std::to_string(head_num), head_num},                      // in range, exists
      {head_id, head_num},                                       // a block id
      {std::to_string(head_num + 1), std::nullopt},              // in range, does not exist
      {std::to_string(uint32_max), std::nullopt},                // 4294967295: the largest block number
      {std::to_string(uint32_max + 1), std::nullopt},            // 4294967296: low 32 bits are 0
      {std::to_string(uint32_max + 2), std::nullopt},            // 4294967297: low 32 bits name block 1
      {std::to_string(uint32_max + 1 + head_num), std::nullopt}, // low 32 bits name the head block
      {std::to_string(uint64_max), std::nullopt},                // low 32 bits are 4294967295
   };

   // Neither a number nor a block id. A minus sign makes no block number: "-4294967295" is 1 modulo 2^32 and
   // "-18446744073709551615" is 1 modulo 2^64.
   const std::vector<std::string> not_block_num_or_id{
      "nope", "-1", "-" + std::to_string(uint32_max), "-" + std::to_string(uint64_max)};

   for (const auto& [endpoint, lookup_block_num] : endpoints) {
      for (const auto& [block_num_or_id, named] : cases) {
         BOOST_TEST_CONTEXT(endpoint << " " << block_num_or_id) {
            if (named) {
               BOOST_CHECK_EQUAL(lookup_block_num(block_num_or_id), *named);
            } else {
               BOOST_CHECK_THROW(lookup_block_num(block_num_or_id), unknown_block_exception);
            }
         }
      }
      for (const std::string& block_num_or_id : not_block_num_or_id) {
         BOOST_TEST_CONTEXT(endpoint << " " << block_num_or_id) {
            BOOST_CHECK_THROW(lookup_block_num(block_num_or_id), block_id_type_exception);
         }
      }
   }
} FC_LOG_AND_RETHROW() /// block_num_string_outside_uint32_is_unknown_block

BOOST_AUTO_TEST_CASE( get_consensus_parameters ) try {
   tester t{setup_policy::preactivate_feature_and_new_bios};
   t.produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(t.control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), nullptr);

   auto parms = plugin.get_consensus_parameters({}, fc::time_point::maximum());

   chain_config_v0 v0config;
   from_variant(parms.chain_config, v0config);

   // verifying chain_config
   BOOST_TEST(v0config.max_block_cpu_usage == t.control->get_global_properties().configuration.max_block_cpu_usage);
   BOOST_TEST(v0config.target_block_net_usage_pct == t.control->get_global_properties().configuration.target_block_net_usage_pct);
   BOOST_TEST(v0config.max_transaction_net_usage == t.control->get_global_properties().configuration.max_transaction_net_usage);
   BOOST_TEST(v0config.net_usage_leeway == t.control->get_global_properties().configuration.net_usage_leeway);
   BOOST_TEST(v0config.max_block_cpu_usage == t.control->get_global_properties().configuration.max_block_cpu_usage);
   BOOST_TEST(v0config.target_block_cpu_usage_pct == t.control->get_global_properties().configuration.target_block_cpu_usage_pct);
   BOOST_TEST(v0config.max_transaction_cpu_usage == t.control->get_global_properties().configuration.max_transaction_cpu_usage);
   BOOST_TEST(v0config.min_transaction_cpu_usage == t.control->get_global_properties().configuration.min_transaction_cpu_usage);
   BOOST_TEST(v0config.max_transaction_lifetime == t.control->get_global_properties().configuration.max_transaction_lifetime);
   BOOST_TEST(v0config.max_transaction_delay == t.control->get_global_properties().configuration.max_transaction_delay);
   BOOST_TEST(v0config.max_inline_action_size == t.control->get_global_properties().configuration.max_inline_action_size);
   BOOST_TEST(v0config.max_inline_action_depth == t.control->get_global_properties().configuration.max_inline_action_depth);
   BOOST_TEST(v0config.max_authority_depth == t.control->get_global_properties().configuration.max_authority_depth);
   BOOST_TEST(v0config.max_action_return_value_size == t.control->get_global_properties().configuration.max_action_return_value_size);
   BOOST_TEST(v0config.max_kv_key_size == t.control->get_global_properties().configuration.max_kv_key_size);
   BOOST_TEST(v0config.max_kv_value_size == t.control->get_global_properties().configuration.max_kv_value_size);
   BOOST_TEST(v0config.max_kv_secondary_key_size == t.control->get_global_properties().configuration.max_kv_secondary_key_size);
   BOOST_TEST(v0config.notify_cpu_stipend == t.control->get_global_properties().configuration.notify_cpu_stipend);

   BOOST_TEST(!!parms.wasm_config); // enabled at genesis for Wire

   parms = plugin.get_consensus_parameters({}, fc::time_point::maximum());

   BOOST_REQUIRE(!!parms.wasm_config);

   // verifying wasm_config
   BOOST_TEST(parms.wasm_config->max_mutable_global_bytes == t.control->get_global_properties().wasm_configuration.max_mutable_global_bytes);
   BOOST_TEST(parms.wasm_config->max_table_elements == t.control->get_global_properties().wasm_configuration.max_table_elements);
   BOOST_TEST(parms.wasm_config->max_section_elements == t.control->get_global_properties().wasm_configuration.max_section_elements);
   BOOST_TEST(parms.wasm_config->max_linear_memory_init == t.control->get_global_properties().wasm_configuration.max_linear_memory_init);
   BOOST_TEST(parms.wasm_config->max_func_local_bytes == t.control->get_global_properties().wasm_configuration.max_func_local_bytes);
   BOOST_TEST(parms.wasm_config->max_nested_structures == t.control->get_global_properties().wasm_configuration.max_nested_structures);
   BOOST_TEST(parms.wasm_config->max_symbol_bytes == t.control->get_global_properties().wasm_configuration.max_symbol_bytes);
   BOOST_TEST(parms.wasm_config->max_module_bytes == t.control->get_global_properties().wasm_configuration.max_module_bytes);
   BOOST_TEST(parms.wasm_config->max_code_bytes == t.control->get_global_properties().wasm_configuration.max_code_bytes);
   BOOST_TEST(parms.wasm_config->max_pages == t.control->get_global_properties().wasm_configuration.max_pages);
   BOOST_TEST(parms.wasm_config->max_call_depth == t.control->get_global_properties().wasm_configuration.max_call_depth);

} FC_LOG_AND_RETHROW() //get_consensus_parameters

BOOST_FIXTURE_TEST_CASE( get_account, validating_tester ) try {
   produce_block();

   std::vector<account_name> accs{{ "alice"_n, "bob"_n, "cindy"_n}};
   create_accounts(accs, false, false);

   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), nullptr);

   chain_apis::read_only::get_account_params p{"alice"_n};

   chain_apis::read_only::get_account_results result = get_account_full(plugin, p, fc::time_point::maximum());

   auto check_result_basic = [](chain_apis::read_only::get_account_results result, sysio::name nm, bool isPriv) {
      BOOST_REQUIRE_EQUAL(nm, result.account_name);
      BOOST_REQUIRE_EQUAL(isPriv, result.privileged);

      BOOST_REQUIRE_EQUAL(2u, result.permissions.size());
      if (result.permissions.size() > 1u) {
         auto perm = result.permissions[0];
         BOOST_REQUIRE_EQUAL(name("active"_n), perm.perm_name);
         BOOST_REQUIRE_EQUAL(name("owner"_n), perm.parent);
         auto auth = perm.required_auth;
         BOOST_REQUIRE_EQUAL(1u, auth.threshold);
         BOOST_REQUIRE_EQUAL(1u, auth.keys.size());
         BOOST_REQUIRE_EQUAL(0u, auth.accounts.size());


         perm = result.permissions[1];
         BOOST_REQUIRE_EQUAL(name("owner"_n), perm.perm_name);
         BOOST_REQUIRE_EQUAL(name(""_n), perm.parent);
         auth = perm.required_auth;
         BOOST_REQUIRE_EQUAL(1u, auth.threshold);
         BOOST_REQUIRE_EQUAL(1u, auth.keys.size());
         BOOST_REQUIRE_EQUAL(0u, auth.accounts.size());

      }
   };

   check_result_basic(result, name("alice"_n), false);

   for (auto perm : result.permissions) {
      BOOST_REQUIRE_EQUAL(true, perm.linked_actions.has_value());
      if (perm.linked_actions.has_value())
         BOOST_REQUIRE_EQUAL(0u, perm.linked_actions->size());
   }
   BOOST_REQUIRE_EQUAL(0u, result.sysio_any_linked_actions.size());

   // test link authority
   link_authority(name("alice"_n), name("bob"_n), name("active"_n), name("foo"_n));
   produce_block();
   result = get_account_full(plugin, p, fc::time_point::maximum());

   check_result_basic(result, name("alice"_n), false);
   auto perm = result.permissions[0];
   BOOST_REQUIRE_EQUAL(1u, perm.linked_actions->size());
   if (perm.linked_actions->size() >= 1u) {
      auto la = (*perm.linked_actions)[0];
      BOOST_REQUIRE_EQUAL(name("bob"_n), la.account);
      BOOST_REQUIRE_EQUAL(true, la.action.has_value());
      if(la.action.has_value()) {
         BOOST_REQUIRE_EQUAL(name("foo"_n), la.action.value());
      }
   }
   BOOST_REQUIRE_EQUAL(0u, result.sysio_any_linked_actions.size());

   // test link authority to sysio.any
   link_authority(name("alice"_n), name("bob"_n), name("sysio.any"_n), name("foo"_n));
   produce_block();
   result = get_account_full(plugin, p, fc::time_point::maximum());
   check_result_basic(result, name("alice"_n), false);
   // active permission should no longer have linked auth, as sysio.any replaces it
   perm = result.permissions[0];
   BOOST_REQUIRE_EQUAL(0u, perm.linked_actions->size());

   auto sysio_any_la = result.sysio_any_linked_actions;
   BOOST_REQUIRE_EQUAL(1u, sysio_any_la.size());
   if (sysio_any_la.size() >= 1u) {
      auto la = sysio_any_la[0];
      BOOST_REQUIRE_EQUAL(name("bob"_n), la.account);
      BOOST_REQUIRE_EQUAL(true, la.action.has_value());
      if(la.action.has_value()) {
         BOOST_REQUIRE_EQUAL(name("foo"_n), la.action.value());
      }
   }
} FC_LOG_AND_RETHROW() /// get_account

BOOST_FIXTURE_TEST_CASE( key_alone_satisfies_follows_the_authority_checker, validating_tester ) try {
   produce_block();
   create_accounts({"alice"_n, "bob"_n, "carol"_n}, false, false);
   produce_block();

   const auto             alice_active = get_public_key("alice"_n, "active");
   const permission_level alice{"alice"_n, config::active_name};
   BOOST_CHECK(key_alone_satisfies(*control, alice, alice_active));
   // Declaring a permission takes that permission's own authority, not its parent's.
   BOOST_CHECK(key_alone_satisfies(*control, {"alice"_n, config::owner_name}, get_public_key("alice"_n, "owner")));
   BOOST_CHECK(!key_alone_satisfies(*control, alice, get_public_key("alice"_n, "owner")));
   BOOST_CHECK(!key_alone_satisfies(*control, {"alice"_n, "nosuch"_n}, alice_active));
   BOOST_CHECK(!key_alone_satisfies(*control, {"nosuch"_n, config::active_name}, alice_active));

   // A permission delegated to another account's permission is satisfied by that account's key.
   set_authority("bob"_n, config::active_name, authority(1, {}, {permission_level_weight{alice, 1}}));
   produce_block();
   BOOST_CHECK(key_alone_satisfies(*control, {"bob"_n, config::active_name}, alice_active));

   // Under threshold 2, a key of weight 1 is not enough alone; one of weight 2 is.
   const auto k1 = get_public_key("carol"_n, "k1");
   const auto k2 = get_public_key("carol"_n, "k2");
   authority  two_of_two(2, {key_weight{k1, 1}, key_weight{k2, 1}});
   two_of_two.sort_fields();
   set_authority("carol"_n, config::active_name, two_of_two);
   produce_block();
   BOOST_CHECK(!key_alone_satisfies(*control, {"carol"_n, config::active_name}, k1));
   authority heavy_k1(2, {key_weight{k1, 2}, key_weight{k2, 1}});
   heavy_k1.sort_fields();
   set_authority("carol"_n, config::active_name, heavy_k1);
   produce_block();
   BOOST_CHECK(key_alone_satisfies(*control, {"carol"_n, config::active_name}, k1));
   BOOST_CHECK(!key_alone_satisfies(*control, {"carol"_n, config::active_name}, k2));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( permission_satisfies_link_follows_linkauth, validating_tester ) try {
   produce_block();
   create_accounts({"alice"_n, "bob"_n}, false, false);
   produce_block();
   set_authority("alice"_n, "underwrite"_n, authority(get_public_key("alice"_n, "underwrite")), config::active_name);
   produce_block();

   const permission_level underwrite{"alice"_n, "underwrite"_n};
   const permission_level active{"alice"_n, config::active_name};
   // Nothing linked: an action needs active, which active and owner satisfy and a child of active does not.
   BOOST_CHECK(!permission_satisfies_link(*control, underwrite, "bob"_n, "accept"_n));
   BOOST_CHECK(permission_satisfies_link(*control, active, "bob"_n, "accept"_n));
   BOOST_CHECK(permission_satisfies_link(*control, {"alice"_n, config::owner_name}, "bob"_n, "accept"_n));

   link_authority("alice"_n, "bob"_n, "underwrite"_n, "accept"_n);
   produce_block();
   BOOST_CHECK(permission_satisfies_link(*control, underwrite, "bob"_n, "accept"_n));
   BOOST_CHECK(permission_satisfies_link(*control, active, "bob"_n, "accept"_n));
   BOOST_CHECK(!permission_satisfies_link(*control, underwrite, "bob"_n, "claim"_n));   // links are per action

   // Linked to sysio.any: any permission of the account may declare it.
   link_authority("alice"_n, "bob"_n, "sysio.any"_n, "anyact"_n);
   produce_block();
   BOOST_CHECK(permission_satisfies_link(*control, underwrite, "bob"_n, "anyact"_n));

   // A permission that does not exist satisfies nothing.
   BOOST_CHECK(!permission_satisfies_link(*control, {"alice"_n, "nosuch"_n}, "bob"_n, "accept"_n));
} FC_LOG_AND_RETHROW()

/// get_required_keys returns the keys of every declared permission however often and in whatever order actions
/// declare them, applies the payer rules to each action on its own, and stops at the request deadline.
BOOST_FIXTURE_TEST_CASE( get_required_keys_duplicate_declarations, validating_tester ) try {
   produce_block();
   create_accounts({"alice"_n, "bob"_n}, false, false);
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*control, {}, {}, _tracked_votes, fc::microseconds::maximum(),
                                fc::microseconds::maximum(), nullptr);
   const fc::time_point no_deadline = fc::time_point::maximum();

   const public_key_type alice_key = get_public_key("alice"_n, "active");
   const public_key_type bob_key   = get_public_key("bob"_n, "active");
   const public_key_type bob_owner = get_public_key("bob"_n, "owner");
   const public_key_type unrelated = get_public_key("nobody"_n, "active");
   const flat_set<public_key_type> all_keys{alice_key, bob_key, bob_owner, unrelated};

   const permission_level alice{"alice"_n, config::active_name};
   const permission_level bob{"bob"_n, config::active_name};
   const permission_level bob_as_owner{"bob"_n, config::owner_name};
   const permission_level bob_payer{"bob"_n, config::sysio_payer_name};

   using declarations = std::vector<std::vector<permission_level>>;
   const auto request = [&](const declarations& actions, const flat_set<public_key_type>& available) {
      signed_transaction trx;
      set_transaction_headers(trx);
      for (const std::vector<permission_level>& authorization : actions)
         trx.actions.emplace_back(authorization, "alice"_n, "doit"_n, bytes{});
      return chain_apis::read_only::get_required_keys_params{fc::variant(trx), available};
   };
   const auto required_keys = [&](const declarations& actions) {
      return plugin.get_required_keys(request(actions, all_keys), no_deadline).required_keys;
   };
   /// `copies` actions, alternating between the two authorization lists.
   const auto repeated = [](uint32_t copies, const std::vector<permission_level>& even,
                            const std::vector<permission_level>& odd) {
      declarations actions;
      for (uint32_t i = 0; i < copies; ++i)
         actions.push_back(i % 2 == 0 ? even : odd);
      return actions;
   };
   constexpr uint32_t many = 2000;

   BOOST_CHECK(required_keys({{alice}}) == (flat_set<public_key_type>{alice_key}));
   BOOST_CHECK(required_keys({{bob}}) == (flat_set<public_key_type>{bob_key}));
   BOOST_CHECK(required_keys({{bob_as_owner}}) == (flat_set<public_key_type>{bob_owner}));

   // Duplicates within and across actions, in either order, require each key once.
   const flat_set<public_key_type> alice_and_bob{alice_key, bob_key};
   BOOST_CHECK(required_keys({{alice, bob, alice}}) == alice_and_bob);
   BOOST_CHECK(required_keys(repeated(many, {alice, bob}, {bob, alice})) == alice_and_bob);
   BOOST_CHECK(required_keys(repeated(many, {bob}, {alice, bob_as_owner})) ==
              (flat_set<public_key_type>{alice_key, bob_key, bob_owner}));

   // A payer marker needs no key of its own; its pairing is checked on every action.
   BOOST_CHECK(required_keys({{bob_payer, bob}}) == (flat_set<public_key_type>{bob_key}));
   BOOST_CHECK(required_keys(repeated(many, {bob_payer, bob}, {alice})) == alice_and_bob);
   BOOST_CHECK_THROW(required_keys({{bob_payer, alice}, {bob}}), unsatisfied_authorization);
   BOOST_CHECK_THROW(required_keys(repeated(many, {bob_payer, bob}, {bob_payer, alice})), unsatisfied_authorization);
   BOOST_CHECK_THROW(required_keys({{alice, bob_payer, bob}}), irrelevant_auth_exception);
   BOOST_CHECK_THROW(required_keys({{bob_payer, bob_payer, bob}}), irrelevant_auth_exception);

   // A permission the available keys cannot satisfy fails however many times it is declared.
   BOOST_CHECK_THROW(plugin.get_required_keys(request(repeated(many, {alice}, {bob}), {alice_key}), no_deadline),
                     unsatisfied_authorization);

   // A deadline already passed stops the request.
   const fc::time_point passed = fc::time_point::now() - fc::microseconds(1);
   BOOST_CHECK_THROW(plugin.get_required_keys(request({{alice}}, all_keys), passed), deadline_exception);
   BOOST_CHECK_THROW(plugin.get_required_keys(request(repeated(many, {alice}, {bob}), all_keys), passed),
                     deadline_exception);
} FC_LOG_AND_RETHROW()

/// get_transaction_id reads each action from `hex_data` when it also has `data`, rejects malformed requests, and
/// converts a request in time proportional to its size.
BOOST_AUTO_TEST_CASE( get_transaction_id_many_actions ) try {
   using chain_apis::read_only;
   constexpr uint32_t action_count = 8000;
   // Converting a request may take at most this many times as long as parsing its JSON, which is linear in its size.
   constexpr int64_t max_convert_to_parse_ratio = 10;
   constexpr int     timing_runs                = 3;

   signed_transaction trx;
   trx.expiration       = fc::time_point_sec(fc::time_point::from_iso_string("2030-01-01T00:00:00"));
   trx.ref_block_num    = 1;
   trx.ref_block_prefix = 2;
   for (uint32_t i = 0; i < action_count; ++i)
      trx.actions.emplace_back(std::vector<permission_level>{{"alice"_n, config::active_name}}, "alice"_n, "doit"_n,
                               fc::raw::pack(i));

   // Every action carries the exploded `data` the endpoint cannot use alongside the `hex_data` it reads.
   fc::mutable_variant_object request(fc::variant(static_cast<const transaction&>(trx)).get_object());
   fc::variants                actions;
   for (uint32_t i = 0; i < action_count; ++i) {
      const action& act = trx.actions[i];
      actions.emplace_back(fc::mutable_variant_object()
         ("account", act.account)
         ("name", act.name)
         ("authorization", act.authorization)
         ("data", fc::mutable_variant_object()("value", i))
         ("hex_data", fc::to_hex(act.data.data(), act.data.size())));
   }
   request["actions"] = std::move(actions);
   const std::string body = fc::json::to_string(fc::variant(std::move(request)), fc::time_point::maximum());

   BOOST_CHECK(read_only::to_get_transaction_id_params(fc::json::from_string(body)).id() == trx.id());

   const auto elapsed_us = [](const auto& f) {
      const fc::time_point start = fc::time_point::now();
      f();
      return (fc::time_point::now() - start).count();
   };
   int64_t parse_us   = std::numeric_limits<int64_t>::max();
   int64_t convert_us = std::numeric_limits<int64_t>::max();
   for (int run = 0; run < timing_runs; ++run) {
      fc::variant                          parsed;
      read_only::get_transaction_id_params converted;
      parse_us   = std::min(parse_us, elapsed_us([&] { parsed = fc::json::from_string(body); }));
      convert_us = std::min(convert_us, elapsed_us([&] {
         converted = read_only::to_get_transaction_id_params(std::move(parsed));
      }));
      BOOST_TEST(converted.actions.size() == action_count);
   }
   BOOST_TEST_MESSAGE("parse " << parse_us << "us, convert " << convert_us << "us");
   BOOST_TEST(convert_us < parse_us * max_convert_to_parse_ratio);

   // `hex_data` wins over a `data` string; a `data` string alone is used as is.
   const fc::variant first_action = fc::variant(trx.actions[0]);
   const auto with_actions = [&](fc::variants acts) {
      fc::mutable_variant_object single(fc::variant(static_cast<const transaction&>(trx)).get_object());
      single["actions"] = std::move(acts);
      return fc::variant(std::move(single));
   };
   transaction one_action = trx;
   one_action.actions.resize(1);
   const fc::variant          hex_data = first_action["data"];
   fc::mutable_variant_object stale_data(first_action.get_object());
   stale_data["hex_data"] = hex_data;
   stale_data["data"]     = "00";
   BOOST_CHECK(read_only::to_get_transaction_id_params(with_actions({fc::variant(std::move(stale_data))})).id() ==
              one_action.id());
   BOOST_CHECK(read_only::to_get_transaction_id_params(with_actions({first_action})).id() == one_action.id());

   // Malformed requests.
   fc::mutable_variant_object exploded_only(first_action.get_object());
   exploded_only["data"] = fc::mutable_variant_object()("value", 0);
   fc::mutable_variant_object no_actions(fc::variant(static_cast<const transaction&>(trx)).get_object());
   no_actions.erase("actions");
   BOOST_CHECK_THROW(read_only::to_get_transaction_id_params(fc::variant("not an object")), invalid_http_request);
   BOOST_CHECK_THROW(read_only::to_get_transaction_id_params(fc::variant(std::move(no_actions))),
                     invalid_http_request);
   BOOST_CHECK_THROW(read_only::to_get_transaction_id_params(with_actions({fc::variant(1)})), invalid_http_request);
   BOOST_CHECK_THROW(read_only::to_get_transaction_id_params(with_actions({fc::variant(std::move(exploded_only))})),
                     invalid_http_request);
   BOOST_CHECK_THROW(read_only::to_get_transaction_id_params(fc::variant(transaction())), invalid_http_request);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
