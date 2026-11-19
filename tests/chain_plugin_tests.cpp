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

#include <fc/io/fstream.hpp>

#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <array>
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

namespace {

/// The asserter ABI with its `condition` field renamed to `field`. Each test passes its own name, so the bytes are ones
/// no serializer has been built for yet.
std::string asserter_abi_with_field(std::string_view field) {
   std::string abi = test_contracts::asserter_abi();
   const size_t at = abi.find("condition");
   BOOST_REQUIRE(at != std::string::npos);
   abi.replace(at, std::string_view("condition").size(), field);
   return abi;
}

/// Serializer builds the shared cache has started so far.
uint64_t shared_builds() {
   return chain_apis::shared_abi_serializer_cache().builds();
}

/// An ABI whose `store` action takes an array of structs, so its data can be made as slow to decode as a test needs.
constexpr std::string_view bulk_abi = R"({
   "version": "sysio::abi/1.2", "types": [],
   "structs": [
      {"name": "item", "base": "", "fields": [{"name": "owner", "type": "name"}, {"name": "amount", "type": "uint64"},
                                              {"name": "balance", "type": "asset"}]},
      {"name": "store", "base": "", "fields": [{"name": "items", "type": "item[]"}]}
   ],
   "actions": [{"name": "store", "type": "store", "ricardian_contract": ""}],
   "tables": [], "ricardian_clauses": [], "variants": [], "action_results": []
})";

/// Data of bulk's `store` action holding `count` items.
bytes bulk_store_data(size_t count) {
   std::vector<char>                 packed;
   fc::datastream<std::vector<char>> ds(packed);
   fc::raw::pack(ds, fc::unsigned_int(count));
   for (size_t i = 0; i < count; ++i) {
      fc::raw::pack(ds, name("alice"));
      fc::raw::pack(ds, uint64_t(i));
      fc::raw::pack(ds, asset(int64_t(i), symbol(4, "SYS")));
   }
   return ds.storage();
}

/// The trace of a processed transaction whose top-level actions are `acts`, in order.
transaction_trace_ptr trace_of(const std::vector<action>& acts) {
   auto trace = std::make_shared<transaction_trace>();
   trace->id  = transaction_id_type::hash("trace_of");
   for (size_t i = 0; i < acts.size(); ++i) {
      action_trace act_trace;
      act_trace.action_ordinal = i + 1;
      act_trace.receiver       = acts[i].account;
      act_trace.act            = acts[i];
      action_receipt receipt;
      receipt.receiver        = acts[i].account;
      receipt.global_sequence = i + 1;
      receipt.recv_sequence   = i + 1;
      act_trace.receipt       = receipt;
      trace->action_traces.push_back(std::move(act_trace));
   }
   return trace;
}

/**
 * Starts a request with `start` while asserter's ABI names its first field `field`, then restores the ABI and runs the
 * function the request completed with, as the http thread pool would. Checks that the request built no serializer
 * before that function ran, that the function built the one serializer, and that it decoded the data with the ABI as
 * it was when the transaction completed.
 */
