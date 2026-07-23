#include <ranges>

#include <fc/log/logger.hpp>

#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

namespace sysio {

namespace {
constexpr auto option_name_client          = "outpost-solana-client";
constexpr auto option_idl_file             = "solana-idl-file";
constexpr auto option_outpost_program_name = "solana-outpost-program-name";
constexpr auto option_additional_ca_file   = "outpost-solana-additional-ca-file";
constexpr auto option_additional_ca_path   = "outpost-solana-additional-ca-path";
constexpr auto option_proxy                = "outpost-solana-proxy";
constexpr uint64_t solana_rpc_max_request_bytes = 1ULL * 1024ULL * 1024ULL;
constexpr uint64_t solana_rpc_max_response_bytes = 4ULL * 1024ULL * 1024ULL;

[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_solana_client_plugin"};
   return log;
}

/** Build the named bounded transport policy shared by configured Solana RPC clients. */
fc::network::json_rpc::client_options solana_rpc_options(const variables_map& options) {
   fc::network::json_rpc::client_options result{
      .request =
         fc::http::request_options{
            .max_request_body_bytes = solana_rpc_max_request_bytes,
            .max_response_body_bytes = solana_rpc_max_response_bytes,
            .timeouts =
               fc::http::timeout_options{
                  .connect = fc::seconds(10),
                  .header = fc::seconds(30),
                  .read = fc::seconds(30),
                  .idle = fc::seconds(30),
                  .total = fc::seconds(30),
               },
         },
   };
   if (options.contains(option_additional_ca_file))
      result.transport.additional_ca_file =
         options.at(option_additional_ca_file).as<std::filesystem::path>();
   if (options.contains(option_additional_ca_path))
      result.transport.additional_ca_path =
         options.at(option_additional_ca_path).as<std::filesystem::path>();
   if (options.contains(option_proxy))
      result.transport.proxy = options.at(option_proxy).as<std::string>();
   return result;
}
} // namespace

class outpost_solana_client_plugin_impl {
   std::map<std::string, solana_client_entry_ptr> _clients{};
   using file_idl_programs_t = std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>;
   std::vector<file_idl_programs_t> _idl_files{};
   std::string _outpost_program_name{OPP_SOLANA_OUTPOST_PROGRAM_NAME};

public:
   void set_outpost_program_name(std::string name) {
      FC_ASSERT(!name.empty(), "--{} cannot be empty", option_outpost_program_name);
      _outpost_program_name = std::move(name);
   }

   const std::string& outpost_program_name() const {
      return _outpost_program_name;
   }

   // Called only from plugin_initialize -- sequential, main-thread -- so the IDL list needs no synchronization.
   std::vector<file_idl_programs_t> load_idl_files(const std::vector<std::filesystem::path>& file_names) {
      for (auto& filename : file_names) {
         auto file_path = std::filesystem::absolute(filename);
         ilog("Loading IDL file: {}", file_path.string());
         if (!std::ranges::none_of(_idl_files, [&](const auto& f) { return f.first == file_path; })) {
            wlog("Already registered IDL file: {}", file_path.string());
            continue;
         }
         // Parse each IDL file as a single program definition
         auto program = fc::network::solana::idl::parse_idl_file(file_path.string());
         _idl_files.emplace_back(file_path, std::vector<fc::network::solana::idl::program>{std::move(program)});
      }

      return _idl_files;
   }

   std::vector<solana_client_entry_ptr> get_clients() {
      return std::views::values(_clients) | std::ranges::to<std::vector>();
   }

   solana_client_entry_ptr get_client(const std::string& id) {
      return _clients.at(id);
   }

   void add_client(const std::string& id, solana_client_entry_ptr client) {
      FC_ASSERT(client, "Client cannot be null");
      FC_ASSERT(!_clients.contains(id), "Client with id {} already exists", id);
      _clients.emplace(id, client);
   }

