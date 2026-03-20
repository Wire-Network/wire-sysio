#include <sysio/state_history/serialization.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <boost/test/unit_test.hpp>
#include <contracts.hpp>
#include <test_contracts.hpp>
#include <sysio/state_history/abi.hpp>
#include <sysio/state_history/create_deltas.hpp>
#include <sysio/state_history/log_catalog.hpp>
#include <sysio/state_history/trace_converter.hpp>
#include <sysio/testing/tester.hpp>
#include <fc/io/json.hpp>
#include <fc/io/cfile.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/block_state.hpp>
#include <sysio/chain/resource_limits.hpp>

#include "test_cfd_transaction.hpp"

#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/copy.hpp>

using namespace sysio::chain;
using namespace sysio::testing;
using namespace std::literals;

static const abi_serializer::yield_function_t null_yield_function{};

namespace sysio::state_history {

template <typename ST>
datastream<ST>& operator>>(datastream<ST>& ds, row_pair& rp) {
   fc::raw::unpack(ds, rp.first);
   fc::unsigned_int sz;
   fc::raw::unpack(ds, sz);
   if(sz) {
      rp.second.resize(sz);
      ds.read(rp.second.data(), sz);
   }
   return ds;
}

template <typename ST, typename T>
datastream<ST>& operator>>(datastream<ST>& ds, sysio::state_history::big_vector_wrapper<T>& obj) {
   fc::unsigned_int sz;
   fc::raw::unpack(ds, sz);
   obj.obj.resize(sz);
   for (auto& x : obj.obj)
      fc::raw::unpack(ds, x);
   return ds;
}

std::vector<table_delta> create_deltas(const chainbase::database& db, bool full_snapshot) {
   namespace bio = boost::iostreams;
   std::vector<char> buf;
   bio::filtering_ostreambuf obuf;
   obuf.push(bio::back_inserter(buf));
   pack_deltas(obuf, db, full_snapshot);

   fc::datastream<const char*> is{buf.data(), buf.size()};
   std::vector<table_delta> result;
   fc::raw::unpack(is, result);
   return result;
}
}

BOOST_AUTO_TEST_SUITE(test_state_history)

class table_deltas_tester : public tester {
public:
   using tester::tester;
   using deltas_vector = vector<sysio::state_history::table_delta>;

   pair<bool, deltas_vector::iterator> find_table_delta(const std::string &name, bool full_snapshot = false) {
      v = sysio::state_history::create_deltas(control->db(), full_snapshot);;

      auto find_by_name = [&name](const auto& x) {
         return x.name == name;
      };

      auto it = std::find_if(v.begin(), v.end(), find_by_name);

      return make_pair(it != v.end(), it);
   }

   variants deserialize_data(deltas_vector::iterator &it, const std::string& type, const std::string& in_variant_type) {
      variants result;
      for(size_t i=0; i < it->rows.obj.size(); i++) {
         fc::variant v = shipabi.binary_to_variant(in_variant_type, it->rows.obj[i].second, null_yield_function);
         BOOST_REQUIRE(v.is_array() && v.size() == 2 && v[0ul].is_string());
         BOOST_REQUIRE_EQUAL(v[0ul].get_string(), type);
         result.push_back(std::move(v[1ul]));
      }
      return result;
   }

private:
   deltas_vector v;
   abi_serializer shipabi = abi_serializer(json::from_string(sysio::state_history::ship_abi_without_tables()).as<abi_def>(), null_yield_function);
};

using testers = boost::mpl::list<savanna_tester>;

BOOST_AUTO_TEST_CASE(test_deltas_not_empty) {
   table_deltas_tester chain;

   auto deltas = sysio::state_history::create_deltas(chain.control->db(), false);

   for(const auto &delta: deltas) {
      BOOST_REQUIRE(!delta.rows.obj.empty());
   }
}

BOOST_AUTO_TEST_CASE(test_deltas_account_creation) {
   table_deltas_tester chain;
   chain.produce_block();

   // verify only onblock recv_sequence in delta
   auto result = chain.find_table_delta("account");
   BOOST_REQUIRE(result.first);
   auto& it_account = result.second;
   BOOST_REQUIRE_EQUAL(it_account->rows.obj.size(), 1u);
   auto accounts = chain.deserialize_data(it_account, "account_v0", "account");
   BOOST_REQUIRE_EQUAL(accounts[0]["name"].get_string(), config::system_account_name.to_string());

   // Create new account
   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);

   // Verify that a new record for the new account in the state delta of the block
   result = chain.find_table_delta("account");
   BOOST_REQUIRE(result.first);
   auto& it_account2 = result.second;
   BOOST_REQUIRE_EQUAL(it_account2->rows.obj.size(), 2u);

   accounts = chain.deserialize_data(it_account, "account_v0", "account");
   BOOST_REQUIRE_EQUAL(accounts[0]["name"].get_string(), "sysio"); // onblock
   BOOST_REQUIRE_EQUAL(accounts[1]["name"].get_string(), "newacc");
}

BOOST_AUTO_TEST_CASE(test_deltas_account_metadata) {
   table_deltas_tester chain;

   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);
   chain.produce_block();

   chain.set_code("newacc"_n, std::vector<uint8_t>{}); // creates the account_metadata

   // Spot onto account metadata
   auto result = chain.find_table_delta("account_metadata");
   BOOST_REQUIRE(result.first);
   auto &it_account_metadata = result.second;
   BOOST_REQUIRE_EQUAL(it_account_metadata->rows.obj.size(), 1u);

   const variants accounts_metadata = chain.deserialize_data(it_account_metadata, "account_metadata_v0", "account_metadata");
   BOOST_REQUIRE_EQUAL(accounts_metadata[0]["name"].get_string(), "newacc");
   BOOST_REQUIRE_EQUAL(accounts_metadata[0]["privileged"].as_bool(), false);
}


BOOST_AUTO_TEST_CASE(test_deltas_account_permission) {
   table_deltas_tester chain;
   chain.produce_block();

   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);

   // Check that the permissions of this new account are in the delta
   vector<string> expected_permission_names{ "owner", "active" };
   auto result = chain.find_table_delta("permission");
   BOOST_REQUIRE(result.first);
   auto &it_permission = result.second;
   BOOST_REQUIRE_EQUAL(it_permission->rows.obj.size(), 2u);
   const variants accounts_permissions = chain.deserialize_data(it_permission, "permission_v0", "permission");
   for(size_t i = 0; i < accounts_permissions.size(); i++)
   {
      BOOST_REQUIRE_EQUAL(it_permission->rows.obj[i].first, true);
      BOOST_REQUIRE_EQUAL(accounts_permissions[i]["owner"].get_string(), "newacc");
      BOOST_REQUIRE_EQUAL(accounts_permissions[i]["name"].get_string(), expected_permission_names[i]);
   }
}