template <typename Result>
void expect_abis_copied_at_completion(validating_tester& chain, std::string_view field,
                                      const std::function<void(next_function<Result>)>& start) {
   chain.set_abi("asserter"_n, asserter_abi_with_field(field));
   chain.produce_block();

   const uint64_t builds = shared_builds();
   std::optional<next_function_variant<Result>> completed;
   start([&completed](next_function_variant<Result>&& result) { completed = std::move(result); });
   BOOST_CHECK_EQUAL(shared_builds(), builds);
   BOOST_REQUIRE(completed);
   using deferred_t = std::function<t_or_exception<Result>()>;
   BOOST_REQUIRE(std::holds_alternative<deferred_t>(*completed));

   chain.set_abi("asserter"_n, test_contracts::asserter_abi());
   chain.produce_block();
   const t_or_exception<Result> decoded = std::get<deferred_t>(*completed)();
   BOOST_REQUIRE(std::holds_alternative<Result>(decoded));
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);
   const fc::variant& data = std::get<Result>(decoded).processed["action_traces"][size_t(0)]["act"]["data"];
   BOOST_REQUIRE(data.is_object());
   BOOST_CHECK(data.get_object().contains(std::string(field)));
   BOOST_CHECK(!data.get_object().contains("condition"));
}

} // namespace

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
   std::string block_str =
      json::to_pretty_string(plugin.convert_block(block, chain_apis::captured_abis::capture(*control, *block)));
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
   std::string block_str2 =
      json::to_pretty_string(plugin.convert_block(block2, chain_apis::captured_abis::capture(*control, *block2)));
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
      {"-1", std::nullopt},
      {"-" + std::to_string(uint32_max), std::nullopt},          // -4294967295: 1 modulo 2^32
      {"-" + std::to_string(uint64_max), std::nullopt},          // -18446744073709551615: 1 modulo 2^64
   };

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
      // Neither a number nor a block id.
      BOOST_TEST_CONTEXT(endpoint) {
         BOOST_CHECK_THROW(lookup_block_num("nope"), block_id_type_exception);
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

/// Packing a transaction request runs under the request deadline: past it, push_transaction, send_transaction,
/// send_transaction2 and compute_transaction fail before the transaction reaches the node, with
/// packed_transaction_type_exception rather than a deadline exception.
BOOST_FIXTURE_TEST_CASE( push_transaction_honors_request_deadline, validating_tester ) try {
   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{config::system_account_name, config::active_name}},
                            config::system_account_name, "noop"_n, bytes{});
   set_transaction_headers(trx);
   const fc::variant params{packed_transaction(trx)};

   using incoming_method = plugin_interface::incoming::methods::transaction_async;
   appbase::scoped_app app;
   size_t              provided = 0;
   // The provider stays registered while its handle lives.
   incoming_method::method_type::handle provider = app->get_method<incoming_method>().register_provider(
      [&provided](const packed_transaction_ptr&, bool, transaction_metadata::trx_type, bool,
                  next_function<transaction_trace_ptr>) { ++provided; });

   std::optional<chain_apis::trx_retry_db>  no_retry;
   std::optional<chain_apis::tracked_votes> tracked_votes;
   chain_apis::read_write rw_expired(*control, no_retry, fc::microseconds::maximum(), fc::microseconds(0), true);
   chain_apis::read_only  ro_expired(*control, {}, {}, tracked_votes, fc::microseconds::maximum(), fc::microseconds(0),
                                     nullptr);

   const auto expect_not_submitted = [](const auto& refused) {
      BOOST_REQUIRE(refused);
      BOOST_REQUIRE(std::holds_alternative<fc::exception_ptr>(*refused));
      const fc::exception_ptr& error = std::get<fc::exception_ptr>(*refused);
      BOOST_CHECK_EQUAL(error->code(), packed_transaction_type_exception::code_value);
      BOOST_CHECK(error->to_detail_string().find("not submitted") != std::string::npos);
   };
   using push_results_t = chain_apis::read_write::push_transaction_results;
   std::optional<next_function_variant<push_results_t>> pushed;
   rw_expired.push_transaction(params.get_object(),
                               [&pushed](next_function_variant<push_results_t>&& result) { pushed = std::move(result); });
   expect_not_submitted(pushed);

   std::optional<next_function_variant<push_results_t>> sent;
   rw_expired.send_transaction(params.get_object(),
                               [&sent](next_function_variant<push_results_t>&& result) { sent = std::move(result); });
   expect_not_submitted(sent);

   std::optional<next_function_variant<push_results_t>> sent2;
   rw_expired.send_transaction2({.return_failure_trace = true, .transaction = params},
                                [&sent2](next_function_variant<push_results_t>&& result) { sent2 = std::move(result); });
   expect_not_submitted(sent2);

   using compute_results_t = chain_apis::read_only::compute_transaction_results;
   std::optional<next_function_variant<compute_results_t>> computed;
   ro_expired.compute_transaction({params}, [&computed](next_function_variant<compute_results_t>&& result) {
      computed = std::move(result);
   });
   expect_not_submitted(computed);
   BOOST_CHECK_EQUAL(provided, 0u);

   // JSON action data whose serializer has no time to build, under abi_serializer_max_time rather than the deadline.
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, asserter_abi_with_field("pack_probe"));
   produce_block();
   signed_transaction json_trx;
   json_trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                                 "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   set_transaction_headers(json_trx);
   json_trx.sign(get_private_key("asserter"_n, "active"), control->get_chain_id());
   fc::variant json_trx_var;
   abi_serializer::to_variant(static_cast<const transaction&>(json_trx), json_trx_var, get_resolver(),
                              abi_serializer_max_time);
   BOOST_REQUIRE(json_trx_var["actions"][size_t(0)]["data"].is_object());
   const fc::variant json_params = fc::mutable_variant_object()("signatures", json_trx.signatures)(
      "compression", "none")("transaction", json_trx_var);

   chain_apis::read_write no_build_time(*control, no_retry, fc::microseconds(0), fc::microseconds::maximum(), true);
   std::optional<next_function_variant<push_results_t>> unpacked;
   no_build_time.push_transaction(json_params.get_object(), [&unpacked](next_function_variant<push_results_t>&& result) {
      unpacked = std::move(result);
   });
   expect_not_submitted(unpacked);
   BOOST_CHECK_EQUAL(provided, 0u);

   chain_apis::read_write in_time(*control, no_retry, fc::microseconds::maximum(), fc::microseconds::maximum(), true);
   in_time.push_transaction(json_params.get_object(), [](next_function_variant<push_results_t>&&) {});
   BOOST_CHECK_EQUAL(provided, 1u);
} FC_LOG_AND_RETHROW()