   const std::vector<file_idl_programs_t>& get_idl_files() {
      return _idl_files;
   }

};

outpost_solana_client_plugin::outpost_solana_client_plugin()
   : my(std::make_unique<outpost_solana_client_plugin_impl>()) {}

outpost_solana_client_plugin::~outpost_solana_client_plugin() = default;

void outpost_solana_client_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_idl_file)) {
      auto& idl_files = options.at(option_idl_file).as<std::vector<std::filesystem::path>>();
      my->load_idl_files(idl_files);
   }
   my->set_outpost_program_name(options.at(option_outpost_program_name).as<std::string>());
   ilog("Solana OPP outpost program name: {}", my->outpost_program_name());
   FC_ASSERT(options.count(option_name_client),
             "At least one solana client argument is required {}",
             option_name_client);

   // This plugin APPBASE_PLUGIN_REQUIRES the signature_provider_manager_plugin, which creates every configured provider
   // at its own plugin_initialize (failing the boot there on a misconfigured or not-enabled scheme). So by the time
   // this runs, every provider already exists regardless of `--plugin` ordering, and clients can be resolved and
   // constructed here rather than deferred to startup.
   auto& sig_mgr      = app().get_plugin<signature_provider_manager_plugin>();
   auto client_specs  = options.at(option_name_client).as<std::vector<std::string>>();
   const auto rpc_options = solana_rpc_options(options);

   for (auto& client_spec : client_specs) {
      dlog("Adding configured Solana client");
      auto parts = fc::split(client_spec, ',');
      SYS_ASSERT(parts.size() == 3,
                 chain::plugin_config_exception,
                 "Invalid {} spec '{}' (expected: <client-id>,<sig-provider-id>,<rpc-url>)",
                 option_name_client,
                 client_spec);

      auto& id     = parts[0];
      auto& sig_id = parts[1];
      auto& url    = parts[2];
      SYS_ASSERT(!id.empty(), chain::plugin_config_exception,
                 "Invalid {} spec: Solana client id must not be empty", option_name_client);
      SYS_ASSERT(!sig_id.empty(), chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': signer name must not be empty", option_name_client, id);
      SYS_ASSERT(!url.empty(), chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': RPC URL must not be empty", option_name_client, id);
      SYS_ASSERT(sig_mgr.is_explicitly_configured_provider(sig_id),
                 chain::plugin_config_exception,
                 "Outpost Solana client '{}' references signer '{}', but no explicitly named "
                 "--signature-provider with that name was specified",
                 id,
                 sig_id);

      auto sig_provider = sig_mgr.get_provider(sig_id);
      SYS_ASSERT(sig_provider->target_chain == fc::crypto::chain_kind_solana &&
                    sig_provider->key_type == fc::crypto::chain_key_type_solana,
                 chain::plugin_config_exception,
                 "Outpost Solana client '{}' signer '{}' must use chain=solana and key-type=solana",
                 id,
                 sig_id);

      my->add_client(id, std::make_shared<solana_client_entry_t>(
                            id, url, sig_provider,
                            std::make_shared<solana_client>(sig_provider, url, rpc_options)));
      ilog("Added solana client (id={},sig_id={},endpoint={})",
           id, sig_id, fc::http::sanitized_endpoint(fc::url(url)));
   }
}

void outpost_solana_client_plugin::plugin_startup() {
   ilog("Starting outpost solana client plugin");
}

void outpost_solana_client_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()(
      option_name_client,
      boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Outpost Solana Client spec, the plugin supports 1 to many clients in a given process. "
      "Format: `<sol-client-id>,<sig-provider-id>,<rpc-url>`. The signer id must "
      "match an explicitly named --signature-provider with the Solana target chain and key type")(
      option_idl_file,
      boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
      "Solana program IDL file(s). Expects each file to be a JSON IDL (Anchor format) program definition.")(
      option_outpost_program_name,
      boost::program_options::value<std::string>()->default_value(OPP_SOLANA_OUTPOST_PROGRAM_NAME),
      "Anchor IDL program name of the Solana OPP outpost. The loaded --solana-idl-file set is filtered to "
      "programs with this name when constructing outpost clients. The default targets the standalone "
      "opp_outpost program; pass liqsol_core when the outpost interface is hosted inside the liqsol-core "
      "program (clean-room layout).")(
      option_additional_ca_file,
      boost::program_options::value<std::filesystem::path>(),
      "PEM CA bundle added to system trust for Solana RPC HTTPS clients.")(
      option_additional_ca_path,
      boost::program_options::value<std::filesystem::path>(),
      "Hashed CA directory added to system trust for Solana RPC HTTPS clients.")(
      option_proxy,
      boost::program_options::value<std::string>(),
      "Explicit proxy URL for Solana RPC HTTP clients.");
}

void outpost_solana_client_plugin::plugin_shutdown() {
   ilog("Shutdown outpost solana client plugin");
}

std::vector<solana_client_entry_ptr> outpost_solana_client_plugin::get_clients() {
   return my->get_clients();
}

solana_client_entry_ptr outpost_solana_client_plugin::get_client(const std::string& id) {
   return my->get_client(id);
}

const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>&
outpost_solana_client_plugin::get_idl_files() {
   return my->get_idl_files();
}

std::shared_ptr<outpost_client>
outpost_solana_client_plugin::create_outpost_client(const std::string&  sol_client_id,
                                                  uint64_t            chain_code,
                                                  uint32_t            chain_id,
                                                  const std::string&  program_id,
                                                  solana_outpost_role role) {
   auto entry = my->get_client(sol_client_id);
   FC_ASSERT(entry, "Unknown solana client id: {}", sol_client_id);
   FC_ASSERT(!program_id.empty(), "Solana program id is required");

   auto program_key = fc::crypto::solana::solana_public_key::from_base58_string(program_id);

   // Filter the loaded IDL set down to programs whose name matches the
   // configured OPP outpost program name so we don't construct a client
   // around an unrelated IDL.
   auto program_idls = filter_outpost_program_idls(my->get_idl_files(), my->outpost_program_name());
   FC_ASSERT(!program_idls.empty(),
             "IDL for program '{}' not loaded — pass --solana-idl-file (and --{} when the outpost "
             "IDL uses a different program name)",
             my->outpost_program_name(),
             option_outpost_program_name);

   return std::make_shared<outpost_solana_client>(
      entry, program_key, std::move(program_idls), chain_code, chain_id, role);
}

std::vector<fc::network::solana::idl::program> filter_outpost_program_idls(
   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>& idl_files,
   std::string_view program_name) {
   std::vector<fc::network::solana::idl::program> program_idls;
   for (auto& [path, programs] : idl_files) {
      for (auto& p : programs) {
         if (p.name == program_name) {
            program_idls.push_back(p);
         }
      }
   }
   return program_idls;
}

} // namespace sysio
