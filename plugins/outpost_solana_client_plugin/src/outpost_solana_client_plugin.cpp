#include <ranges>
#include <fc/log/logger.hpp>

#include <sysio/outpost_solana_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

namespace sysio {

namespace {
constexpr auto option_name_client = "outpost-solana-client";
constexpr auto option_idl_file    = "solana-idl-file";

[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_solana_client_plugin"};
   return log;
}
} // namespace

class outpost_solana_client_plugin_impl {
   std::map<std::string, solana_client_entry_ptr> _clients{};
   using file_idl_programs_t = std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>;
   std::vector<file_idl_programs_t> _idl_files{};

public:
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
};

outpost_solana_client_plugin::outpost_solana_client_plugin()
   : my(std::make_unique<outpost_solana_client_plugin_impl>()) {}

void outpost_solana_client_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_idl_file)) {
      auto& idl_files = options.at(option_idl_file).as<std::vector<std::filesystem::path>>();
      my->load_idl_files(idl_files);
   }
   FC_ASSERT(options.count(option_name_client),
             "At least one solana client argument is required {}",
             option_name_client);

   auto plug_sig      = app().find_plugin<signature_provider_manager_plugin>();
   auto client_specs  = options.at(option_name_client).as<std::vector<std::string>>();

   for (auto& client_spec : client_specs) {
      dlog("Adding solana client with spec: {}", client_spec);
      auto parts = fc::split(client_spec, ',');
      FC_ASSERT(parts.size() == 3, "Invalid spec {} (expected: <client-id>,<sig-provider-id>,<rpc-url>)",
                client_spec);

      auto& id     = parts[0];
      auto& sig_id = parts[1];
      auto& url    = parts[2];

      auto sig_provider = plug_sig->get_provider(sig_id);
      my->add_client(id,
                     std::make_shared<solana_client_entry_t>(
                        id,
                        url,
                        sig_provider,
                        std::make_shared<solana_client>(sig_provider, url)));

      ilog("Added solana client (id={},sig_id={},url={})", id, sig_id, url);
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
      "Format: `<sol-client-id>,<sig-provider-id>,<rpc-url>`")(
      option_idl_file,
      boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
      "Solana program IDL file(s). Expects each file to be a JSON IDL (Anchor format) program definition.");
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
                                                  uint64_t           outpost_id,
                                                  uint32_t           chain_id,
                                                  const std::string& program_id) {
   auto entry = my->get_client(sol_client_id);
   FC_ASSERT(entry, "Unknown solana client id: {}", sol_client_id);
   FC_ASSERT(!program_id.empty(), "Solana program id is required");

   auto program_key = fc::crypto::solana::solana_public_key::from_base58_string(program_id);

   // Filter the loaded IDL set down to programs whose name matches the OPP
   // outpost program so we don't construct a client around an unrelated IDL.
   std::vector<fc::network::solana::idl::program> program_idls;
   for (auto& [path, programs] : my->get_idl_files()) {
      for (auto& p : programs) {
         if (p.name == OPP_SOLANA_OUTPOST_PROGRAM_NAME) {
            program_idls.push_back(p);
         }
      }
   }
   FC_ASSERT(!program_idls.empty(),
             "IDL for program '{}' not loaded — pass --solana-idl-file",
             OPP_SOLANA_OUTPOST_PROGRAM_NAME);

   return std::make_shared<outpost_solana_client>(
      entry, program_key, std::move(program_idls), outpost_id, chain_id);
}

} // namespace sysio