/// get_required_keys packs its transaction under the request deadline it is given as well as the per-action limit: past
/// the deadline it fails before returning keys.
BOOST_FIXTURE_TEST_CASE( get_required_keys_honors_request_deadline, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                            "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   set_transaction_headers(trx);
   fc::variant trx_var;
   abi_serializer::to_variant(trx, trx_var, get_resolver(), abi_serializer_max_time);
   BOOST_REQUIRE(trx_var["actions"][size_t(0)]["data"].is_object());

   const public_key_type key = get_public_key("asserter"_n, "active");
   const chain_apis::read_only::get_required_keys_params params{trx_var, {key}};
   std::optional<chain_apis::tracked_votes> tracked_votes;

   chain_apis::read_only unlimited(*control, {}, {}, tracked_votes, fc::microseconds::maximum(),
                                   fc::microseconds::maximum(), nullptr);
   BOOST_CHECK(unlimited.get_required_keys(params, unlimited.start()).required_keys ==
               flat_set<public_key_type>{key});

   chain_apis::read_only expired(*control, {}, {}, tracked_votes, fc::microseconds::maximum(), fc::microseconds(0),
                                 nullptr);
   BOOST_CHECK_THROW(expired.get_required_keys(params, expired.start()), abi_serialization_deadline_exception);
} FC_LOG_AND_RETHROW()

