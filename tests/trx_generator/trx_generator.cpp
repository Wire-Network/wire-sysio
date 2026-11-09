#include <trx_generator.hpp>
#include <iostream>
#include <fc/log/logger.hpp>
#include <boost/algorithm/string.hpp>
#include <sysio/chain/chain_id_type.hpp>
#include <boost/program_options.hpp>
#include <sysio/chain/name.hpp>
#include <fc/bitutil.hpp>
#include <fc/io/raw.hpp>
#include <fmt/format.h>
#include <regex>
#include <span>

namespace sysio::testing {
   using namespace chain::literals;
   namespace chain = sysio::chain;

   constexpr chain::name noop_account_name = "sysio.noop"_n;
   constexpr chain::name noop_nonce_action_name = "nonce"_n;

   void trx_generator_base::set_transaction_headers(chain::transaction& trx, const chain::block_id_type& last_irr_block_id, const fc::microseconds& expiration, uint32_t delay_sec) {
      trx.expiration = fc::time_point_sec{fc::time_point::now() + expiration};
      trx.set_reference_block(last_irr_block_id);

      trx.max_net_usage_words = 0;// No limit
      trx.max_cpu_usage_ms = 0;   // No limit
      trx.delay_sec = delay_sec;
   }

   signed_transaction_w_signer trx_generator_base::create_trx_w_actions_and_signer(std::vector<chain::action>&& acts, const fc::crypto::private_key& priv_key,
                                                                                   uint64_t& nonce_prefix, uint64_t& nonce, const fc::microseconds& trx_expiration,
                                                                                   const chain::chain_id_type& chain_id, const chain::block_id_type& last_irr_block_id) {
      chain::signed_transaction trx;
      set_transaction_headers(trx, last_irr_block_id, trx_expiration);
      trx.actions = std::move(acts);
      add_noop_nonce_action(trx, nonce_prefix, nonce);

      trx.sign(priv_key, chain_id);
      return signed_transaction_w_signer(trx, priv_key);
   }

   chain::bytes trx_generator_base::make_noop_nonce_data(uint64_t& nonce_prefix, uint64_t& nonce) const {
      const auto nonce_data = fmt::format("{}:{}:{}:{}", _config._generator_id, nonce_prefix, ++nonce,
                                          fc::time_point::now().time_since_epoch().count());
      return fc::raw::pack(nonce_data);
   }

   void trx_generator_base::add_noop_nonce_action(chain::transaction& trx, uint64_t& nonce_prefix, uint64_t& nonce) {
      if (!_config._noop_nonce) {
         return;
      }
      trx.actions.emplace_back(std::vector<chain::permission_level>(), noop_account_name, noop_nonce_action_name,
                               make_noop_nonce_data(nonce_prefix, nonce));
   }

   void transfer_trx_generator::create_initial_transfer_transactions(uint64_t& nonce_prefix, uint64_t& nonce) {
      std::vector<signed_transaction_w_signer> trxs;
      _trxs.reserve(2 * _action_pairs_vector.size());

      for(const action_pair_w_keys& ap: _action_pairs_vector) {
         _trxs.push_back(create_trx_w_actions_and_signer({ap._first_act}, ap._first_act_priv_key, nonce_prefix, nonce, _config._trx_expiration_us, _config._chain_id,
                                                         _config._last_irr_block_id));
         _trxs.push_back(create_trx_w_actions_and_signer({ap._second_act}, ap._second_act_priv_key, nonce_prefix, nonce, _config._trx_expiration_us, _config._chain_id,
                                                         _config._last_irr_block_id));
      }
   }

   void trx_generator_base::update_resign_transaction(chain::signed_transaction& trx, const fc::crypto::private_key& priv_key, uint64_t& nonce_prefix, uint64_t& nonce,
                                                      const fc::microseconds& trx_expiration, const chain::chain_id_type& chain_id, const chain::block_id_type& last_irr_block_id) {
      if (_config._noop_nonce) {
         // create_trx_w_actions_and_signer appended the nonce action last.
         auto& nonce_action = trx.actions.back();
         SYS_ASSERT(nonce_action.account == noop_account_name && nonce_action.name == noop_nonce_action_name,
                    chain::transaction_exception, "the last action is not the sysio.noop nonce action");
         nonce_action.data = make_noop_nonce_data(nonce_prefix, nonce);
      }
      set_transaction_headers(trx, last_irr_block_id, trx_expiration);
      trx.signatures.clear();
      trx.sign(priv_key, chain_id);
   }

