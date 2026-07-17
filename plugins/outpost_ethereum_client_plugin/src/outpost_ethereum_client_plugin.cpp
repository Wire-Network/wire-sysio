#include <ranges>
#include <fc/log/logger.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

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

   ethereum_client_entry_ptr get_client_by_chain_id(uint64_t chain_id) {
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

   /**
    * A client spec parsed and validated at plugin_initialize, awaiting
    * signature-provider resolution. Client construction happens at
    * plugin_startup so the provider set is complete first: the provider
    * plugins (e.g. signature_provider_ssm_plugin) are `--plugin` peers of
    * this plugin, and resolving at initialize would make boot success depend
    * on their relative order in the config. By startup, dependency ordering
    * has already run the manager's unclaimed-spec check, so a misconfigured
    * scheme fails with its precise error before the lookup here ever runs.
    */
   struct pending_client_t {
      std::string                id;
      std::string                sig_id;
      std::string                url;
      std::optional<fc::uint256> chain_id;
      std::optional<uint64_t>    chain_id_num;
   };

   /** Specs validated at initialize, consumed by `resolve_pending_clients`. */
   std::vector<pending_client_t> _pending_clients{};

   /**
    * Queue a parsed client spec, preserving initialize-time duplicate-id
    * detection (the `add_client` assert would otherwise not fire until
    * startup).
    */
   void add_pending_client(pending_client_t pending) {
      const bool duplicate_id =
         std::ranges::any_of(_pending_clients, [&](const auto& p) { return p.id == pending.id; });
      FC_ASSERT(!duplicate_id, "Client with id {} already exists", pending.id);
      _pending_clients.push_back(std::move(pending));
   }

   /** Resolve each pending spec's provider and construct its client. */
   void resolve_pending_clients() {
      auto& sig_mgr = app().get_plugin<signature_provider_manager_plugin>();
      for (auto& pending : _pending_clients) {
         auto sig_provider = sig_mgr.get_provider(pending.sig_id);
         add_client(pending.id,
                    std::make_shared<ethereum_client_entry_t>(
                       pending.id, pending.url, sig_provider,
                       std::make_shared<ethereum_client>(sig_provider, pending.url, pending.chain_id),
                       pending.chain_id_num));
         ilog("Added ethereum client (id={},sig_id={},url={},chainId={})", pending.id, pending.sig_id,
              pending.url, pending.chain_id_num ? std::to_string(*pending.chain_id_num) : "none");
      }
      _pending_clients.clear();
   }
};

void outpost_ethereum_client_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_abi_file)) {
      auto& abi_files = options.at(option_abi_file).as<std::vector<std::filesystem::path>>();
      my->load_abi_files(abi_files);
   }
   FC_ASSERT(options.count(option_name_client), "At least one ethereum client argument is required {}", option_name_client);
   auto client_specs    = options.at(option_name_client).as<std::vector<std::string>>();
   for (auto& client_spec : client_specs) {
      dlog("Queueing ethereum client with spec: {}", client_spec);
      auto parts = fc::split(client_spec, ',');
      FC_ASSERT(parts.size() == 3 || parts.size() == 4, "Invalid spec {}", client_spec);
      auto& id           = parts[0];
      auto& url          = parts[2];
      auto& sig_id       = parts[1];
      fc::ostring chain_id_str = parts.size() == 4 ? fc::ostring{parts[3]} : fc::ostring{};
      std::optional<fc::uint256> chain_id;
      std::optional<uint64_t>    chain_id_num;
      if (chain_id_str.has_value()) {
         chain_id     = std::make_optional<fc::uint256>(fc::to_uint256(chain_id_str.value()));
         chain_id_num = chain_id->convert_to<uint64_t>();
      }

      // Pure-config validation only at initialize; the signature provider is
      // resolved -- and the client constructed -- at plugin_startup, once the
      // provider plugins have all initialized (see pending_client_t).
      my->add_pending_client({.id           = id,
                              .sig_id       = sig_id,
                              .url          = url,
                              .chain_id     = chain_id,
                              .chain_id_num = chain_id_num});
   }
}

void outpost_ethereum_client_plugin::plugin_startup() {
   ilog("Starting outpost client plugin");
   my->resolve_pending_clients();
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

std::vector<ethereum_client_entry_ptr> outpost_ethereum_client_plugin::get_clients() {
   return my->get_clients();
}

ethereum_client_entry_ptr outpost_ethereum_client_plugin::get_client(const std::string& id) {
   return my->get_client(id);
}

ethereum_client_entry_ptr outpost_ethereum_client_plugin::get_client_by_chain_id(uint64_t chain_id) {
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