BOOST_AUTO_TEST_CASE(test_deltas_account_permission_creation_and_deletion) {
   table_deltas_tester chain;
   chain.produce_block();

   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);

   auto& authorization_manager = chain.control->get_authorization_manager();
   const permission_object* ptr = authorization_manager.find_permission( {"newacc"_n, "active"_n} );
   BOOST_REQUIRE(ptr != nullptr);

   // Create new permission
   chain.set_authority("newacc"_n, "mypermission"_n, ptr->auth.to_authority(),  "active"_n);

   const permission_object* ptr_sub = authorization_manager.find_permission( {"newacc"_n, "mypermission"_n} );
   BOOST_REQUIRE(ptr_sub != nullptr);

   // Verify that the new permission is present in the state delta
   std::vector<std::string> expected_permission_names{ "owner", "active", "mypermission" };
   auto result = chain.find_table_delta("permission");
   BOOST_REQUIRE(result.first);
   auto &it_permission = result.second;
   BOOST_REQUIRE_EQUAL(it_permission->rows.obj.size(), 3u);
   BOOST_REQUIRE_EQUAL(it_permission->rows.obj[2].first, true);
   variants accounts_permissions = chain.deserialize_data(it_permission, "permission_v0", "permission");
   BOOST_REQUIRE_EQUAL(accounts_permissions[2]["owner"].get_string(), "newacc");
   BOOST_REQUIRE_EQUAL(accounts_permissions[2]["name"].get_string(), "mypermission");
   BOOST_REQUIRE_EQUAL(accounts_permissions[2]["parent"].get_string(), "active");

   chain.produce_block();

   // Delete the permission
   chain.delete_authority("newacc"_n, "mypermission"_n);

   result = chain.find_table_delta("permission");
   BOOST_REQUIRE(result.first);
   auto &it_permission_del = result.second;
   BOOST_REQUIRE_EQUAL(it_permission_del->rows.obj.size(), 1u);
   BOOST_REQUIRE_EQUAL(it_permission_del->rows.obj[0].first, false);
   accounts_permissions = chain.deserialize_data(it_permission_del, "permission_v0", "permission");
   BOOST_REQUIRE_EQUAL(accounts_permissions[0]["owner"].get_string(), "newacc");
   BOOST_REQUIRE_EQUAL(accounts_permissions[0]["name"].get_string(), "mypermission");
   BOOST_REQUIRE_EQUAL(accounts_permissions[0]["parent"].get_string(), "active");
}


BOOST_AUTO_TEST_CASE(test_deltas_account_permission_modification) {
   table_deltas_tester chain;
   chain.produce_block();

   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);
   chain.produce_block();
   public_key_type keys[] = {
         public_key_type::from_string("PUB_WA_WdCPfafVNxVMiW5ybdNs83oWjenQXvSt1F49fg9mv7qrCiRwHj5b38U3ponCFWxQTkDsMC"s), // Test for correct serialization of WA key, see issue #9087
         public_key_type::from_string("PUB_K1_12wkBET2rRgE8pahuaczxKbmv7ciehqsne57F9gtzf1PVb7Rf7o"s),
         public_key_type::from_string("PUB_R1_6FPFZqw5ahYrR9jD96yDbbDNTdKtNqRbze6oTDLntrsANgQKZu"s)};

   for(auto &key: keys) {
      // Modify the permission authority
      auto wa_authority = authority(1, {key_weight{key, 1}}, {});
      chain.set_authority("newacc"_n, "active"_n, wa_authority, "owner"_n);

      auto result = chain.find_table_delta("permission");
      BOOST_REQUIRE(result.first);

      auto &it_permission = result.second;
      BOOST_REQUIRE_EQUAL(it_permission->rows.obj.size(), 1u);
      const variants accounts_permissions = chain.deserialize_data(it_permission, "permission_v0", "permission");
      BOOST_REQUIRE_EQUAL(accounts_permissions[0]["owner"].get_string(), "newacc");
      BOOST_REQUIRE_EQUAL(accounts_permissions[0]["name"].get_string(), "active");
      BOOST_REQUIRE_EQUAL(accounts_permissions[0]["auth"]["keys"].size(), 1u);
      BOOST_REQUIRE_EQUAL(accounts_permissions[0]["auth"]["keys"][0ul]["key"].get_string(), key.to_string({}, true));

      chain.produce_block();
   }
}


BOOST_AUTO_TEST_CASE(test_deltas_permission_link) {
   table_deltas_tester chain;
   chain.produce_block();

   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);

   // Spot onto permission_link
   const auto spending_priv_key = chain.get_private_key("newacc"_n, "spending");
   const auto spending_pub_key = spending_priv_key.get_public_key();

   chain.set_authority("newacc"_n, "spending"_n, authority{spending_pub_key}, "active"_n);
   chain.link_authority("newacc"_n, "sysio"_n, "spending"_n, "reqauth"_n);
   chain.push_reqauth("newacc"_n, { permission_level{"newacc"_n, "spending"_n} }, { spending_priv_key });


   auto result = chain.find_table_delta("permission_link");
   BOOST_REQUIRE(result.first);
   auto &it_permission_link = result.second;
   BOOST_REQUIRE_EQUAL(it_permission_link->rows.obj.size(), 1u);
   const variants permission_links = chain.deserialize_data(it_permission_link, "permission_link_v0", "permission_link");
   BOOST_REQUIRE_EQUAL(permission_links[0]["account"].get_string(), "newacc");
   BOOST_REQUIRE_EQUAL(permission_links[0]["message_type"].get_string(), "reqauth");
   BOOST_REQUIRE_EQUAL(permission_links[0]["required_permission"].get_string(), "spending");
}


BOOST_AUTO_TEST_CASE(test_deltas_global_property_history) {
   // Assuming max transaction delay is 45 days (default in config.hpp)
   table_deltas_tester chain;
   chain.produce_block();

   // Change max_transaction_delay to 60 sec
   auto params = chain.control->get_global_properties().configuration;
   params.max_transaction_delay = 60;
   chain.push_action( config::system_account_name, "setparams"_n, config::system_account_name,
                             mutable_variant_object()
                             ("params", params) );

   // Deserialize and spot onto some data
   auto result = chain.find_table_delta("global_property");
   BOOST_REQUIRE(result.first);
   auto &it_global_property = result.second;
   BOOST_REQUIRE_EQUAL(it_global_property->rows.obj.size(), 1u);
   const variants global_properties = chain.deserialize_data(it_global_property, "global_property_v1", "global_property");
   BOOST_REQUIRE_EQUAL(global_properties[0]["configuration"][0ul].get_string(), "chain_config_v1");
   BOOST_REQUIRE_EQUAL(global_properties[0]["configuration"][1ul]["max_transaction_delay"].as_uint64(), 60u);
}


BOOST_AUTO_TEST_CASE(test_deltas_protocol_feature_history) {
   table_deltas_tester chain(setup_policy::none);
   const auto &pfm = chain.control->get_protocol_feature_manager();

   chain.produce_block();

   auto d = pfm.get_builtin_digest(builtin_protocol_feature_t::reserved_first_protocol_feature);
   BOOST_REQUIRE(d);

   // Activate protocol feature.
   chain.schedule_protocol_features_wo_preactivation({*d});

   chain.produce_block();

   chain.set_bios_contract();

   // Spot onto some data of the protocol state table delta
   auto result = chain.find_table_delta("protocol_state");
   BOOST_REQUIRE(result.first);
   auto &it_protocol_state = result.second;
   BOOST_REQUIRE_EQUAL(it_protocol_state->rows.obj.size(), 1u);
   const variants protocol_states = chain.deserialize_data(it_protocol_state, "protocol_state_v0", "protocol_state");
   BOOST_REQUIRE_EQUAL(protocol_states[0]["activated_protocol_features"][0ul][0ul].get_string(), "activated_protocol_feature_v0");
   const digest_type digest_in_delta = protocol_states[0]["activated_protocol_features"][0ul][1ul]["feature_digest"].as<digest_type>();
   BOOST_REQUIRE_EQUAL(digest_in_delta, *d);
}