/// push_transaction and push_transactions hand decoding their traces to the function they return, which runs on the
/// http thread pool rather than on the thread that executed the transactions. abi_serializer_max_time bounds that
/// decoding as a whole; with none, the data stays hex.
BOOST_FIXTURE_TEST_CASE( push_transaction_defers_trace_decode, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                            "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   set_transaction_headers(trx);
   trx.sign(get_private_key("asserter"_n, "active"), control->get_chain_id());
   const fc::variant params{packed_transaction(trx)};
   const transaction_trace_ptr trace = push_transaction(trx);
   const std::string hex_data = fc::to_hex(trx.actions[0].data);

   using incoming_method = plugin_interface::incoming::methods::transaction_async;
   appbase::scoped_app app;
   size_t              provided = 0;
   // The provider stays registered while its handle lives.
   incoming_method::method_type::handle provider = app->get_method<incoming_method>().register_provider(
      [&trace, &provided](const packed_transaction_ptr&, bool, transaction_metadata::trx_type, bool,
                          next_function<transaction_trace_ptr> next) {
         ++provided;
         next(transaction_trace_ptr(trace));
      });

   std::optional<chain_apis::trx_retry_db> no_retry;
   using results_t  = chain_apis::read_write::push_transaction_results;
   using deferred_t = std::function<t_or_exception<results_t>()>;
   const auto push = [&](const fc::microseconds& max_time) {
      chain_apis::read_write rw(*control, no_retry, max_time, fc::microseconds::maximum(), true);
      std::optional<next_function_variant<results_t>> pushed;
      rw.push_transaction(params.get_object(), [&pushed](next_function_variant<results_t>&& result) {
         pushed = std::move(result);
      });
      BOOST_REQUIRE(pushed);
      BOOST_REQUIRE(std::holds_alternative<deferred_t>(*pushed));
      t_or_exception<results_t> decoded = std::get<deferred_t>(*pushed)();
      BOOST_REQUIRE(std::holds_alternative<results_t>(decoded));
      return std::get<results_t>(std::move(decoded));
   };

   const results_t results = push(fc::microseconds::maximum());
   BOOST_CHECK(results.transaction_id == trx.id());
   const fc::variant& act = results.processed["action_traces"][size_t(0)]["act"];
   BOOST_REQUIRE(act["data"].is_object());
   BOOST_CHECK_EQUAL(act["data"]["message"].as_string(), "fine");

   // With no time to decode, the data stays hex.
   const results_t spent = push(fc::microseconds(0));
   BOOST_CHECK_EQUAL(spent.processed["action_traces"][size_t(0)]["act"]["data"].as_string(), hex_data);
   BOOST_CHECK_EQUAL(spent.processed["action_traces"][size_t(0)]["act"]["hex_data"].as_string(), hex_data);

   using batch_results_t  = chain_apis::read_write::push_transactions_results;
   using batch_deferred_t = std::function<t_or_exception<batch_results_t>()>;
   const auto push_batch = [&](const fc::microseconds& max_time) {
      chain_apis::read_write rw(*control, no_retry, max_time, fc::microseconds::maximum(), true);
      std::optional<next_function_variant<batch_results_t>> pushed;
      rw.push_transactions({params.get_object(), params.get_object()},
                           [&pushed](next_function_variant<batch_results_t>&& result) { pushed = std::move(result); });
      BOOST_REQUIRE(pushed);
      BOOST_REQUIRE(std::holds_alternative<batch_deferred_t>(*pushed));
      t_or_exception<batch_results_t> decoded = std::get<batch_deferred_t>(*pushed)();
      BOOST_REQUIRE(std::holds_alternative<batch_results_t>(decoded));
      return std::get<batch_results_t>(std::move(decoded));
   };

   const batch_results_t batch = push_batch(fc::microseconds::maximum());
   BOOST_REQUIRE_EQUAL(batch.size(), 2u);
   for (const results_t& result : batch)
      BOOST_CHECK(result.processed["action_traces"][size_t(0)]["act"]["data"].is_object());

   const batch_results_t batch_spent = push_batch(fc::microseconds(0));
   BOOST_REQUIRE_EQUAL(batch_spent.size(), 2u);
   for (const results_t& result : batch_spent)
      BOOST_CHECK_EQUAL(result.processed["action_traces"][size_t(0)]["act"]["data"].as_string(), hex_data);

   BOOST_CHECK_EQUAL(provided, 6u);
} FC_LOG_AND_RETHROW()

/// get_block builds serializers and decodes a block's action data within abi_serializer_max_time as a whole; with
/// none, the data stays hex and no serializer is built.
BOOST_FIXTURE_TEST_CASE( get_block_honors_response_budget, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, asserter_abi_with_field("budget_probe"));
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                            "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   set_transaction_headers(trx);
   trx.sign(get_private_key("asserter"_n, "active"), control->get_chain_id());
   push_transaction(trx);
   produce_block();

   const chain_apis::read_only::get_raw_block_params param{std::to_string(control->head().block_num())};
   std::optional<chain_apis::tracked_votes> tracked_votes;
   const auto block_json = [&](const fc::microseconds& max_time) {
      chain_apis::read_only plugin(*control, {}, {}, tracked_votes, max_time, fc::microseconds::maximum(), nullptr);
      const t_or_exception<fc::variant> converted = plugin.get_block(param, fc::time_point::maximum())();
      BOOST_REQUIRE(std::holds_alternative<fc::variant>(converted));
      return json::to_string(std::get<fc::variant>(converted), fc::time_point::maximum());
   };

   const uint64_t    builds   = shared_builds();
   const std::string hex_data = fc::to_hex(trx.actions[0].data);
   const std::string spent    = block_json(fc::microseconds(0));
   BOOST_CHECK(spent.find("\"message\":\"fine\"") == std::string::npos);
   BOOST_CHECK(spent.find("\"data\":\"" + hex_data + "\"") != std::string::npos);
   BOOST_CHECK(spent.find("\"hex_data\":\"" + hex_data + "\"") != std::string::npos);
   BOOST_CHECK_EQUAL(shared_builds(), builds);

   const std::string decoded = block_json(fc::microseconds::maximum());
   BOOST_CHECK(decoded.find("\"budget_probe\":1,\"message\":\"fine\"") != std::string::npos);
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);
} FC_LOG_AND_RETHROW()