   chain::bytes transfer_trx_generator::make_transfer_data(const chain::name& from, const chain::name& to, const chain::asset& quantity, const std::string& memo) {
      return fc::raw::pack< chain::name>(from, to, quantity, memo);
   }

   auto transfer_trx_generator::make_transfer_action(chain::name account, chain::name from, chain::name to, chain::asset quantity, std::string memo) {
      return chain::action(std::vector<chain::permission_level>{{from, chain::config::active_name}},
                           account, "transfer"_n, make_transfer_data(from, to, quantity, std::move(memo)));
   }

   void transfer_trx_generator::create_initial_transfer_actions(const std::string& salt, const uint64_t& period) {

      for (size_t i = 0; i < _accts_config._acct_name_vec.size(); ++i) {
         for (size_t j = i + 1; j < _accts_config._acct_name_vec.size(); ++j) {
            //create the actions here
            ilog("create_initial_transfer_actions: creating transfer from {} to {}",
                 _accts_config._acct_name_vec.at(i), _accts_config._acct_name_vec.at(j));
            chain::action act_a_to_b = make_transfer_action(_config._contract_owner_account, _accts_config._acct_name_vec.at(i), _accts_config._acct_name_vec.at(j),
                                                            chain::asset::from_string("1.0000 CUR"), salt);

            ilog("create_initial_transfer_actions: creating transfer from {} to {}",
                 _accts_config._acct_name_vec.at(j), _accts_config._acct_name_vec.at(i));
            chain::action act_b_to_a = make_transfer_action(_config._contract_owner_account, _accts_config._acct_name_vec.at(j), _accts_config._acct_name_vec.at(i),
                                                            chain::asset::from_string("1.0000 CUR"), salt);

            _action_pairs_vector.emplace_back(act_a_to_b, act_b_to_a, _accts_config._priv_keys_vec.at(i), _accts_config._priv_keys_vec.at(j));
         }
      }
      ilog("create_initial_transfer_actions: total action pairs created: {}", _action_pairs_vector.size());
   }

   trx_generator_base::trx_generator_base(const trx_generator_base_config& trx_gen_base_config, const provider_base_config& provider_config)
       : _config(trx_gen_base_config), _provider(provider_config) {}

   transfer_trx_generator::transfer_trx_generator(const trx_generator_base_config& trx_gen_base_config, const provider_base_config& provider_config,
                                                  const accounts_config& accts_config)
       : trx_generator_base(trx_gen_base_config, provider_config), _accts_config(accts_config) {}

   bool transfer_trx_generator::setup() {
      const std::string salt = std::to_string(getpid());
      const uint64_t &period = 20;
      _nonce_prefix = 0;
      _nonce = static_cast<uint64_t>(fc::time_point::now().sec_since_epoch()) << 32;

      ilog("Stop Generation (form potential ongoing generation in preparation for starting new generation run).");
      stop_generation();

      ilog("Create All Initial Transfer Action/Reaction Pairs (acct 1 -> acct 2, acct 2 -> acct 1) between all provided accounts.");
      create_initial_transfer_actions(salt, period);

      ilog("Create All Initial Transfer Transactions (one for each created action).");
      create_initial_transfer_transactions(++_nonce_prefix, _nonce);

      ilog("Setup p2p transaction provider");

      ilog("Update each trx to qualify as unique and fresh timestamps, re-sign trx, and send each updated transactions via p2p transaction provider");

      _provider.setup(_config._chain_id, _config._last_irr_block_id);
      return true;
   }