BOOST_AUTO_TEST_CASE(test_deltas_contract) {
   table_deltas_tester chain;
   chain.produce_block();

   chain.create_account("tester"_n, config::system_account_name, false, false, false, false);

   chain.set_code("tester"_n, test_contracts::get_table_test_wasm());
   chain.set_abi("tester"_n, test_contracts::get_table_test_abi());

   chain.produce_block();

   auto trace = chain.push_action("tester"_n, "addhashobj"_n, "tester"_n, mutable_variant_object()("hashinput", "hello" ));

   trace = chain.push_action("tester"_n, "addnumobj"_n, "tester"_n, mutable_variant_object()("input", 2));

   // Spot onto contract_table
   auto result = chain.find_table_delta("contract_table");
   BOOST_REQUIRE(result.first);
   auto &it_contract_table = result.second;
   BOOST_REQUIRE_EQUAL(it_contract_table->rows.obj.size(), 6u);
   const variants contract_tables = chain.deserialize_data(it_contract_table, "contract_table_v0", "contract_table");
   BOOST_REQUIRE_EQUAL(contract_tables[0]["table"].get_string(), "hashobjs");
   BOOST_REQUIRE_EQUAL(contract_tables[1]["table"].get_string(), "hashobjs....1");
   BOOST_REQUIRE_EQUAL(contract_tables[2]["table"].get_string(), "numobjs");
   BOOST_REQUIRE_EQUAL(contract_tables[3]["table"].get_string(), "numobjs.....1");
   BOOST_REQUIRE_EQUAL(contract_tables[4]["table"].get_string(), "numobjs.....2");
   BOOST_REQUIRE_EQUAL(contract_tables[5]["table"].get_string(), "numobjs.....3");

   // Spot onto contract_row
   result = chain.find_table_delta("contract_row");
   BOOST_REQUIRE(result.first);
   auto &it_contract_row = result.second;
   BOOST_REQUIRE_EQUAL(it_contract_row->rows.obj.size(), 2u);
   const variants contract_rows = chain.deserialize_data(it_contract_row, "contract_row_v0", "contract_row");
   BOOST_REQUIRE_EQUAL(contract_rows[0]["table"].get_string(), "hashobjs");
   BOOST_REQUIRE_EQUAL(contract_rows[1]["table"].get_string(), "numobjs");

   // Spot onto contract_index256
   result = chain.find_table_delta("contract_index256");
   BOOST_REQUIRE(result.first);
   auto &it_contract_index256 = result.second;
   BOOST_REQUIRE_EQUAL(it_contract_index256->rows.obj.size(), 2u);
   const variants contract_indices = chain.deserialize_data(it_contract_index256, "contract_index256_v0", "contract_index256");
   BOOST_REQUIRE_EQUAL(contract_indices[0]["table"].get_string(), "hashobjs");
   BOOST_REQUIRE_EQUAL(contract_indices[1]["table"].get_string(), "hashobjs....1");
}



   BOOST_AUTO_TEST_CASE(test_deltas) {
      table_deltas_tester main;
      main.produce_block();

      auto v = sysio::state_history::create_deltas(main.control->db(), false);

      std::string name="permission";
      auto find_by_name = [&name](const auto& x) {
         return x.name == name;
      };

      auto it = std::find_if(v.begin(), v.end(), find_by_name);
      BOOST_REQUIRE(it==v.end());

      name="resource_limits";
      it = std::find_if(v.begin(), v.end(), find_by_name);
      BOOST_REQUIRE(it!=v.end()); // updated by onblock in start_block
      BOOST_REQUIRE_EQUAL(it->rows.obj.size(), 1u);
      auto resources = main.deserialize_data(it, "resource_limits_v0", "resource_limits");
      BOOST_REQUIRE_EQUAL(resources[0]["owner"].get_string(), config::system_account_name.to_string());

      main.create_account("newacc"_n, config::system_account_name, false, false, false, false);

      v = sysio::state_history::create_deltas(main.control->db(), false);

      name="permission";
      it = std::find_if(v.begin(), v.end(), find_by_name);
      BOOST_REQUIRE(it!=v.end());

      name="resource_limits";
      it = std::find_if(v.begin(), v.end(), find_by_name);
      BOOST_REQUIRE(it!=v.end());
      BOOST_REQUIRE_EQUAL(it->rows.obj.size(), 2u);
      resources = main.deserialize_data(it, "resource_limits_v0", "resource_limits");
      BOOST_REQUIRE_EQUAL(resources[0]["owner"].get_string(), config::system_account_name.to_string());
      BOOST_REQUIRE_EQUAL(resources[1]["owner"].get_string(), "newacc");

      main.produce_block();

      v = sysio::state_history::create_deltas(main.control->db(), false);

      name="permission";
      it = std::find_if(v.begin(), v.end(), find_by_name);
      BOOST_REQUIRE(it==v.end());

      name="resource_limits";
      it = std::find_if(v.begin(), v.end(), find_by_name);
      BOOST_REQUIRE(it!=v.end()); // updated by onblock in start_block
   }

   BOOST_AUTO_TEST_CASE(test_deltas_contract_several_rows){
      // test expects only bios contract to be loaded
      table_deltas_tester chain(setup_policy::preactivate_feature_only);
      chain.set_bios_contract();

      chain.produce_block();
      chain.create_account("tester"_n, config::system_account_name, false, false, false, false);

      chain.set_code("tester"_n, test_contracts::get_table_test_wasm());
      chain.set_abi("tester"_n, test_contracts::get_table_test_abi());

      chain.produce_block();

      auto trace = chain.push_action("tester"_n, "addhashobj"_n, "tester"_n, mutable_variant_object()("hashinput", "hello"));

      trace = chain.push_action("tester"_n, "addhashobj"_n, "tester"_n, mutable_variant_object()("hashinput", "world"));

      trace = chain.push_action("tester"_n, "addhashobj"_n, "tester"_n, mutable_variant_object()("hashinput", "!"));

      trace = chain.push_action("tester"_n, "addnumobj"_n, "tester"_n, mutable_variant_object()("input", 2));

      trace = chain.push_action("tester"_n, "addnumobj"_n, "tester"_n, mutable_variant_object()("input", 3));

      trace = chain.push_action("tester"_n, "addnumobj"_n, "tester"_n, mutable_variant_object()("input", 4));

      // Spot onto contract_row with full snapshot
      auto result = chain.find_table_delta("contract_row", true);
      BOOST_REQUIRE(result.first);
      auto &it_contract_row = result.second;
      BOOST_REQUIRE_EQUAL(it_contract_row->rows.obj.size(), 8u);
      variants contract_rows = chain.deserialize_data(it_contract_row, "contract_row_v0", "contract_row");

      std::multiset<std::string> expected_contract_row_table_names {"abihash", "abihash", "hashobjs", "hashobjs", "hashobjs", "numobjs", "numobjs", "numobjs"};

      std::multiset<uint64_t> expected_contract_row_table_primary_keys {14389258095169634304U,14605619288908759040U, 0, 1 ,2, 0, 1, 2};
      std::multiset<std::string> result_contract_row_table_names;
      std::multiset<uint64_t> result_contract_row_table_primary_keys;
      for(auto &contract_row : contract_rows) {
         result_contract_row_table_names.insert(contract_row["table"].get_string());
         result_contract_row_table_primary_keys.insert(contract_row["primary_key"].as_uint64());
      }
      BOOST_TEST_REQUIRE(expected_contract_row_table_names == result_contract_row_table_names);
      BOOST_TEST_REQUIRE(expected_contract_row_table_primary_keys == result_contract_row_table_primary_keys);

      chain.produce_block();

      trace = chain.push_action("tester"_n, "erasenumobj"_n, "tester"_n, mutable_variant_object()("id", 1));

      trace = chain.push_action("tester"_n, "erasenumobj"_n, "tester"_n, mutable_variant_object()("id", 0));

      result = chain.find_table_delta("contract_row");
      BOOST_REQUIRE(result.first);
      BOOST_REQUIRE_EQUAL(it_contract_row->rows.obj.size(), 2u);
      contract_rows = chain.deserialize_data(it_contract_row, "contract_row_v0", "contract_row");

      for(size_t i=0; i < contract_rows.size(); i++) {
         BOOST_REQUIRE_EQUAL(it_contract_row->rows.obj[i].first, 0);
         BOOST_REQUIRE_EQUAL(contract_rows[i]["table"].get_string(), "numobjs");
      }

      result = chain.find_table_delta("contract_index_double");
      BOOST_REQUIRE(result.first);
      auto &it_contract_index_double = result.second;
      BOOST_REQUIRE_EQUAL(it_contract_index_double->rows.obj.size(), 2u);
      const variants contract_index_double_elems = chain.deserialize_data(it_contract_index_double, "contract_index_double_v0", "contract_index_double");

      for(size_t i=0; i < contract_index_double_elems.size(); i++) {
         BOOST_REQUIRE_EQUAL(it_contract_index_double->rows.obj[i].first, 0);
         BOOST_REQUIRE_EQUAL(contract_index_double_elems[i]["table"].get_string(), "numobjs.....2");
      }

   }

   std::vector<shared_ptr<sysio::state_history::partial_transaction>> get_partial_txns(sysio::state_history::trace_converter& log) {
      std::vector<shared_ptr<sysio::state_history::partial_transaction>> partial_txns;

      for (auto ct : log.cached_traces) {
         partial_txns.push_back(std::get<1>(ct).partial);
      }

      return partial_txns;
   }

   BOOST_AUTO_TEST_CASE(test_trace_log_with_transaction_extensions) {
      SKIP_TEST // TODO: update test to use an ed25519 signed transaction which will have a transaction extension
      validating_tester c;

      fc::temp_directory state_history_dir;
      sysio::state_history::trace_converter log;

      c.control->applied_transaction().connect(
            [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
               log.add_transaction(std::get<0>(t), std::get<1>(t));
            });

      c.create_accounts({"alice"_n, "test"_n});
      // c.set_code("test"_n, test_contracts::deferred_test_wasm());
      // c.set_abi("test"_n, test_contracts::deferred_test_abi());
      c.produce_block();

      c.push_action("test"_n, "defercall"_n, "alice"_n,
                    fc::mutable_variant_object()("payer", "alice")("sender_id", 1)("contract", "test")("payload", 40));

      auto block  = c.produce_block();
      auto partial_txns = get_partial_txns(log);

      auto contains_transaction_extensions = [](shared_ptr<sysio::state_history::partial_transaction> txn) {
         return txn->transaction_extensions.size() > 0;
      };

      BOOST_CHECK(std::any_of(partial_txns.begin(), partial_txns.end(), contains_transaction_extensions));
   }