/// get_block only copies the ABIs of the accounts its block names. The function it returns, which runs on the http
/// thread pool, builds their serializers and decodes the block with the ABIs as they were when get_block ran.
BOOST_FIXTURE_TEST_CASE( get_block_copies_abis_for_the_http_thread_pool, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, asserter_abi_with_field("block_probe"));
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                            "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   set_transaction_headers(trx);
   trx.sign(get_private_key("asserter"_n, "active"), control->get_chain_id());
   push_transaction(trx);
   produce_block();

   const chain_apis::read_only::get_raw_block_params param{std::to_string(control->head().block_num())};
   std::optional<chain_apis::tracked_votes> tracked_votes;
   chain_apis::read_only plugin(*control, {}, {}, tracked_votes, fc::microseconds::maximum(),
                                fc::microseconds::maximum(), nullptr);

   const uint64_t builds  = shared_builds();
   const auto     convert = plugin.get_block(param, fc::time_point::maximum());
   BOOST_CHECK_EQUAL(shared_builds(), builds);

   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();
   const t_or_exception<fc::variant> converted = convert();
   BOOST_REQUIRE(std::holds_alternative<fc::variant>(converted));
   const std::string block_json = json::to_string(std::get<fc::variant>(converted), fc::time_point::maximum());
   BOOST_CHECK(block_json.find("\"block_probe\":1,\"message\":\"fine\"") != std::string::npos);
   BOOST_CHECK(block_json.find("\"condition\"") == std::string::npos);
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);
} FC_LOG_AND_RETHROW()

/// Each transaction API only copies the ABIs its trace names when the transaction completes. The function it returns,
/// which runs on the http thread pool, builds their serializers and decodes the trace with the ABIs as they were then.
BOOST_FIXTURE_TEST_CASE( transaction_completions_copy_abis_for_the_http_thread_pool, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n,
                            "procassert"_n, fc::raw::pack(int8_t{1}, std::string("fine")));
   set_transaction_headers(trx);
   trx.sign(get_private_key("asserter"_n, "active"), control->get_chain_id());
   const fc::variant             params{packed_transaction(trx)};
   const transaction_trace_ptr   trace = push_transaction(trx);

   using incoming_method = plugin_interface::incoming::methods::transaction_async;
   appbase::scoped_app app;
   // The provider stays registered while its handle lives.
   incoming_method::method_type::handle provider = app->get_method<incoming_method>().register_provider(
      [&trace](const packed_transaction_ptr&, bool, transaction_metadata::trx_type, bool,
               next_function<transaction_trace_ptr> next) { next(transaction_trace_ptr(trace)); });

   std::optional<chain_apis::trx_retry_db>  no_retry;
   std::optional<chain_apis::tracked_votes> tracked_votes;
   chain_apis::read_write rw(*control, no_retry, fc::microseconds::maximum(), fc::microseconds::maximum(), true);
   chain_apis::read_only  ro(*control, {}, {}, tracked_votes, fc::microseconds::maximum(),
                             fc::microseconds::maximum(), nullptr);

   using push_results_t = chain_apis::read_write::push_transaction_results;
   expect_abis_copied_at_completion<push_results_t>(*this, "push_probe", [&](next_function<push_results_t> next) {
      rw.push_transaction(params.get_object(), std::move(next));
   });
   expect_abis_copied_at_completion<push_results_t>(*this, "send_probe", [&](next_function<push_results_t> next) {
      rw.send_transaction(params.get_object(), std::move(next));
   });
   expect_abis_copied_at_completion<push_results_t>(*this, "send2_probe", [&](next_function<push_results_t> next) {
      rw.send_transaction2({.return_failure_trace = true, .transaction = params}, std::move(next));
   });
   using compute_results_t = chain_apis::read_only::compute_transaction_results;
   expect_abis_copied_at_completion<compute_results_t>(*this, "compute_probe",
                                                       [&](next_function<compute_results_t> next) {
      ro.compute_transaction({params}, std::move(next));
   });

   // push_transactions copies the ABIs of each trace when its transaction completes, and builds one serializer for an
   // ABI its traces share.
   set_abi("asserter"_n, asserter_abi_with_field("batch_probe"));
   produce_block();
   using batch_results_t  = chain_apis::read_write::push_transactions_results;
   using batch_deferred_t = std::function<t_or_exception<batch_results_t>()>;
   const uint64_t builds = shared_builds();
   std::optional<next_function_variant<batch_results_t>> pushed;
   rw.push_transactions({params.get_object(), params.get_object()},
                        [&pushed](next_function_variant<batch_results_t>&& result) { pushed = std::move(result); });
   BOOST_CHECK_EQUAL(shared_builds(), builds);
   BOOST_REQUIRE(pushed);
   BOOST_REQUIRE(std::holds_alternative<batch_deferred_t>(*pushed));
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();
   const t_or_exception<batch_results_t> decoded = std::get<batch_deferred_t>(*pushed)();
   BOOST_REQUIRE(std::holds_alternative<batch_results_t>(decoded));
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);
   const batch_results_t& batch = std::get<batch_results_t>(decoded);
   BOOST_REQUIRE_EQUAL(batch.size(), 2u);
   for (const push_results_t& result : batch)
      BOOST_CHECK(result.processed["action_traces"][size_t(0)]["act"]["data"].get_object().contains("batch_probe"));
} FC_LOG_AND_RETHROW()