   fc::variant json_from_file_or_string(const std::string& file_or_str, fc::json::parse_type ptype) {
      std::regex r("^[ \t]*[\{\[]");
      if ( !regex_search(file_or_str, r) && std::filesystem::is_regular_file(file_or_str) ) {
         try {
            return fc::json::from_file(file_or_str, ptype);
         } SYS_RETHROW_EXCEPTIONS(chain::json_parse_exception, "Fail to parse JSON from file: {}", file_or_str);

      } else {
         try {
            return fc::json::from_string(file_or_str, ptype);
         } SYS_RETHROW_EXCEPTIONS(chain::json_parse_exception, "Fail to parse JSON from string: {}", file_or_str);
      }
   }

   namespace {
      /// Appends the path of every string under `node` equal to `key_word`; `path` leads from the action to `node`.
      void locate_key_words(std::vector<key_word_path>& paths_out, const fc::variant& node, const std::string& key_word,
                            key_word_path& path) {
         if (node.is_string()) {
            if (node.get_string() == key_word)
               paths_out.push_back(path);
         } else if (node.is_object()) {
            for (const auto& e : node.get_object()) {
               path.emplace_back(e.key());
               locate_key_words(paths_out, e.value(), key_word, path);
               path.pop_back();
            }
         } else if (node.is_array()) {
            const auto& elements = node.get_array();
            for (size_t i = 0; i < elements.size(); ++i) {
               path.emplace_back(i);
               locate_key_words(paths_out, elements[i], key_word, path);
               path.pop_back();
            }
         }
      }

      /// Returns a copy of `node` with the field at `path` set to `replacement`.
      fc::variant replace_at_path(const fc::variant& node, std::span<const key_word_path_step> path,
                                  const std::string& replacement) {
         if (path.empty())
            return fc::variant(replacement);
         return std::visit(chain::overloaded{
            [&](const std::string& key) {
               fc::mutable_variant_object object(node.get_object());
               object.set(key, replace_at_path(object[key], path.subspan(1), replacement));
               return fc::variant(std::move(object));
            },
            [&](size_t index) {
               fc::variants elements = node.get_array();
               elements.at(index) = replace_at_path(elements.at(index), path.subspan(1), replacement);
               return fc::variant(std::move(elements));
            }}, path.front());
      }
   } // namespace

   void locate_key_words_in_action_mvo(std::vector<key_word_path>& acct_gen_fields_out, const fc::mutable_variant_object& action_mvo, const std::string& key_word) {
      key_word_path path;
      locate_key_words(acct_gen_fields_out, fc::variant(action_mvo), key_word, path);
   }

   void locate_key_words_in_action_array(std::map<int, std::vector<key_word_path>>& acct_gen_fields_out, const fc::variants& action_array, const std::string& key_word) {
      for (size_t i = 0; i < action_array.size(); ++i) {
         auto action_mvo = fc::mutable_variant_object(action_array[i]);
         locate_key_words_in_action_mvo(acct_gen_fields_out[i], action_mvo, key_word);
         if(acct_gen_fields_out[i].empty()) {
            acct_gen_fields_out.erase(i);
         }
      }
   }

   void update_key_word_fields_in_action(const std::vector<key_word_path>& acct_gen_fields, fc::mutable_variant_object& action_mvo, const std::string& replacement) {
      for (const auto& path : acct_gen_fields) {
         action_mvo = fc::mutable_variant_object(replace_at_path(fc::variant(action_mvo), path, replacement));
      }
   }

   std::string key_word_path_to_string(const key_word_path& path) {
      std::string out;
      for (const auto& step : path) {
         std::visit(chain::overloaded{
            [&](const std::string& key) {
               if (!out.empty())
                  out += '.';
               out += key;
            },
            [&](size_t index) { out += fmt::format("[{}]", index); }}, step);
      }
      return out;
   }

   bool trx_generator::acct_gen_fields_change_actions() {
      if (_acct_gen_fields.empty()) {
         return false;
      }
      return fc::raw::pack(generate_actions()) != fc::raw::pack(generate_actions());
   }

   void trx_generator::update_resign_transaction(chain::signed_transaction& trx, const fc::crypto::private_key& priv_key, uint64_t& nonce_prefix, uint64_t& nonce,
                                                 const fc::microseconds& trx_expiration, const chain::chain_id_type& chain_id, const chain::block_id_type& last_irr_block_id) {
      trx.actions.clear();
      trx.actions = generate_actions();
      trx_generator_base::update_resign_transaction(trx, priv_key, nonce_prefix, nonce, trx_expiration, chain_id, last_irr_block_id);
   }