struct state_history_tester_logs  {
   state_history_tester_logs(const std::filesystem::path& dir, const sysio::state_history::state_history_log_config& config)
      : traces_log(dir, config, "trace_history") , chain_state_log(dir, config, "chain_state_history") {}

   sysio::state_history::log_catalog traces_log;
   sysio::state_history::log_catalog chain_state_log;
   sysio::state_history::trace_converter trace_converter;
};

template<typename T>
struct state_history_tester : state_history_tester_logs, T {
   state_history_tester(const std::filesystem::path& dir, const sysio::state_history::state_history_log_config& config)
   : state_history_tester_logs(dir, config), T ([this](sysio::chain::controller& control) {
      control.applied_transaction().connect(
       [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          trace_converter.add_transaction(std::get<0>(t), std::get<1>(t));
       });

      control.accepted_block().connect([&](block_signal_params t) {
         const auto& [ block, id ] = t;

         traces_log.pack_and_write_entry(id, block->previous, [this, &block](auto&& buf) {
            trace_converter.pack(buf, false, block);
         });

         chain_state_log.pack_and_write_entry(id, block->previous, [&control](auto&& buf) {
            sysio::state_history::pack_deltas(buf, control.db(), true);
         });
      });
      control.block_start().connect([this](uint32_t block_num) {
         trace_converter.cached_traces.clear();
         trace_converter.onblock_trace.reset();
      });
   }) {}
};

using state_history_testers = boost::mpl::list<state_history_tester<savanna_tester>>;

static std::vector<char> get_decompressed_entry(sysio::state_history::log_catalog& log, block_num_type block_num) {
   std::optional<sysio::state_history::ship_log_entry> entry = log.get_entry(block_num);
   if(!entry) //existing tests expect failure to find a block returns an empty vector here
      return {};

   namespace bio = boost::iostreams;
   bio::filtering_istreambuf istream = entry->get_stream();
   std::vector<char> bytes;
   bio::copy(istream, bio::back_inserter(bytes));
   return bytes;
}

static variants get_traces(sysio::state_history::log_catalog& log, block_num_type            block_num) {
   auto     entry = get_decompressed_entry(log, block_num);

   if (entry.size()) {
      abi_serializer shipabi = abi_serializer(json::from_string(sysio::state_history::ship_abi_without_tables()).as<abi_def>(), null_yield_function);
      return shipabi.binary_to_variant("transaction_trace[]", entry, null_yield_function).get_array();
   }
   return variants();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_splitted_log, T, state_history_testers) {
   fc::temp_directory state_history_dir;

   sysio::state_history::partition_config config{
      .retained_dir = "retained",
      .archive_dir = "archive",
      .stride  = 20,
      .max_retained_files = 5
   };

   T chain(state_history_dir.path(), config);
   chain.produce_block();
   chain.produce_blocks(49, true);

   deploy_test_api(chain);
   auto cfd_trace = push_test_cfd_transaction(chain);

   chain.produce_block();
   chain.produce_blocks(99, true);

   auto log_dir = state_history_dir.path();
   auto archive_dir  = log_dir / "archive";
   auto retained_dir = log_dir / "retained";

   BOOST_CHECK(std::filesystem::exists( archive_dir / "trace_history-2-20.log" ));
   BOOST_CHECK(std::filesystem::exists( archive_dir / "trace_history-2-20.index" ));
   BOOST_CHECK(std::filesystem::exists( archive_dir / "trace_history-21-40.log" ));
   BOOST_CHECK(std::filesystem::exists( archive_dir / "trace_history-21-40.index" ));

   BOOST_CHECK(std::filesystem::exists( archive_dir / "chain_state_history-2-20.log" ));
   BOOST_CHECK(std::filesystem::exists( archive_dir / "chain_state_history-2-20.index" ));
   BOOST_CHECK(std::filesystem::exists( archive_dir / "chain_state_history-21-40.log" ));
   BOOST_CHECK(std::filesystem::exists( archive_dir / "chain_state_history-21-40.index" ));

   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      // Under Savanna, logs are archived earlier because LIB advances faster.
      BOOST_CHECK(std::filesystem::exists( archive_dir / "trace_history-41-60.log" ));
      BOOST_CHECK(std::filesystem::exists( archive_dir / "trace_history-41-60.index" ));
   } else {
      BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-41-60.log" ));
      BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-41-60.index" ));
   }

   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-61-80.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-61-80.index" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-81-100.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-81-100.index" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-101-120.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-101-120.index" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-121-140.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-121-140.index" ));
   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-141-160.log" ));
      BOOST_CHECK(std::filesystem::exists( retained_dir / "trace_history-141-160.index" ));
   }

   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      BOOST_CHECK_EQUAL(chain.traces_log.block_range().first, 61u);
   }
   else {
      BOOST_CHECK_EQUAL(chain.traces_log.block_range().first, 41u);
   }

   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-61-80.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-61-80.index" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-81-100.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-81-100.index" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-101-120.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-101-120.index" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-121-140.log" ));
   BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-121-140.index" ));
   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-141-160.log" ));
      BOOST_CHECK(std::filesystem::exists( retained_dir / "chain_state_history-141-160.index" ));
   }

   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
     BOOST_CHECK_EQUAL(chain.chain_state_log.block_range().first, 61u);
   } else {
     BOOST_CHECK_EQUAL(chain.chain_state_log.block_range().first, 41u);
   }

   BOOST_CHECK(get_traces(chain.traces_log, 10).empty());
   BOOST_CHECK(get_traces(chain.traces_log, 100).size());
   BOOST_CHECK(get_traces(chain.traces_log, 140).size());
   BOOST_CHECK(get_traces(chain.traces_log, 150).size());
   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      BOOST_CHECK(get_traces(chain.traces_log, 160).size());
   } else {
      BOOST_CHECK(get_traces(chain.traces_log, 160).empty());
   }

   BOOST_CHECK(get_decompressed_entry(chain.chain_state_log, 10).empty());
   BOOST_CHECK(get_decompressed_entry(chain.chain_state_log, 100).size());
   BOOST_CHECK(get_decompressed_entry(chain.chain_state_log, 140).size());
   BOOST_CHECK(get_decompressed_entry(chain.chain_state_log, 150).size());
   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      BOOST_CHECK(get_decompressed_entry(chain.chain_state_log, 160).size());
   } else {
      BOOST_CHECK(get_decompressed_entry(chain.chain_state_log, 160).empty());
   }
}