/// A resolver made for one request returns the serializer an earlier request built for the same ABI.
BOOST_FIXTURE_TEST_CASE( make_resolver_reuses_serializer_across_requests, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_block();

   const auto first  = make_resolver(*control, abi_serializer_max_time, throw_on_yield::yes)("asserter"_n);
   const auto second = make_resolver(*control, abi_serializer_max_time, throw_on_yield::yes)("asserter"_n);
   BOOST_REQUIRE(first);
   BOOST_REQUIRE(second);
   BOOST_CHECK(&*first == &*second);
   BOOST_CHECK(!make_resolver(*control, abi_serializer_max_time, throw_on_yield::yes)("nosuchacct"_n));
} FC_LOG_AND_RETHROW()

/// Decoding a processed transaction's trace that runs out of abi_serializer_max_time part way still returns the trace,
/// never an error: actions decoded by then keep their decoded data, the action that ran out of time and every action
/// after it keep their data as hex, and the receipts and ids are all there. Covers push_transaction, send_transaction
/// and push_transactions, whose traces share one budget.
BOOST_FIXTURE_TEST_CASE( trace_decode_out_of_time_returns_trace, validating_tester ) try {
   create_accounts({"asserter"_n, "bulk"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   set_abi("bulk"_n, std::string(bulk_abi));
   produce_block();

   // Far more data than decodes within the limit on any machine; the limit stops its decode, so the size costs no time.
   constexpr size_t           slow_item_count = 128 * 1024;
   constexpr fc::microseconds limit           = fc::milliseconds(20);
   const action fast(std::vector<permission_level>{{"asserter"_n, config::active_name}}, "asserter"_n, "procassert"_n,
                     fc::raw::pack(int8_t{1}, std::string("fine")));
   const action slow(std::vector<permission_level>{{"bulk"_n, config::active_name}}, "bulk"_n, "store"_n,
                     bulk_store_data(slow_item_count));
   const transaction_trace_ptr trace     = trace_of({fast, slow, fast});
   const std::string           fast_hex  = fc::to_hex(fast.data);
   const std::string           slow_hex  = fc::to_hex(slow.data);
   // Both serializers are cached first, so the limit is spent on decoding alone.
   BOOST_REQUIRE(make_resolver(*control, fc::microseconds::maximum(), throw_on_yield::yes)("asserter"_n));
   BOOST_REQUIRE(make_resolver(*control, fc::microseconds::maximum(), throw_on_yield::yes)("bulk"_n));

   using incoming_method = plugin_interface::incoming::methods::transaction_async;
   appbase::scoped_app app;
   // The provider stays registered while its handle lives.
   incoming_method::method_type::handle provider = app->get_method<incoming_method>().register_provider(
      [&trace](const packed_transaction_ptr&, bool, transaction_metadata::trx_type, bool,
               next_function<transaction_trace_ptr> next) { next(transaction_trace_ptr(trace)); });

   signed_transaction trx;
   trx.actions.push_back(fast);
   set_transaction_headers(trx);
   const fc::variant params{packed_transaction(trx)};
   std::optional<chain_apis::trx_retry_db> no_retry;
   chain_apis::read_write rw(*control, no_retry, limit, fc::microseconds::maximum(), true);

   // The first action decoded, the slow one and the one after it hex, each with its receipt.
   const auto expect_cut_short = [&](const fc::variant& action_traces) {
      BOOST_REQUIRE_EQUAL(action_traces.size(), 3u);
      const fc::variant& first = action_traces[size_t(0)]["act"];
      BOOST_REQUIRE(first["data"].is_object());
      BOOST_CHECK_EQUAL(first["data"]["message"].as_string(), "fine");
      BOOST_CHECK_EQUAL(action_traces[size_t(1)]["act"]["data"].as_string(), slow_hex);
      BOOST_CHECK_EQUAL(action_traces[size_t(1)]["act"]["hex_data"].as_string(), slow_hex);
      BOOST_CHECK_EQUAL(action_traces[size_t(2)]["act"]["data"].as_string(), fast_hex);
      for (size_t i = 0; i < 3; ++i)
         BOOST_CHECK_EQUAL(action_traces[i]["receipt"]["global_sequence"].as_uint64(), i + 1);
   };

   using results_t  = chain_apis::read_write::push_transaction_results;
   using deferred_t = std::function<t_or_exception<results_t>()>;
   const auto decoded = [](const std::optional<next_function_variant<results_t>>& completed) {
      BOOST_REQUIRE(completed);
      BOOST_REQUIRE(std::holds_alternative<deferred_t>(*completed));
      t_or_exception<results_t> result = std::get<deferred_t>(*completed)();
      BOOST_REQUIRE(std::holds_alternative<results_t>(result));
      return std::get<results_t>(std::move(result));
   };

   std::optional<next_function_variant<results_t>> pushed;
   rw.push_transaction(params.get_object(),
                       [&pushed](next_function_variant<results_t>&& result) { pushed = std::move(result); });
   const results_t push_result = decoded(pushed);
   BOOST_CHECK(push_result.transaction_id == trace->id);
   expect_cut_short(push_result.processed["action_traces"]);

   std::optional<next_function_variant<results_t>> sent;
   rw.send_transaction(params.get_object(),
                       [&sent](next_function_variant<results_t>&& result) { sent = std::move(result); });
   const results_t send_result = decoded(sent);
   BOOST_CHECK(send_result.transaction_id == trace->id);
   expect_cut_short(send_result.processed["action_traces"]);

   // One budget covers every trace of push_transactions: the second trace starts after it is spent, so all its data
   // stays hex, and it is still returned whole.
   using batch_results_t  = chain_apis::read_write::push_transactions_results;
   using batch_deferred_t = std::function<t_or_exception<batch_results_t>()>;
   std::optional<next_function_variant<batch_results_t>> batch_pushed;
   rw.push_transactions({params.get_object(), params.get_object()},
                        [&batch_pushed](next_function_variant<batch_results_t>&& result) {
                           batch_pushed = std::move(result);
                        });
   BOOST_REQUIRE(batch_pushed);
   BOOST_REQUIRE(std::holds_alternative<batch_deferred_t>(*batch_pushed));
   const t_or_exception<batch_results_t> batch_result = std::get<batch_deferred_t>(*batch_pushed)();
   BOOST_REQUIRE(std::holds_alternative<batch_results_t>(batch_result));
   const batch_results_t& batch = std::get<batch_results_t>(batch_result);
   BOOST_REQUIRE_EQUAL(batch.size(), 2u);
   BOOST_CHECK(batch[0].transaction_id == trace->id);
   expect_cut_short(batch[0].processed["action_traces"]);
   BOOST_CHECK(batch[1].transaction_id == trace->id);
   const fc::variant& second = batch[1].processed["action_traces"];
   BOOST_REQUIRE_EQUAL(second.size(), 3u);
   BOOST_CHECK_EQUAL(second[size_t(0)]["act"]["data"].as_string(), fast_hex);
   BOOST_CHECK_EQUAL(second[size_t(1)]["act"]["data"].as_string(), slow_hex);
   BOOST_CHECK_EQUAL(second[size_t(2)]["act"]["data"].as_string(), fast_hex);
} FC_LOG_AND_RETHROW()

/// A serializer build make_resolver starts gets the time abi_serializer_max_time allows, cut short at the request
/// deadline: with no time left by either, nothing is built.
BOOST_FIXTURE_TEST_CASE( make_resolver_builds_within_request_deadline, validating_tester ) try {
   create_accounts({"asserter"_n});
   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, asserter_abi_with_field("resolver_probe"));
   produce_block();

   const uint64_t builds = shared_builds();
   const fc::time_point later = fc::time_point::now() + fc::seconds(30);
   BOOST_CHECK_THROW(make_resolver(*control, fc::microseconds::maximum(), throw_on_yield::yes,
                                   fc::time_point::now())("asserter"_n),
                     abi_serialization_deadline_exception);
   BOOST_CHECK_THROW(make_resolver(*control, fc::microseconds(0), throw_on_yield::yes, later)("asserter"_n),
                     abi_serialization_deadline_exception);
   BOOST_CHECK(!make_resolver(*control, fc::microseconds(0), throw_on_yield::no, later)("asserter"_n));
   BOOST_CHECK_EQUAL(shared_builds(), builds);

   const auto built = make_resolver(*control, fc::microseconds::maximum(), throw_on_yield::yes, later)("asserter"_n);
   BOOST_REQUIRE(built);
   BOOST_CHECK(built->is_struct("procassert"));
   BOOST_CHECK_EQUAL(shared_builds(), builds + 1);
   // Once built, the serializer is had whatever time is left.
   BOOST_CHECK(make_resolver(*control, fc::microseconds(0), throw_on_yield::yes, fc::time_point::now())("asserter"_n) ==
               built);
} FC_LOG_AND_RETHROW()

/// Two forks that give one account different ABIs at the same ABI sequence each decode with their own ABI.
BOOST_AUTO_TEST_CASE( make_resolver_keys_serializers_by_abi_bytes ) try {
   std::string renamed_abi = test_contracts::asserter_abi();
   const auto  field       = renamed_abi.find("condition");
   BOOST_REQUIRE(field != std::string::npos);
   renamed_abi.replace(field, std::string_view("condition").size(), "verdict");

   tester fork_a;
   tester fork_b;
   const std::string original_abi = test_contracts::asserter_abi();
   for (const auto& [chain, abi] : {std::pair<tester*, const std::string*>{&fork_a, &original_abi},
                                    std::pair<tester*, const std::string*>{&fork_b, &renamed_abi}}) {
      chain->create_accounts({"asserter"_n});
      chain->set_code("asserter"_n, test_contracts::asserter_wasm());
      chain->set_abi("asserter"_n, *abi);
      chain->produce_block();
   }
   BOOST_REQUIRE_EQUAL(fork_a.control->find_account_metadata("asserter"_n)->abi_sequence,
                       fork_b.control->find_account_metadata("asserter"_n)->abi_sequence);

   const bytes data = fc::raw::pack(int8_t{1}, std::string("text"));
   const auto  decode = [&](tester& chain) {
      const fc::microseconds max_time = base_tester::abi_serializer_max_time;
      const auto abis = make_resolver(*chain.control, max_time, throw_on_yield::yes)("asserter"_n);
      BOOST_REQUIRE(abis);
      return abis->binary_to_variant(abis->get_action_type("procassert"_n), data, max_time);
   };
   const fc::variant on_a = decode(fork_a);
   const fc::variant on_b = decode(fork_b);
   BOOST_CHECK(on_a.get_object().contains("condition"));
   BOOST_CHECK(!on_a.get_object().contains("verdict"));
   BOOST_CHECK(on_b.get_object().contains("verdict"));
   BOOST_CHECK(!on_b.get_object().contains("condition"));
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