   trx_generator::trx_generator(const trx_generator_base_config& trx_gen_base_config, const provider_base_config& provider_config, const user_specified_trx_config& usr_trx_config)
       : trx_generator_base(trx_gen_base_config, provider_config), _usr_trx_config(usr_trx_config), _acct_name_generator() {}

   std::vector<chain::action> trx_generator::generate_actions() {
      std::vector<chain::action> actions;
      actions.reserve(_unpacked_actions.size());

      if (!_acct_gen_fields.empty()) {
         std::string generated_account_name = _acct_name_generator.calc_name();
         _acct_name_generator.increment();

         for (auto const& [key, val] : _acct_gen_fields) {
            update_key_word_fields_in_action(_acct_gen_fields.at(key), _unpacked_actions.at(key), generated_account_name);
         }
      }

      std::transform(_unpacked_actions.begin(), _unpacked_actions.end(), std::back_inserter(actions),
                     [&](const auto& action_mvo) {
                        chain::name action_name = chain::name(action_mvo["actionName"].as_string());
                        chain::name action_auth_acct = chain::name(action_mvo["actionAuthAcct"].as_string());
                        chain::bytes packed_action_data;
                        try {
                           auto action_type = _abi.get_action_type(action_name);
                           FC_ASSERT(!action_type.empty(), "Unknown action {} in contract {}", action_name, action_auth_acct);
                           packed_action_data = _abi.variant_to_binary(action_type, action_mvo["actionData"], chain::abi_serializer::create_yield_function(abi_serializer_max_time));
                        }
                        SYS_RETHROW_EXCEPTIONS(chain::transaction_type_exception, "Fail to parse unpacked action data JSON")

                        std::vector<sysio::chain::permission_level> auth = {};
                        if (action_mvo["authorization"].get_object().find("actor") != action_mvo["authorization"].get_object().end() &&
                            action_mvo["authorization"].get_object().find("permission") != action_mvo["authorization"].get_object().end()) {
                           chain::name auth_actor = chain::name(action_mvo["authorization"].get_object()["actor"].as_string());
                           chain::name auth_perm = chain::name(action_mvo["authorization"].get_object()["permission"].as_string());
                           auth.push_back({auth_actor, auth_perm});
                        }

                        return chain::action(auth, _config._contract_owner_account, action_name, std::move(packed_action_data));
                     });

      return actions;
   }