void push_blocks( tester& from, tester& to ) {
   while( to.fork_db_head().block_num()
            < from.fork_db_head().block_num() )
   {
      auto fb = from.fetch_block_by_number( to.fork_db_head().block_num()+1 );
      to.push_block( fb );
   }
}

template<typename T>
bool test_fork(uint32_t stride, uint32_t max_retained_files) {
   fc::temp_directory state_history_dir;

   sysio::state_history::partition_config config{
      .retained_dir = "retained",
      .archive_dir = "archive",
      .stride  = stride,
      .max_retained_files = max_retained_files
   };

   T chain1(state_history_dir.path(), config);
   chain1.produce_blocks(2);

   chain1.create_accounts( {"dan"_n,"sam"_n,"pam"_n} );
   chain1.produce_block();
   chain1.set_producers( {"dan"_n,"sam"_n,"pam"_n} );
   chain1.produce_blocks(30);

   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      // Produce one more block; do not vote it such that it won't become final when
      // the first block from chain2 is pushed to chain1. This is to ensure LIBs
      // on chain1 and chain2 are the same, and further blocks from chain2 can be
      // pushed into chain1's fork_db.
      chain1.control->testing_allow_voting(false);
      chain1.produce_block();
   }

   tester chain2(setup_policy::none);
   push_blocks(chain1, chain2);

   auto fork_block_num = chain1.head().block_num();

   chain1.produce_blocks(12);
   auto create_account_traces = chain2.create_accounts( {"adam"_n} );
   auto create_account_trace_id = create_account_traces[0]->id;

   if constexpr (std::is_same_v<T, state_history_tester<savanna_tester>>) {
      // Disable voting on chain2 such that chain2's blocks can form a fork when
      // pushed to chain1
      chain2.control->testing_allow_voting(false);
   }

   auto b = chain2.produce_block();
   chain2.produce_blocks(11+12);

   // Merge blocks from chain2 to chain1 and make the chain from chain2 as the best chain.
   // Specifically in Savanna, as voting is disabled on both chains, block timestamps
   // are used to decide best chain. chain2 is selected because its last block's
   // timestamp is bigger than chain1's last block's.
   for( uint32_t start = fork_block_num + 1, end = chain2.head().block_num(); start <= end; ++start ) {
      auto fb = chain2.fetch_block_by_number( start );
      chain1.push_block( fb );
   }

   auto traces = get_traces(chain1.traces_log, b->block_num());
   bool trace_found = std::find_if(traces.begin(), traces.end(), [create_account_trace_id](const auto& v) {
                         return v[1ul]["id"].template as<digest_type>() == create_account_trace_id;
                      }) != traces.end();

   return trace_found;
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_fork_no_stride, T, state_history_testers) {
   // In this case, the chain fork would NOT trunk the trace log across the stride boundary.
   BOOST_CHECK(test_fork<T>(UINT32_MAX, 10));
}
BOOST_AUTO_TEST_CASE_TEMPLATE(test_fork_with_stride1, T, state_history_testers) {
   // In this case, the chain fork would trunk the trace log across the stride boundary.
   // However, there are still some traces remains after the truncation.
   BOOST_CHECK(test_fork<T>(10, 10));
}
BOOST_AUTO_TEST_CASE_TEMPLATE(test_fork_with_stride2, T, state_history_testers) {
   // In this case, the chain fork would trunk the trace log across the stride boundary.
   // However, no existing trace remain after the truncation. Because we only keep a very
   // short history, the create_account_trace is not available to be found. We just need
   // to make sure no exception is throw.
   BOOST_CHECK_NO_THROW(test_fork<T>(5, 1));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_corrupted_log_recovery, T, state_history_testers) {
   fc::temp_directory state_history_dir;

   sysio::state_history::partition_config config{
      .archive_dir = "archive",
      .stride  = 100,
      .max_retained_files = 5
   };

   T chain(state_history_dir.path(), config);
   chain.produce_block();
   chain.produce_blocks(49, true);
   chain.close();

   // write a few random bytes to block log indicating the last block entry is incomplete
   fc::cfile logfile;
   logfile.set_file_path(state_history_dir.path() / "trace_history.log");
   logfile.open("ab");
   const char random_data[] = "12345678901231876983271649837";
   logfile.write(random_data, sizeof(random_data));
   logfile.close();

   std::filesystem::remove_all(chain.get_config().blocks_dir/"reversible");

   T new_chain(state_history_dir.path(), config);
   new_chain.produce_block();
   new_chain.produce_blocks(49, true);

   BOOST_CHECK(get_traces(new_chain.traces_log, 10).size());
   BOOST_CHECK(get_decompressed_entry(new_chain.chain_state_log,10).size());
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_no_index_recovery, T, state_history_testers) {
   fc::temp_directory state_history_dir;

   sysio::state_history::partition_config config{};

   T chain(state_history_dir.path(), config);
   chain.produce_block();
   chain.produce_blocks(21, true);
   chain.close();

   std::filesystem::remove(state_history_dir.path() / "trace_history.index");

   T new_chain(state_history_dir.path(), config);
   new_chain.produce_block();

   BOOST_CHECK(get_traces(new_chain.traces_log, 10).size());
   BOOST_CHECK(get_decompressed_entry(new_chain.chain_state_log,10).size());
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_curropted_index_recovery, T, state_history_testers) {
   fc::temp_directory state_history_dir;

   sysio::state_history::partition_config config{};

   T chain(state_history_dir.path(), config);
   chain.produce_block();
   chain.produce_blocks(21, true);
   chain.close();

   // write a few random bytes to end of index log, size will not match and it will be auto-recreated
   fc::cfile indexfile;
   indexfile.set_file_path(state_history_dir.path() / "trace_history.index");
   indexfile.open("ab");
   const char random_data[] = "12345678901231876983271649837";
   indexfile.seek_end(0);
   indexfile.write(random_data, sizeof(random_data));
   indexfile.close();

   T new_chain(state_history_dir.path(), config);
   new_chain.produce_block();

   BOOST_CHECK(get_traces(new_chain.traces_log, 10).size());
   BOOST_CHECK(get_decompressed_entry(new_chain.chain_state_log,10).size());
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_curropted_index_error, T, state_history_testers) {
   fc::temp_directory state_history_dir;

   sysio::state_history::partition_config config{};

   T chain(state_history_dir.path(), config);
   chain.produce_block();
   chain.produce_blocks(21, true);
   chain.close();

   // write a few random bytes to end of index log, size will not match and it will be auto-recreated
   fc::cfile indexfile;
   indexfile.set_file_path(state_history_dir.path() / "trace_history.index");
   indexfile.open("rb+");
   const char random_data[] = "12345678901231876983271649837";
   indexfile.seek_end(-(sizeof(random_data)));
   indexfile.write(random_data, sizeof(random_data));
   indexfile.close();

   BOOST_CHECK_EXCEPTION(T(state_history_dir.path(), config),
                         plugin_exception, [](const plugin_exception& e) {
                            return e.to_detail_string().find("trace_history.index is corrupted and cannot be repaired, will be automatically regenerated if removed") != std::string::npos;
                         });

   // remove index for auto recovery
   std::filesystem::remove(state_history_dir.path() / "trace_history.index");

   T new_chain(state_history_dir.path(), config);
   new_chain.produce_block();

   BOOST_CHECK(get_traces(new_chain.traces_log, 10).size());
   BOOST_CHECK(get_decompressed_entry(new_chain.chain_state_log,10).size());
}

