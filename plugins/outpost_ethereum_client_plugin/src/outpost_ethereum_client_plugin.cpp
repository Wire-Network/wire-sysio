#include <ranges>
#include <regex>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <sysio/chain/exceptions.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {
// using namespace outpost_client::ethereum;

namespace {
constexpr auto option_name_client     = "outpost-ethereum-client";
constexpr auto option_abi_file     = "ethereum-abi-file";

[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_ethereum_client_plugin"};
   return log;
}
}

class outpost_ethereum_client_plugin_impl {
   std::map<std::string, ethereum_client_entry_ptr> _clients{};
   using file_abi_contracts_t = std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>;
   std::vector<file_abi_contracts_t> _abi_files{};

public:
   std::vector<file_abi_contracts_t> load_abi_files(const std::vector<std::filesystem::path>& file_names) {
      static std::mutex mutex;
      std::scoped_lock lock(mutex);

      for (auto& filename : file_names) {
         FC_ASSERT_FMT(exists(filename), "File does not exist: {}", filename.string());
         auto file_path = std::filesystem::absolute(filename);
         ilog("Loading ABI file: {}", file_path.string());
         if (!std::ranges::none_of(_abi_files, [&](const auto& f) { return f.first == file_path; })) {
            wlog("Already registered ABI file: {}", file_path.string());
            continue;
         }
         _abi_files.emplace_back(file_path, fc::network::ethereum::abi::parse_contracts(file_path));
      }

      return _abi_files;
   }
   std::vector<ethereum_client_entry_ptr> get_clients() {
      return std::views::values(_clients) | std::ranges::to<std::vector>();
   }

   ethereum_client_entry_ptr get_client(const std::string& id) {
      return _clients.at(id);
   }

   void add_client(const std::string& id, ethereum_client_entry_ptr client) {
      FC_ASSERT(client, "Client cannot be null");
      FC_ASSERT(!_clients.contains(id), "Client with id {} already exists", id);
      _clients.emplace(id, client);
   }

   const std::vector<file_abi_contracts_t>& get_abi_files() {
      return _abi_files;
   };
};

void outpost_ethereum_client_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_abi_file)) {
      auto& abi_files = options.at(option_abi_file).as<std::vector<std::filesystem::path>>();
      my->load_abi_files(abi_files);
   }
   FC_ASSERT(options.count(option_name_client), "At least one ethereum client argument is required {}", option_name_client);
   auto plug_sig = app().find_plugin<signature_provider_manager_plugin>();
   auto client_specs    = options.at(option_name_client).as<std::vector<std::string>>();
   for (auto& client_spec : client_specs) {
      dlog("Adding ethereum client with spec: {}", client_spec);
      auto parts = fc::split(client_spec, ',');
      FC_ASSERT(parts.size() == 3 || parts.size() == 4, "Invalid spec {}", client_spec);
      auto& id           = parts[0];
      auto& url          = parts[2];
      auto& sig_id       = parts[1];
      std::optional<fc::uint256> chain_id;
      fc::ostring chain_id_str;
      if (parts.size() == 4) {
         chain_id_str = parts[3];
         if (chain_id_str.has_value())
            chain_id = fc::to_uint256(chain_id_str.value());
      } else {
         ilog("chainId: none");
      }

      auto  sig_provider = plug_sig->get_provider(sig_id);
      my->add_client(id,
                     std::make_shared<ethereum_client_entry_t>(
                        id,
                        url,
                        sig_provider,
                        std::make_shared<ethereum_client>(sig_provider, url,
                           chain_id)));

      ilog("Added ethereum client (id={},sig_id={},chainId={},url={})",
           id,sig_id,chain_id_str.value_or("none"),url);
   }
}

void outpost_ethereum_client_plugin::plugin_startup() {
   ilog("Starting outpost client plugin");
}


outpost_ethereum_client_plugin::outpost_ethereum_client_plugin() : my(
   std::make_unique<outpost_ethereum_client_plugin_impl>()) {}

void outpost_ethereum_client_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()(
      option_name_client,
      boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Outpost Ethereum Client spec, the plugin supports 1 to many clients in a given process"
      "`<eth-client-id>,<sig-provider-id>,<eth-node-url>[,<eth-chain-id>]`")(
      option_abi_file,
      boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
      "Ethereum contract ABI file(s).  Expects the file to have a JSON array of ABI complient contract definitions."
      );
}


void outpost_ethereum_client_plugin::plugin_shutdown() {
   ilog("Shutdown outpost client plugin");
}

std::vector<ethereum_client_entry_ptr> outpost_ethereum_client_plugin::get_clients() const {
   return my->get_clients();
}

ethereum_client_entry_ptr outpost_ethereum_client_plugin::get_client(const std::string& id) const {
   return my->get_client(id);
}

ethereum_client_ptr outpost_ethereum_client_plugin::get_client_for_chain(fc::crypto::chain_kind_t target_chain) const {
   ethereum_client_ptr result;
   for (const auto& entry : my->get_clients()) {
      if (target_chain == entry->signature_provider->target_chain) {
         SYS_ASSERT(!result, sysio::chain::plugin_config_exception,
                    "There should only be one ethereum client for chain kind {}, but there were at least 2",
                    static_cast<int>(target_chain));
         result = entry->client;
      }
   }
   SYS_ASSERT(!!result, sysio::chain::plugin_config_exception,
              "could not find any ethereum client for chain kind {}", static_cast<int>(target_chain));
   return result;
}

std::vector<fc::network::ethereum::abi::contract> outpost_ethereum_client_plugin::get_abis_for_contract(const std::string& contract_name) const {
   static const std::regex contract_regex(R"(^(.+?)(?:V\d+)?$)");
   constexpr auto contract_name_field = "contractName";
   std::vector<fc::network::ethereum::abi::contract> result;

   for (const auto& [json_abi_file, abi_contracts] : my->get_abi_files()) {
      auto json_var = fc::json::from_file(json_abi_file);
      if (!json_var.is_object())
         continue;

      const auto var_obj = json_var.get_object();
      if (!var_obj.contains(contract_name_field))
         continue;

      const auto name_var = var_obj[contract_name_field];
      if (name_var.is_array())
         continue;

      const auto name = name_var.as<std::string>();

      std::smatch matches;
      if (!std::regex_search(name, matches, contract_regex))
         continue;

      if (matches[1].str() != contract_name)
         continue;

      result.insert(result.end(), abi_contracts.begin(), abi_contracts.end());
      break;
   }

   return result;
}

const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>>& outpost_ethereum_client_plugin::get_abi_files() const {
   return my->get_abi_files();
}
} // namespace sysio