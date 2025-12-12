#include <ranges>
#include <fc/log/logger.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {
// using namespace outpost_client::ethereum;

namespace {
constexpr auto option_name_client     = "outpost-ethereum-client";

inline fc::logger& logger() {
   static fc::logger log{"outpost_ethereum_client_plugin"};
   return log;
}
}

class outpost_ethereum_client_plugin_impl {
   std::map<std::string, ethereum_client_entry_ptr> _clients{};

public:
   std::vector<ethereum_client_entry_ptr> get_clients() {
      return std::views::values(_clients) | std::ranges::to<std::vector>();
   }

   ethereum_client_entry_ptr get_client(const std::string& id) {
      return _clients.at(id);
   }

   void add_client(const std::string& id, ethereum_client_entry_ptr client) {
      FC_ASSERT(client, "Client cannot be null");
      FC_ASSERT(!_clients.contains(id), "Client with id ${id} already exists", ("id", id));
      _clients.emplace(id, client);
   }
};

static auto _outpost_ethereum_client_plugin = application::register_plugin<outpost_ethereum_client_plugin>();

void outpost_ethereum_client_plugin::plugin_initialize(const variables_map& options) {
   FC_ASSERT(options.count(option_name_client), "At least one ethereum client argument is required ${name}", ("name", option_name_client));
   auto plug_sig = app().find_plugin<signature_provider_manager_plugin>();
   auto client_specs    = options.at(option_name_client).as<std::vector<std::string>>();
   for (auto& client_spec : client_specs) {
      dlog("Adding ethereum client with spec: ${spec}", ("spec", client_spec));
      auto parts = fc::split(client_spec, ',');
      FC_ASSERT(parts.size() == 4, "Invalid spec ${spec}", ("spec", client_spec));
      auto& id           = parts[0];
      auto& url          = parts[2];
      auto& sig_id       = parts[1];
      fc::ostring chain_id_str = parts[3];
      std::optional<fc::uint256> chain_id;
      if (chain_id_str.has_value())
         chain_id = std::make_optional<fc::uint256>(fc::parse_uint256(chain_id_str.value()));

      auto  sig_provider = plug_sig->get_provider(sig_id);
      my->add_client(id,
                     std::make_shared<ethereum_client_entry_t>(
                        id,
                        url,
                        sig_provider,
                        std::make_shared<ethereum_client>(sig_provider, url,
                           chain_id)));

      ilogf("Added ethereum client (id={},sig_id={},chainId={},url={})",
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
      boost::program_options::value<std::vector<std::string>>()->multitoken()->required(),
      "Outpost Ethereum Client spec, the plugin supports 1 to many clients in a given process"
      "`<eth-client-id>,<sig-provider-id>,<eth-node-url>[,<eth-chain-id>]`");
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
}