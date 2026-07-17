#include <ranges>
#include <fc/log/logger.hpp>

#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

namespace sysio {

namespace {
constexpr auto option_name_client          = "outpost-solana-client";
constexpr auto option_idl_file             = "solana-idl-file";
constexpr auto option_outpost_program_name = "solana-outpost-program-name";

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

   const std::string& outpost_program_name() const {
      return _outpost_program_name;
   }

   std::vector<file_idl_programs_t> load_idl_files(const std::vector<std::filesystem::path>& file_names) {
      static std::mutex mutex;
      std::scoped_lock lock(mutex);

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
      std::string id;
      std::string sig_id;
      std::string url;
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
         add_client(pending.id, std::make_shared<solana_client_entry_t>(
                                   pending.id, pending.url, sig_provider,
                                   std::make_shared<solana_client>(sig_provider, pending.url)));
         ilog("Added solana client (id={},sig_id={},url={})", pending.id, pending.sig_id, pending.url);
      }
      _pending_clients.clear();
   }
};

outpost_solana_client_plugin::outpost_solana_client_plugin()
   : my(std::make_unique<outpost_solana_client_plugin_impl>()) {}

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

   auto client_specs  = options.at(option_name_client).as<std::vector<std::string>>();

   for (auto& client_spec : client_specs) {
      dlog("Queueing solana client with spec: {}", client_spec);
      auto parts = fc::split(client_spec, ',');
      FC_ASSERT(parts.size() == 3, "Invalid spec {} (expected: <client-id>,<sig-provider-id>,<rpc-url>)",
                client_spec);

      // Pure-config validation only at initialize; the signature provider is
      // resolved -- and the client constructed -- at plugin_startup, once the
      // provider plugins have all initialized (see pending_client_t).
      my->add_pending_client({.id = parts[0], .sig_id = parts[1], .url = parts[2]});
   }
}

void outpost_solana_client_plugin::plugin_startup() {
   ilog("Starting outpost solana client plugin");
   my->resolve_pending_clients();
}

void outpost_solana_client_plugin::set_program_options(options_description& cli, options_description& cfg) {
   cfg.add_options()(
      option_name_client,
      boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Outpost Solana Client spec, the plugin supports 1 to many clients in a given process. "
      "Format: `<sol-client-id>,<sig-provider-id>,<rpc-url>`")(
      option_idl_file,
      boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
      "Solana program IDL file(s). Expects each file to be a JSON IDL (Anchor format) program definition.")(
      option_outpost_program_name,
      boost::program_options::value<std::string>()->default_value(OPP_SOLANA_OUTPOST_PROGRAM_NAME),
      "Anchor IDL program name of the Solana OPP outpost. The loaded --solana-idl-file set is filtered to "
      "programs with this name when constructing outpost clients. The default targets the standalone "
      "opp_outpost program; pass liqsol_core when the outpost interface is hosted inside the liqsol-core "
      "program (clean-room layout).");
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
outpost_solana_client_plugin::create_outpost_client(const std::string& sol_client_id,
                                                  uint64_t           chain_code,
                                                  uint32_t           chain_id,
                                                  const std::string& program_id) {
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
      entry, program_key, std::move(program_idls), chain_code, chain_id);
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