// Verifies the state_history ABI for signed_block and all its nested types
// (transaction_receipt_header, transaction_receipt, packed_transaction, qc_t, etc.)
// match the actual binary layout from fc::raw::pack(signed_block).
// Deserializes packed blocks using the ship ABI and cross-checks deserialized
// values against the original C++ objects. If a C++ struct changes but the ABI
// is not updated, the deserialized values will be wrong and this test will fail.
BOOST_AUTO_TEST_CASE(test_signed_block_abi_roundtrip) {
   savanna_tester chain;

   abi_serializer shipabi(
      fc::json::from_string(sysio::state_history::ship_abi_without_tables()).as<abi_def>(),
      null_yield_function
   );

   auto verify_block = [&](const signed_block_ptr& b, const std::string& desc) {
      BOOST_TEST_MESSAGE("Verifying signed_block ABI: " << desc);

      auto packed = fc::raw::pack(*b);

      // Deserialize packed block bytes using the ship ABI struct definitions.
      // If the ABI field order/types don't match the C++ FC_REFLECT layout,
      // either this will throw or the deserialized values will be wrong.
      fc::variant deserialized = shipabi.binary_to_variant("signed_block", packed, null_yield_function);
      auto obj = deserialized.get_object();

      // -- block_header fields --
      BOOST_CHECK_EQUAL(obj["producer"].as_string(), b->producer.to_string());
      BOOST_CHECK_EQUAL(obj["previous"].as_string(), b->previous.str());
      BOOST_CHECK_EQUAL(obj["transaction_mroot"].as_string(), b->transaction_mroot.str());
      BOOST_CHECK_EQUAL(obj["finality_mroot"].as_string(), b->finality_mroot.str());

      // qc_claim
      auto qc_claim = obj["qc_claim"].get_object();
      BOOST_CHECK_EQUAL(qc_claim["block_num"].as_uint64(), b->qc_claim.block_num);
      BOOST_CHECK_EQUAL(qc_claim["is_strong_qc"].as_bool(), b->qc_claim.is_strong_qc);

      // header_extensions count
      BOOST_CHECK_EQUAL(obj["header_extensions"].get_array().size(), b->header_extensions.size());

      // -- signed_block_header fields --
      BOOST_CHECK_EQUAL(obj["producer_signatures"].get_array().size(), b->producer_signatures.size());

      // -- signed_block fields --
      auto& transactions = obj["transactions"].get_array();
      BOOST_REQUIRE_EQUAL(transactions.size(), b->transactions.size());

      // Verify each transaction_receipt against the C++ object
      for (size_t i = 0; i < transactions.size(); ++i) {
         auto receipt = transactions[i].get_object();

         // transaction_receipt_header: cpu_usage_us must be an array (vector<unsigned_int>)
         BOOST_REQUIRE(receipt.contains("cpu_usage_us"));
         const auto& cpu_arr = receipt["cpu_usage_us"].get_array();
         BOOST_REQUIRE_EQUAL(cpu_arr.size(), b->transactions[i].cpu_usage_us.size());
         for (size_t j = 0; j < cpu_arr.size(); ++j) {
            BOOST_CHECK_EQUAL(cpu_arr[j].as_uint64(), b->transactions[i].cpu_usage_us[j].value);
         }

         // transaction_receipt: trx must be a packed_transaction struct (not a variant)
         BOOST_REQUIRE(receipt.contains("trx"));
         auto trx = receipt["trx"].get_object();
         BOOST_CHECK(trx.contains("signatures"));
         BOOST_CHECK(trx.contains("compression"));
         BOOST_CHECK(trx.contains("packed_context_free_data"));
         BOOST_CHECK(trx.contains("packed_trx"));
         BOOST_CHECK_EQUAL(trx["signatures"].get_array().size(),
                           b->transactions[i].trx.get_signatures().size());
      }

      // block_extensions count
      BOOST_CHECK_EQUAL(obj["block_extensions"].get_array().size(), b->block_extensions.size());
   };

   // Test 1: Block without user transactions (header fields, no receipts)
   auto empty_block = chain.produce_block();
   verify_block(empty_block, "block without user transactions");

   // Test 2: Block with transaction receipts (exercises transaction_receipt ABI)
   chain.create_accounts({"alice"_n});
   auto trx_block = chain.produce_block();
   BOOST_REQUIRE(!trx_block->transactions.empty());
   verify_block(trx_block, "block with transaction receipts");

   // Test 3: Later block with QC data
   for (int i = 0; i < 4; ++i)
      chain.produce_block();
   auto qc_block = chain.produce_block();
   verify_block(qc_block, "later block with QC data");
}

