#include <ranges>
#include <limits>

#include <fc/log/logger.hpp>
#include <fc/task/deadline.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

namespace sysio {
// using namespace outpost_client::ethereum;

namespace {
constexpr auto option_name_client         = "outpost-ethereum-client";
constexpr auto option_abi_file            = "ethereum-abi-file";
constexpr auto chain_id_validation_timeout = fc::seconds(5);

[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_ethereum_client_plugin"};
   return log;
}

/** Parse a positive decimal or Ethereum hex quantity without fixed-width wraparound. */
std::optional<uint32_t> parse_chain_id(std::string_view text) {
   if (text.empty()) {
      return std::nullopt;
   }

   uint32_t base = 10;
   size_t offset = 0;
   if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      base = 16;
      offset = 2;
   }
   if (offset == text.size()) {
      return std::nullopt;
   }

   uint32_t value = 0;
   for (; offset < text.size(); ++offset) {
      const char c = text[offset];
      uint32_t digit;
      if (c >= '0' && c <= '9') {
         digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
         digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
         digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
         return std::nullopt;
      }
      if (digit >= base || value > (std::numeric_limits<uint32_t>::max() - digit) / base) {
         return std::nullopt;
      }
      value = value * base + digit;
   }

   return value == 0 ? std::nullopt : std::optional<uint32_t>{value};
}

/** Construct the client and verify any explicit chain id within one startup deadline. */
ethereum_client_ptr create_validated_client(
   const std::string&                         client_id,
   const fc::crypto::signature_provider_ptr& signature_provider,
   const std::string&                         url,
   const std::optional<uint32_t>&              configured_chain_id) {
   fc::task::deadline_scope deadline(fc::time_point::now() + chain_id_validation_timeout);

   ethereum_client_ptr client;
   try {
      const auto client_chain_id = configured_chain_id
         ? std::optional<fc::uint256>{fc::uint256{*configured_chain_id}}
         : std::nullopt;
      client = std::make_shared<ethereum_client>(signature_provider, url, client_chain_id);
   } catch (const fc::exception&) {
      FC_THROW_EXCEPTION(
         chain::plugin_config_exception,
         "Unable to initialize outpost Ethereum client '{}': the configured RPC endpoint is invalid or could not be resolved",
         client_id);
   } catch (const std::exception&) {
      FC_THROW_EXCEPTION(
         chain::plugin_config_exception,
         "Unable to initialize outpost Ethereum client '{}': the configured RPC endpoint is invalid or could not be resolved",
         client_id);
   }

   if (configured_chain_id) {
      std::string remote_chain_id_text;
      try {
         remote_chain_id_text = client->execute("eth_chainId", fc::variants{}).as_string();
      } catch (const fc::exception&) {
         FC_THROW_EXCEPTION(
            chain::plugin_config_exception,
            "Unable to validate chain id for outpost Ethereum client '{}': "
            "the configured RPC endpoint did not return a valid eth_chainId",
            client_id);
      } catch (const std::exception&) {
         FC_THROW_EXCEPTION(
            chain::plugin_config_exception,
            "Unable to validate chain id for outpost Ethereum client '{}': "
            "the configured RPC endpoint did not return a valid eth_chainId",
            client_id);
      }

      const auto remote_chain_id = parse_chain_id(remote_chain_id_text);
      SYS_ASSERT(remote_chain_id,
                 chain::plugin_config_exception,
                 "Unable to validate chain id for outpost Ethereum client '{}': "
                 "the configured RPC endpoint did not return a valid 32-bit eth_chainId",
                 client_id);

      SYS_ASSERT(*remote_chain_id == *configured_chain_id,
                 chain::plugin_config_exception,
                 "Chain id mismatch for outpost Ethereum client '{}': configured {}, RPC endpoint reports {}",
                 client_id,
                 *configured_chain_id,
                 *remote_chain_id);
   }

   return client;
}
}

