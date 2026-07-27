#include <fc/log/logger.hpp>
#include <magic_enum/magic_enum.hpp>
#include <ranges>
#include <set>

#include <sysio/http_client_plugin/http_client_options.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>
#include <sysio/outpost_client/rpc_options.hpp>

namespace sysio {

namespace {
constexpr auto option_name_client = "outpost-solana-client";
constexpr auto option_name_cluster_identity = "outpost-solana-cluster-identity";
constexpr auto option_name_identity_max_age = "outpost-solana-cluster-identity-max-age-ms";
constexpr auto option_name_identity_timeout = "outpost-solana-cluster-identity-probe-timeout-ms";
constexpr auto option_idl_file = "solana-idl-file";
constexpr auto option_outpost_program_name = "solana-outpost-program-name";
constexpr outbound_http::transport_option_names
   transport_option_names{
      .additional_ca_file =
         "outpost-solana-additional-ca-file",
      .additional_ca_path =
         "outpost-solana-additional-ca-path",
      .proxy = "outpost-solana-proxy",
   };
constexpr uint32_t default_identity_max_age_ms = 30'000;
constexpr uint32_t default_identity_probe_timeout_ms = 5'000;

/** Credential-bearing client configuration parsed before any client is published. */
struct parsed_client_spec {
   std::string id;
   std::string signature_provider_id;
   std::string rpc_url;
   fc::url parsed_rpc_url;
};

[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_solana_client_plugin"};
   return log;
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

   const std::string& outpost_program_name() const { return _outpost_program_name; }

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

   std::vector<solana_cluster_identity_snapshot> get_cluster_identity_snapshots() {
      std::vector<solana_cluster_identity_snapshot> snapshots;
      snapshots.reserve(_clients.size());
      for (const auto& [client_id, entry] : _clients) {
         auto snapshot = entry->client->get_cluster_identity_snapshot();
         if (snapshot.client_id.empty())
            snapshot.client_id = client_id;
         snapshots.push_back(std::move(snapshot));
      }
      return snapshots;
   }

   solana_client_entry_ptr get_client(const std::string& id) { return _clients.at(id); }

   void add_client(const std::string& id, solana_client_entry_ptr client) {
      FC_ASSERT(client, "Client cannot be null");
      FC_ASSERT(!_clients.contains(id), "Client with id {} already exists", id);
      _clients.emplace(id, client);
   }

   const std::vector<file_idl_programs_t>& get_idl_files() { return _idl_files; }
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
   FC_ASSERT(options.count(option_name_client), "At least one solana client argument is required {}",
             option_name_client);

   // This plugin APPBASE_PLUGIN_REQUIRES the signature_provider_manager_plugin, which creates every configured provider
   // at its own plugin_initialize (failing the boot there on a misconfigured or not-enabled scheme). So by the time
   // this runs, every provider already exists regardless of `--plugin` ordering, and clients can be resolved and
   // constructed here rather than deferred to startup.
   auto& sig_mgr      = app().get_plugin<signature_provider_manager_plugin>();
   auto client_specs  = options.at(option_name_client).as<std::vector<std::string>>();
   const auto rpc_options =
      outpost_rpc::rpc_options(
         options,
         transport_option_names);
   std::vector<parsed_client_spec> parsed_clients;
   parsed_clients.reserve(client_specs.size());
   std::set<std::string> client_ids;

   for (const auto& client_spec : client_specs) {
      auto parts = fc::split(client_spec, ',');
      SYS_ASSERT(parts.size() == 3, chain::plugin_config_exception,
                 "Invalid {} spec (expected exactly: <client-id>,<sig-provider-id>,<rpc-url>)", option_name_client);

      auto& id = parts[0];
      auto& sig_id = parts[1];
      auto& url = parts[2];
      SYS_ASSERT(!id.empty(), chain::plugin_config_exception, "Invalid {} spec: Solana client id must not be empty",
                 option_name_client);
      SYS_ASSERT(!sig_id.empty(), chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': signature provider reference must not be empty", option_name_client,
                 id);
      SYS_ASSERT(!url.empty(), chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': RPC URL must not be empty", option_name_client, id);
      SYS_ASSERT(client_ids.emplace(id).second, chain::plugin_config_exception, "Duplicate {} client id '{}'",
                 option_name_client, id);
      SYS_ASSERT(sig_mgr.is_explicitly_configured_provider(sig_id), chain::plugin_config_exception,
                 "Outpost Solana client '{}' does not reference an explicitly configured signature provider", id);

      auto sig_provider = sig_mgr.get_provider(sig_id);
      SYS_ASSERT(sig_provider->target_chain == fc::crypto::chain_kind_solana &&
                    sig_provider->key_type == fc::crypto::chain_key_type_solana,
                 chain::plugin_config_exception,
                 "Outpost Solana client '{}' signature provider must use chain=solana and key-type=solana", id);

      fc::url parsed_rpc_url;
      try {
         parsed_rpc_url = fc::url(url);
      } catch (const std::exception&) {
         SYS_ASSERT(false, chain::plugin_config_exception, "Invalid {} spec for client '{}': RPC URL is malformed",
                    option_name_client, id);
      }
      SYS_ASSERT((parsed_rpc_url.proto() == "http" || parsed_rpc_url.proto() == "https") && parsed_rpc_url.host() &&
                    !parsed_rpc_url.host()->empty(),
                 chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': RPC URL must contain an HTTP(S) scheme and host", option_name_client,
                 id);

      parsed_clients.push_back(parsed_client_spec{id, sig_id, url, std::move(parsed_rpc_url)});
   }

   std::map<std::string, solana_genesis_hash> expected_identities;
   const bool pinning_enabled = options.contains(option_name_cluster_identity);
   if (pinning_enabled) {
      const auto identity_specs = options.at(option_name_cluster_identity).as<std::vector<std::string>>();
      for (const auto& identity_spec : identity_specs) {
         auto parts = fc::split(identity_spec, ',');
         SYS_ASSERT(parts.size() == 2, chain::plugin_config_exception,
                    "Invalid {} spec (expected exactly: <client-id>,<expected-genesis-hash>)",
                    option_name_cluster_identity);
         const auto& client_id = parts[0];
         const auto& expected_hash = parts[1];
         SYS_ASSERT(!client_id.empty(), chain::plugin_config_exception,
                    "Invalid {} spec: Solana client id must not be empty", option_name_cluster_identity);
         SYS_ASSERT(!expected_hash.empty(), chain::plugin_config_exception,
                    "Invalid {} spec for client '{}': reason={}", option_name_cluster_identity, client_id,
                    magic_enum::enum_name(solana_cluster_identity_reason::missing_expected_identity));
         SYS_ASSERT(client_ids.contains(client_id), chain::plugin_config_exception,
                    "Invalid {} spec for unknown client '{}'", option_name_cluster_identity, client_id);
         SYS_ASSERT(!expected_identities.contains(client_id), chain::plugin_config_exception,
                    "Duplicate {} spec for client '{}'", option_name_cluster_identity, client_id);
         try {
            expected_identities.emplace(client_id, parse_solana_genesis_hash(expected_hash));
         } catch (const std::exception&) {
            SYS_ASSERT(false, chain::plugin_config_exception, "Invalid {} spec for client '{}': reason={}",
                       option_name_cluster_identity, client_id,
                       magic_enum::enum_name(solana_cluster_identity_reason::malformed_expected_identity));
         }
      }

      for (const auto& client : parsed_clients) {
         SYS_ASSERT(expected_identities.contains(client.id), chain::plugin_config_exception,
                    "Pinned Solana configuration is missing {} for client '{}'", option_name_cluster_identity,
                    client.id);
      }
   }

   const auto maximum_age = fc::milliseconds(options.at(option_name_identity_max_age).as<uint32_t>());
   const auto probe_timeout = fc::milliseconds(options.at(option_name_identity_timeout).as<uint32_t>());
   SYS_ASSERT(maximum_age.count() > 0, chain::plugin_config_exception, "--{} must be greater than zero",
              option_name_identity_max_age);
   SYS_ASSERT(probe_timeout.count() > 0, chain::plugin_config_exception, "--{} must be greater than zero",
              option_name_identity_timeout);

   // Construct every client before publishing any of them. If a pinned startup
   // probe fails, the plugin exits with an empty registry rather than exposing
   // the clients that happened to validate first.
   std::vector<solana_client_entry_ptr> pending_clients;
   pending_clients.reserve(parsed_clients.size());
   for (const auto& client : parsed_clients) {
      auto sig_provider = sig_mgr.get_provider(client.signature_provider_id);
      solana_client_ptr rpc_client;
      if (pinning_enabled) {
         rpc_client = std::make_shared<solana_client>(sig_provider, client.parsed_rpc_url,
                                                      solana_cluster_identity_config{
                                                         .client_id = client.id,
                                                         .expected_genesis_hash = expected_identities.at(client.id),
                                                         .maximum_verification_age = maximum_age,
                                                         .probe_timeout = probe_timeout,
                                                      },
                                                      rpc_options);
      } else {
         rpc_client = std::make_shared<solana_client>(sig_provider, client.parsed_rpc_url, rpc_options);
         wlog("SEC-139 staged rollout: Solana client '{}' is running without cluster identity "
              "pinning (reason={}). Supply --{} for every configured Solana client to enable "
              "strict pinned mode.",
              client.id, magic_enum::enum_name(solana_cluster_identity_reason::missing_expected_identity),
              option_name_cluster_identity);
      }
      pending_clients.push_back(
         std::make_shared<solana_client_entry_t>(client.id, client.rpc_url, sig_provider, std::move(rpc_client)));
   }

   for (std::size_t i = 0; i < pending_clients.size(); ++i) {
      auto& entry = pending_clients[i];
      const auto client_id = entry->id;
      my->add_client(client_id, std::move(entry));
      ilog("Added Solana client (id={},endpoint={},cluster_identity_mode={})", client_id,
           fc::http::sanitized_endpoint(parsed_clients[i].parsed_rpc_url),
           pinning_enabled ? magic_enum::enum_name(solana_cluster_identity_mode::pinned)
                           : magic_enum::enum_name(solana_cluster_identity_mode::unpinned));
   }
}

void outpost_solana_client_plugin::plugin_startup() {
   ilog("Starting outpost solana client plugin");
}

void outpost_solana_client_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()(option_name_client, boost::program_options::value<std::vector<std::string>>()->multitoken(),
                     "Outpost Solana Client spec, the plugin supports 1 to many clients in a given process. "
                     "Format: `<sol-client-id>,<sig-provider-id>,<rpc-url>`. The signer id must "
                     "match an explicitly named --signature-provider with the Solana target chain and key type")(
      option_name_cluster_identity, boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Optional staged Solana cluster identity pin. Format: "
      "`<sol-client-id>,<expected-genesis-hash>`. When this option is present, "
      "every configured Solana client must have exactly one canonical 32-byte "
      "base58 genesis hash; when absent, legacy unpinned behavior is preserved "
      "with a prominent security warning")(
      option_name_identity_max_age,
      boost::program_options::value<uint32_t>()->default_value(default_identity_max_age_ms),
      "Maximum successful Solana cluster identity age in milliseconds used for freshness telemetry; protected RPCs "
      "still preflight every call")(
      option_name_identity_timeout,
      boost::program_options::value<uint32_t>()->default_value(default_identity_probe_timeout_ms),
      "Maximum duration in milliseconds of each Solana cluster identity probe")(
      option_idl_file, boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
      "Solana program IDL file(s). Expects each file to be a JSON IDL (Anchor format) program definition.")(
      option_outpost_program_name,
      boost::program_options::value<std::string>()->default_value(OPP_SOLANA_OUTPOST_PROGRAM_NAME),
      "Anchor IDL program name of the Solana OPP outpost. The loaded --solana-idl-file set is filtered to "
      "programs with this name when constructing outpost clients. The default targets the standalone "
      "opp_outpost program; pass liqsol_core when the outpost interface is hosted inside the liqsol-core "
      "program (clean-room layout).");
   outbound_http::add_transport_program_options(
      cfg,
      transport_option_names,
      "Solana RPC");
}

void outpost_solana_client_plugin::plugin_shutdown() {
   ilog("Shutdown outpost solana client plugin");
}

std::vector<solana_client_entry_ptr> outpost_solana_client_plugin::get_clients() {
   return my->get_clients();
}

std::vector<solana_cluster_identity_snapshot> outpost_solana_client_plugin::get_cluster_identity_snapshots() {
   return my->get_cluster_identity_snapshots();
}

solana_client_entry_ptr outpost_solana_client_plugin::get_client(const std::string& id) {
   return my->get_client(id);
}

const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>&
outpost_solana_client_plugin::get_idl_files() {
   return my->get_idl_files();
}

std::shared_ptr<outpost_client> outpost_solana_client_plugin::create_outpost_client(const std::string& sol_client_id,
                                                                                    uint64_t chain_code,
                                                                                    uint32_t chain_id,
                                                                                    const std::string& program_id,
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
             my->outpost_program_name(), option_outpost_program_name);

   return std::make_shared<outpost_solana_client>(entry, program_key, std::move(program_idls), chain_code, chain_id,
                                                  role);
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