// Verifies that the SHiP ABI definitions for transaction_trace_v0, action_trace_v1,
// action_receipt_v0, and partial_transaction_v0 match the custom serialization
// operators in serialization.hpp (history_context_wrapper).
// Produces blocks with transactions, reads traces from the log (deserialized via ABI),
// and compares each deserialized field against the original C++ trace objects.
BOOST_AUTO_TEST_CASE(test_trace_abi_roundtrip) {
   fc::temp_directory state_history_dir;
   sysio::state_history::partition_config config{};

   state_history_tester<savanna_tester> chain(state_history_dir.path(), config);

   // Capture C++ traces for each accepted block via signals.
   // pending_cpp_traces collects traces during block production;
   // accepted_cpp_traces snapshots them when the block is accepted.
   std::vector<std::pair<transaction_trace_ptr, packed_transaction_ptr>> pending_cpp_traces;
   std::vector<std::pair<transaction_trace_ptr, packed_transaction_ptr>> accepted_cpp_traces;

   chain.control->block_start().connect([&](uint32_t) {
      pending_cpp_traces.clear();
   });

   chain.control->applied_transaction().connect(
      [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
         pending_cpp_traces.emplace_back(std::get<0>(t), std::get<1>(t));
      }
   );

   chain.control->accepted_block().connect([&](block_signal_params) {
      accepted_cpp_traces = pending_cpp_traces;
   });

   // Flush the pending block from tester setup so the next block's onblock
   // fires after our signal handlers are connected.
   chain.produce_block();

   // Produce a block with user transactions
   chain.create_accounts({"alice"_n});
   auto b = chain.produce_block();

   BOOST_TEST_MESSAGE("Verifying trace ABI roundtrip for block " << b->block_num());

   // Read traces from the log (deserialized via the SHiP ABI)
   auto abi_traces = get_traces(chain.traces_log, b->block_num());
   BOOST_REQUIRE(!abi_traces.empty());
   BOOST_REQUIRE_EQUAL(abi_traces.size(), accepted_cpp_traces.size());

   for (size_t ti = 0; ti < abi_traces.size(); ++ti) {
      auto& trace_v = abi_traces[ti];
      BOOST_REQUIRE(trace_v.is_array());
      BOOST_REQUIRE_EQUAL(trace_v.size(), 2u);
      BOOST_REQUIRE_EQUAL(trace_v[0ul].get_string(), "transaction_trace_v0");

      auto& t = trace_v[1ul].get_object();
      auto& cpp = *accepted_cpp_traces[ti].first;
      auto& cpp_ptrx = accepted_cpp_traces[ti].second;

      // -- transaction_trace_v0 fields --
      BOOST_CHECK_EQUAL(t["id"].as<transaction_id_type>(), cpp.id);
      BOOST_CHECK_EQUAL(t["status"].as_uint64(), 0u); // executed
      BOOST_CHECK_EQUAL(t["cpu_usage_us"].as_uint64(), (uint64_t)cpp.total_cpu_usage_us);
      // net_usage_words is rounded up to the nearest multiple of 8 bytes
      uint32_t expected_net_words = ((cpp.net_usage + 7) / 8) * 8;
      BOOST_CHECK_EQUAL(t["net_usage_words"].as_uint64(), expected_net_words);
      BOOST_CHECK_EQUAL(t["elapsed"].as_int64(), 0); // 0 in non-debug mode
      BOOST_CHECK_EQUAL(t["net_usage"].as_uint64(), cpp.net_usage);
      BOOST_CHECK_EQUAL(t["scheduled"].as_bool(), false);

      // -- action_traces --
      auto& abi_ats = t["action_traces"].get_array();
      BOOST_REQUIRE_EQUAL(abi_ats.size(), cpp.action_traces.size());

      for (size_t ai = 0; ai < abi_ats.size(); ++ai) {
         auto& at_v = abi_ats[ai];
         BOOST_REQUIRE(at_v.is_array());
         BOOST_REQUIRE_EQUAL(at_v[0ul].get_string(), "action_trace_v1");

         auto& at = at_v[1ul].get_object();
         auto& cpp_at = cpp.action_traces[ai];

         BOOST_CHECK_EQUAL(at["action_ordinal"].as_uint64(), (uint64_t)cpp_at.action_ordinal);
         BOOST_CHECK_EQUAL(at["creator_action_ordinal"].as_uint64(), (uint64_t)cpp_at.creator_action_ordinal);
         BOOST_CHECK_EQUAL(at["receiver"].as_string(), cpp_at.receiver.to_string());
         BOOST_CHECK_EQUAL(at["context_free"].as_bool(), cpp_at.context_free);
         BOOST_CHECK_EQUAL(at["elapsed"].as_int64(), 0); // 0 in non-debug mode

         // action fields
         auto& act = at["act"].get_object();
         BOOST_CHECK_EQUAL(act["account"].as_string(), cpp_at.act.account.to_string());
         BOOST_CHECK_EQUAL(act["name"].as_string(), cpp_at.act.name.to_string());
         BOOST_CHECK_EQUAL(act["authorization"].get_array().size(), cpp_at.act.authorization.size());

         // return_value (action_trace_v1 field)
         BOOST_CHECK(at["return_value"].as<bytes>() == cpp_at.return_value);

         // receipt
         if (cpp_at.receipt) {
            auto& receipt_v = at["receipt"];
            BOOST_REQUIRE(receipt_v.is_array());
            BOOST_REQUIRE_EQUAL(receipt_v[0ul].get_string(), "action_receipt_v0");

            auto& r = receipt_v[1ul].get_object();
            BOOST_CHECK_EQUAL(r["receiver"].as_string(), cpp_at.receipt->receiver.to_string());
            BOOST_CHECK_EQUAL(r["global_sequence"].as_uint64(), cpp_at.receipt->global_sequence);
            BOOST_CHECK_EQUAL(r["recv_sequence"].as_uint64(), cpp_at.receipt->recv_sequence);
            BOOST_CHECK_EQUAL(r["code_sequence"].as_uint64(), (uint64_t)cpp_at.receipt->code_sequence.value);
            BOOST_CHECK_EQUAL(r["abi_sequence"].as_uint64(), (uint64_t)cpp_at.receipt->abi_sequence.value);

            // auth_sequence
            auto& auth_seq = r["auth_sequence"].get_array();
            BOOST_REQUIRE_EQUAL(auth_seq.size(), cpp_at.receipt->auth_sequence.size());
         }

         // account_ram_deltas
         BOOST_CHECK_EQUAL(at["account_ram_deltas"].get_array().size(), cpp_at.account_ram_deltas.size());
      }

      // -- account_ram_delta (transaction-level) --
      if (cpp.account_ram_delta) {
         auto& delta = t["account_ram_delta"].get_object();
         BOOST_CHECK_EQUAL(delta["account"].as_string(), cpp.account_ram_delta->account.to_string());
         BOOST_CHECK_EQUAL(delta["delta"].as_int64(), cpp.account_ram_delta->delta);
      }

      // -- partial transaction --
      if (cpp_ptrx) {
         auto& partial_v = t["partial"];
         if (partial_v.is_array() && partial_v.size() == 2) {
            BOOST_REQUIRE_EQUAL(partial_v[0ul].get_string(), "partial_transaction_v0");
            auto& p = partial_v[1ul].get_object();
            auto& txn = cpp_ptrx->get_transaction();
            auto& stxn = cpp_ptrx->get_signed_transaction();

            BOOST_CHECK_EQUAL(p["ref_block_num"].as_uint64(), txn.ref_block_num);
            BOOST_CHECK_EQUAL(p["ref_block_prefix"].as_uint64(), txn.ref_block_prefix);
            BOOST_CHECK_EQUAL(p["max_cpu_usage_ms"].as_uint64(), txn.max_cpu_usage_ms);
            BOOST_CHECK_EQUAL(p["max_net_usage_words"].as_uint64(), txn.max_net_usage_words.value);
            BOOST_CHECK_EQUAL(p["delay_sec"].as_uint64(), txn.delay_sec.value);
            BOOST_CHECK_EQUAL(p["signatures"].get_array().size(), stxn.signatures.size());
            BOOST_CHECK_EQUAL(p["context_free_data"].get_array().size(), stxn.context_free_data.size());
         }
      }
   }
}

// Verifies the SHiP ABI definition for finality_data matches the finality_data_t
// C++ struct serialized via fc::raw::pack (FC_REFLECT). Packs the finality data
// from the chain head, deserializes via the ABI, and compares every field.
BOOST_AUTO_TEST_CASE(test_finality_data_abi_roundtrip) {
   savanna_tester chain;

   abi_serializer shipabi(
      fc::json::from_string(sysio::state_history::ship_abi_without_tables()).as<abi_def>(),
      null_yield_function
   );

   // Produce enough blocks for finality data to be fully populated
   chain.produce_blocks(4);

   auto fd = chain.control->head_finality_data();

   // Pack via fc::raw::pack (uses FC_REFLECT fields)
   auto packed = fc::raw::pack(fd);

   // Deserialize via the SHiP ABI definition
   fc::variant v = shipabi.binary_to_variant("finality_data", packed, null_yield_function);
   auto obj = v.get_object();

   BOOST_CHECK_EQUAL(obj["major_version"].as_uint64(), fd.major_version);
   BOOST_CHECK_EQUAL(obj["minor_version"].as_uint64(), fd.minor_version);
   BOOST_CHECK_EQUAL(obj["active_finalizer_policy_generation"].as_uint64(), fd.active_finalizer_policy_generation);
   BOOST_CHECK_EQUAL(obj["action_mroot"].as_string(), fd.action_mroot.str());
   BOOST_CHECK_EQUAL(obj["reversible_blocks_mroot"].as_string(), fd.reversible_blocks_mroot.str());
   BOOST_CHECK_EQUAL(obj["latest_qc_claim_block_num"].as_uint64(), fd.latest_qc_claim_block_num);
   BOOST_CHECK_EQUAL(obj["latest_qc_claim_finality_digest"].as_string(), fd.latest_qc_claim_finality_digest.str());
   BOOST_CHECK_EQUAL(obj["latest_qc_claim_timestamp"].as<block_timestamp_type>(), fd.latest_qc_claim_timestamp);
   BOOST_CHECK_EQUAL(obj["base_digest"].as_string(), fd.base_digest.str());
   BOOST_CHECK_EQUAL(obj["last_pending_finalizer_policy_generation"].as_uint64(), fd.last_pending_finalizer_policy_generation);

   // pending_finalizer_policy
   if (fd.pending_finalizer_policy) {
      auto& pfp = obj["pending_finalizer_policy"];
      BOOST_REQUIRE(!pfp.is_null());
      auto pfp_obj = pfp.get_object();
      BOOST_CHECK_EQUAL(pfp_obj["generation"].as_uint64(), fd.pending_finalizer_policy->generation);
      BOOST_CHECK_EQUAL(pfp_obj["threshold"].as_uint64(), fd.pending_finalizer_policy->threshold);
      BOOST_CHECK_EQUAL(pfp_obj["finalizers"].get_array().size(), fd.pending_finalizer_policy->finalizers.size());

      // Verify each finalizer authority
      auto& abi_fins = pfp_obj["finalizers"].get_array();
      for (size_t i = 0; i < abi_fins.size(); ++i) {
         auto fin = abi_fins[i].get_object();
         BOOST_CHECK_EQUAL(fin["description"].as_string(), fd.pending_finalizer_policy->finalizers[i].description);
         BOOST_CHECK_EQUAL(fin["weight"].as_uint64(), fd.pending_finalizer_policy->finalizers[i].weight);
         BOOST_CHECK_EQUAL(fin["public_key"].as_string(), fd.pending_finalizer_policy->finalizers[i].public_key);
      }
   } else {
      BOOST_CHECK(obj["pending_finalizer_policy"].is_null());
   }
}