   bool trx_generator::setup() {
      _nonce_prefix = 0;
      _nonce = static_cast<uint64_t>(fc::time_point::now().sec_since_epoch()) << 32;

      ilog("Stop Generation (form potential ongoing generation in preparation for starting new generation run).");
      stop_generation();

      ilog("Create Initial Transaction with action data.");
      _abi = chain::abi_serializer(fc::json::from_file(_usr_trx_config._abi_data_file_path).as<chain::abi_def>(), chain::abi_serializer::create_yield_function( abi_serializer_max_time ));
      fc::variant unpacked_actions_data_json = json_from_file_or_string(_usr_trx_config._actions_data_json_file_or_str);
      fc::variant unpacked_actions_auths_data_json = json_from_file_or_string(_usr_trx_config._actions_auths_json_file_or_str);
      ilog("Loaded actions data: {}", fc::json::to_pretty_string(unpacked_actions_data_json));
      ilog("Loaded actions auths data: {}", fc::json::to_pretty_string(unpacked_actions_auths_data_json));

      const auto& action_array = unpacked_actions_data_json.get_array();
      _unpacked_actions.reserve(action_array.size());
      std::transform(action_array.begin(), action_array.end(), std::back_inserter(_unpacked_actions),
                     [&](const auto& var) {
                        return fc::mutable_variant_object(var);
                     });
      locate_key_words_in_action_array(_acct_gen_fields, action_array, std::string(acct_per_trx_key_word));

      if (!_acct_gen_fields.empty()) {
         ilog("Located the following account names that need to be generated and populated in each transaction:");
         for (const auto& [action_index, paths] : _acct_gen_fields) {
            for (const auto& path : paths) {
               ilog("acct_gen_fields entry: action {} field {}", action_index, key_word_path_to_string(path));
            }
         }
         ilog("Priming name generator for trx generator prefix.");
         _acct_name_generator.setPrefix(_config._generator_id);
      }

      if (!_config._noop_nonce && !acct_gen_fields_change_actions()) {
         elog("Without the sysio.noop nonce action, the actions data needs an {} field that the ABI serializes, "
              "so each transaction is unique.", acct_per_trx_key_word);
         return false;
      }

      ilog("Setting up transaction signer.");
      fc::crypto::private_key signer_key;
      signer_key = fc::crypto::private_key::from_string(unpacked_actions_auths_data_json.get_object()[_unpacked_actions.at(0)["actionAuthAcct"].as_string()].as_string());

      ilog("Setting up initial transaction actions.");
      auto actions = generate_actions();
      ilog("Initial actions ({}):", _unpacked_actions.size());
      for (size_t i = 0; i < _unpacked_actions.size(); ++i) {
         ilog("Initial action {}: {}", i, fc::json::to_pretty_string(_unpacked_actions.at(i)));
         ilog("Initial action packed data {}: {}", i, fc::to_hex(actions.at(i).data.data(), actions.at(i).data.size()));
      }

      ilog("Populate initial transaction.");
      _trxs.push_back(create_trx_w_actions_and_signer(std::move(actions), signer_key, ++_nonce_prefix, _nonce, _config._trx_expiration_us, _config._chain_id,
                                                      _config._last_irr_block_id));

      ilog("Setup p2p transaction provider");

      ilog("Update each trx to qualify as unique and fresh timestamps and update each action with unique generated account name if necessary,"
           " re-sign trx, and send each updated transactions via p2p transaction provider");

      _provider.setup(_config._chain_id, _config._last_irr_block_id);
      return true;
   }

   bool trx_generator_base::tear_down() {
      _provider.teardown();
      _provider.log_trxs(_config._log_dir);

      ilog("Sent transactions: {}", _txcount);
      ilog("Tear down p2p transaction provider");

      //Stop & Cleanup
      ilog("Stop Generation.");
      stop_generation();
      return true;
   }

   bool trx_generator_base::generate_and_send() {
      try {
         if (_trxs.size()) {
            size_t index_to_send = _txcount % _trxs.size();
            push_transaction(_trxs.at(index_to_send), ++_nonce_prefix, _nonce, _config._trx_expiration_us, _config._chain_id,
                             _config._last_irr_block_id);
            ++_txcount;
         } else {
            elog("no transactions available to send");
            return false;
         }
      } catch (const fc::exception& e) {
         elog("{}", e.to_detail_string());
         return false;
      } catch (const std::exception& e) {
         elog("{}", e.what());
         return false;
      } catch (...) {
         elog("unknown exception");
         return false;
      }

      return true;
   }

   void trx_generator_base::log_first_trx(const std::string& log_dir, const chain::signed_transaction& trx) {
      std::ostringstream fileName;
      fileName << log_dir << "/first_trx_" << getpid() << ".txt";
      std::ofstream out(fileName.str());

      out << trx.id().str() << "\n";
      out.close();
   }

   void trx_generator_base::push_transaction(signed_transaction_w_signer& trx, uint64_t& nonce_prefix, uint64_t& nonce,
                                             const fc::microseconds& trx_expiration, const chain::chain_id_type& chain_id, const chain::block_id_type& last_irr_block_id) {
      const auto reference_block_id = _provider.reference_block_id(last_irr_block_id);
      update_resign_transaction(trx._trx, trx._signer, ++nonce_prefix, nonce, trx_expiration, chain_id, reference_block_id);
      if (_txcount == 0) {
         log_first_trx(_config._log_dir, trx._trx);
      }
      _provider.send(trx._trx);
   }

   void trx_generator_base::stop_generation() {
      ilog("Stopping transaction generation");

      if (_txcount) {
         ilog("{} transactions executed, {}us / transaction", _txcount, _total_us / (double) _txcount);
         _txcount = _total_us = 0;
      }
   }

   bool trx_generator_base::stop_on_trx_fail() {
      return _config._stop_on_trx_failed;
   }
}
