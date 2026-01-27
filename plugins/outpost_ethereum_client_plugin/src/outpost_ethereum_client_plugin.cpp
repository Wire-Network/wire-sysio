#include <ranges>
#include <fc/log/logger.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {
// using namespace outpost_client::ethereum;

namespace {
constexpr auto option_name_client     = "outpost-ethereum-client";
constexpr auto option_abi_file     = "ethereum-abi-file";

auto _register_outpost_ethereum_client_plugin = application::register_plugin<outpost_ethereum_client_plugin>();

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
      fc::ostring chain_id_str = parts[3];
      std::optional<fc::uint256> chain_id;
      if (chain_id_str.has_value())
         chain_id = std::make_optional<fc::uint256>(fc::to_uint256(chain_id_str.value()));

      auto  sig_provider = plug_sig->get_provider(sig_id);
      my->add_client(id,
                     std::make_shared<ethereum_client_entry_t>(
                        id,
                        url,
                        sig_provider,
                        std::make_shared<ethereum_client>(sig_provider, url,
                           chain_id)));

      ilog("Added ethereum client (id={},sig_id={},chainId={},url={})",
           id,sig_id,url,chain_id_str.value_or("none"));
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

std::vector<ethereum_client_entry_ptr> outpost_ethereum_client_plugin::get_clients() {
   return my->get_clients();
}

ethereum_client_entry_ptr outpost_ethereum_client_plugin::get_client(const std::string& id) {
   return my->get_client(id);
}

const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>>& outpost_ethereum_client_plugin::get_abi_files() {
   return my->get_abi_files();
}
} // namespace sysio