// Verifies SHiP ABI definitions for table delta types (account_v0, account_metadata_v0,
// code_v0, resource_limits_v0) by comparing ABI-deserialized values back to the actual
// C++ chainbase objects. Uses full_snapshot=true to get all objects for comparison.
BOOST_AUTO_TEST_CASE(test_delta_abi_roundtrip) {
   table_deltas_tester chain;
   chain.produce_block();

   chain.create_account("newacc"_n, config::system_account_name, false, false, false, false);
   chain.set_code("newacc"_n, test_contracts::get_table_test_wasm());
   chain.set_abi("newacc"_n, test_contracts::get_table_test_abi());

   chain.produce_block();

   chain.push_action("newacc"_n, "addhashobj"_n, "newacc"_n, mutable_variant_object()("hashinput", "hello"));
   chain.push_action("newacc"_n, "addnumobj"_n, "newacc"_n, mutable_variant_object()("input", 2));

   // --- account_v0: compare against account_object in chainbase ---
   {
      auto [found, it] = chain.find_table_delta("account", true);
      BOOST_REQUIRE(found);
      auto accounts = chain.deserialize_data(it, "account_v0", "account");
      BOOST_REQUIRE(!accounts.empty());

      for (auto& acc : accounts) {
         auto name = acc["name"].as<account_name>();
         auto* obj = chain.control->db().find<account_object, by_name>(name);
         BOOST_REQUIRE_MESSAGE(obj != nullptr, "Account " + name.to_string() + " not in chainbase");
         BOOST_CHECK_EQUAL(acc["name"].as_string(), obj->name.to_string());
         BOOST_CHECK_EQUAL(acc["creation_date"].as<block_timestamp_type>(), block_timestamp_type{}); // always zero after removal
      }
   }

   // --- account_metadata_v0: compare against account_metadata_object ---
   {
      auto [found, it] = chain.find_table_delta("account_metadata", true);
      BOOST_REQUIRE(found);
      auto metadata = chain.deserialize_data(it, "account_metadata_v0", "account_metadata");
      BOOST_REQUIRE(!metadata.empty());

      for (auto& m : metadata) {
         auto name = m["name"].as<account_name>();
         auto* obj = chain.control->db().find<account_metadata_object, by_name>(name);
         BOOST_REQUIRE_MESSAGE(obj != nullptr, "Account metadata for " + name.to_string() + " not in chainbase");
         BOOST_CHECK_EQUAL(m["name"].as_string(), obj->name.to_string());
         BOOST_CHECK_EQUAL(m["privileged"].as_bool(), obj->is_privileged());
         BOOST_CHECK_EQUAL(m["last_code_update"].as<fc::time_point>(), obj->last_code_update);

         // code_id optional field
         bool has_code = obj->code_hash != digest_type();
         if (has_code) {
            BOOST_REQUIRE(!m["code"].is_null());
            auto code_id = m["code"].get_object();
            BOOST_CHECK_EQUAL(code_id["vm_type"].as_uint64(), (uint64_t)obj->vm_type);
            BOOST_CHECK_EQUAL(code_id["vm_version"].as_uint64(), (uint64_t)obj->vm_version);
            BOOST_CHECK_EQUAL(code_id["code_hash"].as<digest_type>(), obj->code_hash);
         }
      }
   }

   // --- code_v0: verify code_hash, vm_type, vm_version ---
   {
      auto [found, it] = chain.find_table_delta("code", true);
      BOOST_REQUIRE(found);
      auto codes = chain.deserialize_data(it, "code_v0", "code");
      BOOST_REQUIRE(!codes.empty());

      for (auto& c : codes) {
         auto hash = c["code_hash"].as<digest_type>();
         BOOST_CHECK(hash != digest_type());
         BOOST_CHECK_EQUAL(c["vm_type"].as_uint64(), 0u);
         BOOST_CHECK_EQUAL(c["vm_version"].as_uint64(), 0u);
         // code bytes should be non-empty
         auto code_bytes = c["code"].as<bytes>();
         BOOST_CHECK(!code_bytes.empty());
      }
   }

   // --- contract_row_v0: verify fields match table structure ---
   {
      auto [found, it] = chain.find_table_delta("contract_row");
      BOOST_REQUIRE(found);
      auto rows = chain.deserialize_data(it, "contract_row_v0", "contract_row");
      BOOST_REQUIRE(!rows.empty());

      for (auto& row : rows) {
         // Verify all required fields are present and have the right types
         BOOST_CHECK_EQUAL(row["code"].as_string(), "newacc");
         BOOST_CHECK(!row["table"].as_string().empty());
         BOOST_CHECK(row.get_object().contains("primary_key"));
         BOOST_CHECK_EQUAL(row["payer"].as_string(), "newacc");
         BOOST_CHECK(!row["value"].as<bytes>().empty());
      }
   }

   // --- resource_limits_v0: compare against resource_limits_manager ---
   {
      auto [found, it] = chain.find_table_delta("resource_limits", true);
      BOOST_REQUIRE(found);
      auto limits = chain.deserialize_data(it, "resource_limits_v0", "resource_limits");
      BOOST_REQUIRE(!limits.empty());

      auto& rlm = chain.control->get_resource_limits_manager();
      for (auto& l : limits) {
         auto owner = l["owner"].as<account_name>();
         int64_t ram, net, cpu;
         rlm.get_account_limits(owner, ram, net, cpu);
         BOOST_CHECK_EQUAL(l["ram_bytes"].as_int64(), ram);
         BOOST_CHECK_EQUAL(l["net_weight"].as_int64(), net);
         BOOST_CHECK_EQUAL(l["cpu_weight"].as_int64(), cpu);
      }
   }

   // --- contract_index64_v0: verify secondary index fields ---
   {
      auto [found, it] = chain.find_table_delta("contract_index64");
      if (found) {
         auto indices = chain.deserialize_data(it, "contract_index64_v0", "contract_index64");
         for (auto& idx : indices) {
            BOOST_CHECK_EQUAL(idx["code"].as_string(), "newacc");
            BOOST_CHECK(!idx["table"].as_string().empty());
            BOOST_CHECK(idx.get_object().contains("primary_key"));
            BOOST_CHECK(idx.get_object().contains("secondary_key"));
            BOOST_CHECK_EQUAL(idx["payer"].as_string(), "newacc");
         }
      }
   }

   // --- contract_index256_v0: verify secondary index fields ---
   {
      auto [found, it] = chain.find_table_delta("contract_index256");
      if (found) {
         auto indices = chain.deserialize_data(it, "contract_index256_v0", "contract_index256");
         for (auto& idx : indices) {
            BOOST_CHECK_EQUAL(idx["code"].as_string(), "newacc");
            BOOST_CHECK(!idx["table"].as_string().empty());
            BOOST_CHECK(idx.get_object().contains("primary_key"));
            BOOST_CHECK(idx.get_object().contains("secondary_key"));
            BOOST_CHECK_EQUAL(idx["payer"].as_string(), "newacc");
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