class outpost_ethereum_client_plugin_impl {
   std::map<std::string, ethereum_client_entry_ptr> _clients{};
   using file_abi_contracts_t = std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>;
   std::vector<file_abi_contracts_t> _abi_files{};

public:
   // Called only from plugin_initialize -- sequential, main-thread -- so the ABI list needs no synchronization.
   std::vector<file_abi_contracts_t> load_abi_files(const std::vector<std::filesystem::path>& file_names) {
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

   ethereum_client_entry_ptr get_client_by_chain_id(uint32_t chain_id) {
      ethereum_client_entry_ptr match;
      for (auto& [id, entry] : _clients) {
         if (entry->chain_id && *entry->chain_id == chain_id) {
            if (match) return nullptr;  // ambiguous: >1 client on this chain id
            match = entry;
         }
      }
      return match;  // nullptr when none matched
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

   // This plugin APPBASE_PLUGIN_REQUIRES the signature_provider_manager_plugin, which creates every configured provider
   // at its own plugin_initialize (failing the boot there on a misconfigured or not-enabled scheme). So by the time
   // this runs, every provider already exists regardless of `--plugin` ordering, and clients can be resolved and
   // constructed here rather than deferred to startup.
   auto& sig_mgr        = app().get_plugin<signature_provider_manager_plugin>();
   auto client_specs    = options.at(option_name_client).as<std::vector<std::string>>();
   for (auto& client_spec : client_specs) {
      dlog("Adding ethereum client with spec: {}", client_spec);
      auto parts = fc::split(client_spec, ',');
      SYS_ASSERT(parts.size() == 3 || parts.size() == 4,
                 chain::plugin_config_exception,
                 "Invalid {} spec '{}' (expected: <client-id>,<sig-provider-id>,<rpc-url>[,<chain-id>])",
                 option_name_client,
                 client_spec);
      auto& id           = parts[0];
      auto& sig_id       = parts[1];
      auto& url          = parts[2];
      SYS_ASSERT(!id.empty(), chain::plugin_config_exception,
                 "Invalid {} spec: Ethereum client id must not be empty", option_name_client);
      SYS_ASSERT(!sig_id.empty(), chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': signer name must not be empty", option_name_client, id);
      SYS_ASSERT(!url.empty(), chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': RPC URL must not be empty", option_name_client, id);
      SYS_ASSERT(sig_mgr.is_explicitly_configured_provider(sig_id),
                 chain::plugin_config_exception,
                 "Outpost Ethereum client '{}' references signer '{}', but no explicitly named "
                 "--signature-provider with that name was specified",
                 id,
                 sig_id);

      fc::ostring chain_id_str = parts.size() == 4 ? fc::ostring{parts[3]} : fc::ostring{};
      std::optional<uint32_t> chain_id;
      if (chain_id_str.has_value()) {
         SYS_ASSERT(!chain_id_str->empty(), chain::plugin_config_exception,
                    "Invalid {} spec for client '{}': chain id must not be empty", option_name_client, id);
         chain_id = parse_chain_id(*chain_id_str);
         SYS_ASSERT(chain_id,
                    chain::plugin_config_exception,
                    "Invalid {} spec for client '{}': chain id must be a positive 32-bit decimal or hex integer",
                    option_name_client,
                    id);
      }

      auto sig_provider = sig_mgr.get_provider(sig_id);
      SYS_ASSERT(sig_provider->target_chain == fc::crypto::chain_kind_ethereum &&
                    sig_provider->key_type == fc::crypto::chain_key_type_ethereum,
                 chain::plugin_config_exception,
                 "Outpost Ethereum client '{}' signer '{}' must use chain=ethereum and key-type=ethereum",
                 id,
                 sig_id);

      auto eth_client = create_validated_client(id, sig_provider, url, chain_id);
      my->add_client(id,
                     std::make_shared<ethereum_client_entry_t>(
                        id, url, sig_provider,
                        std::move(eth_client),
                        chain_id));
      ilog("Added ethereum client (id={},sig_id={},url={},chainId={})", id, sig_id, url,
           chain_id ? std::to_string(*chain_id) : "none");
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
      "Outpost Ethereum Client spec, the plugin supports 1 to many clients in a given process: "
      "`<eth-client-id>,<sig-provider-id>,<eth-node-url>[,<eth-chain-id>]`. The signer id must "
      "match an explicitly named --signature-provider; an explicit chain ID is checked against "
      "the endpoint's eth_chainId response during startup")(
      option_abi_file,
      boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
      "Ethereum contract ABI file(s).  Expects the file to have a JSON array of ABI complient contract definitions."
      );
}


void outpost_ethereum_client_plugin::plugin_shutdown() {
   ilog("Shutdown outpost client plugin");
}

std::vector<ethereum_client_entry_ptr> outpost_ethereum_client_plugin::get_clients() {
   return my->get_clients();
}

ethereum_client_entry_ptr outpost_ethereum_client_plugin::get_client(const std::string& id) {
   return my->get_client(id);
}

ethereum_client_entry_ptr outpost_ethereum_client_plugin::get_client_by_chain_id(uint32_t chain_id) {
   return my->get_client_by_chain_id(chain_id);
}

const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>>& outpost_ethereum_client_plugin::get_abi_files() {
   return my->get_abi_files();
}

std::shared_ptr<outpost_client>
outpost_ethereum_client_plugin::create_outpost_client(const std::string& eth_client_id,
                                                    uint64_t           chain_code,
                                                    uint32_t           chain_id,
                                                    const std::string& opp_addr,
                                                    const std::string& opp_inbound_addr,
                                                    const std::string& operator_registry_addr) {
   auto entry = my->get_client(eth_client_id);
   FC_ASSERT(entry, "Unknown ethereum client id: {}", eth_client_id);

   std::vector<fc::network::ethereum::abi::contract> all_abis;
   for (auto& [path, contracts] : my->get_abi_files()) {
      all_abis.insert(all_abis.end(), contracts.begin(), contracts.end());
   }
   return std::make_shared<outpost_ethereum_client>(entry,
                                                    opp_addr,
                                                    opp_inbound_addr,
                                                    operator_registry_addr,
                                                    std::move(all_abis),
                                                    chain_code,
                                                    chain_id);
}

} // namespace sysio
