#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <ranges>
#include <set>
#include <utility>

namespace sysio {
// using namespace outpost_client::ethereum;

namespace {
constexpr auto option_name_client                    = "outpost-ethereum-client";
constexpr auto option_name_client_configuration_file = "outpost-ethereum-client-config-file";
constexpr auto option_abi_file                        = "ethereum-abi-file";
constexpr int64_t legacy_chain_id_resolution_timeout_seconds = 5;

constexpr auto root_field_version = "version";
constexpr auto root_field_clients = "clients";
constexpr auto policy_field_client_id                    = "client_id";
constexpr auto client_field_signature_provider_id        = "signature_provider_id";
constexpr auto client_field_rpc_url                      = "rpc_url";
constexpr auto policy_field_chain_id                     = "chain_id";
constexpr auto client_field_transaction_policy           = "transaction_policy";
constexpr auto policy_field_max_priority_fee_per_gas_wei = "max_priority_fee_per_gas_wei";
constexpr auto policy_field_max_fee_per_gas_wei          = "max_fee_per_gas_wei";
constexpr auto policy_field_max_gas_limit                = "max_gas_limit";
constexpr auto policy_field_max_total_native_cost_wei    = "max_total_native_cost_wei";
constexpr uint64_t transaction_policy_schema_version     = 1;

constexpr std::array<std::string_view, 2> client_configuration_root_fields{
   root_field_version,
   root_field_clients,
};
constexpr std::array<std::string_view, 4> required_client_configuration_fields{
   policy_field_client_id,
   client_field_signature_provider_id,
   client_field_rpc_url,
   policy_field_chain_id,
};
constexpr std::array<std::string_view, 5> client_configuration_fields_with_policy{
   policy_field_client_id,
   client_field_signature_provider_id,
   client_field_rpc_url,
   policy_field_chain_id,
   client_field_transaction_policy,
};
constexpr std::array<std::string_view, 4> transaction_policy_fields{
   policy_field_max_priority_fee_per_gas_wei,
   policy_field_max_fee_per_gas_wei,
   policy_field_max_gas_limit,
   policy_field_max_total_native_cost_wei,
};

/** Parsed non-secret fields of one `--outpost-ethereum-client` specification. */
struct ethereum_client_spec {
   std::string             id;
   std::string             signature_provider_id;
   std::string             url;
   std::optional<uint32_t> chain_id;
};

/** Require an object to contain each expected field exactly once and no unknown fields. */
template <size_t FieldCount>
void validate_exact_fields(const fc::variant_object&                    object,
                           const std::array<std::string_view, FieldCount>& expected,
                           std::string_view                               context) {
   std::set<std::string> observed_fields;
   for (const auto& entry : object) {
      const bool known = std::ranges::find(expected, entry.key()) != expected.end();
      if (!known) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_schema_invalid,
            context,
            "<unknown-field>");
      }
      if (!observed_fields.insert(entry.key()).second) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_schema_invalid,
            context,
            "<duplicate-field>");
      }
   }

   for (const auto field : expected) {
      if (!observed_fields.contains(std::string(field))) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_schema_invalid,
            context,
            "missing=" + std::string(field));
      }
   }
}

/** Read a required JSON string without coercing another JSON type. */
std::string require_string_field(const fc::variant_object& object, std::string_view field) {
   const auto& value = object[std::string(field)];
   if (!value.is_string()) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_schema_invalid, field, "<non-string>");
   }
   return value.as_string();
}

/** Parse an externally registered chain id without truncation, then enforce its uint32 domain. */
[[nodiscard]] uint32_t require_external_chain_id(std::string_view value, std::string_view field) {
   const fc::uint256 chain_id = parse_canonical_uint256_decimal(value, field);
   constexpr auto max_external_chain_id = std::numeric_limits<uint32_t>::max();
   if (chain_id > max_external_chain_id) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_value_invalid,
                                         field,
                                         chain_id.str(),
                                         std::to_string(max_external_chain_id));
   }
   return chain_id.convert_to<uint32_t>();
}

/** Load a strict versioned JSON object without exposing file contents in diagnostics. */
template <size_t FieldCount>
fc::variant_object load_versioned_configuration_file(
   const std::filesystem::path&                    configuration_file,
   std::string_view                                option_name,
   const std::array<std::string_view, FieldCount>& expected_root_fields) {
   std::ifstream readable_configuration_file{configuration_file};
   if (!readable_configuration_file.is_open()) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_file_unreadable,
         option_name,
         "<unreadable>");
   }

   fc::variant document;
   try {
      document = fc::json::from_file(configuration_file, fc::json::parse_type::strict_parser);
   } catch (const fc::exception&) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_schema_invalid,
         option_name,
         "<invalid-json>");
   } catch (const std::exception&) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_schema_invalid,
         option_name,
         "<invalid-json>");
   }

   if (!document.is_object()) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_schema_invalid, "root", "<non-object>");
   }
   const auto root = document.get_object();
   validate_exact_fields(root, expected_root_fields, "root");

   const auto& version = root[root_field_version];
   // fc's strict parser stores non-negative JSON integers in the uint64 alternative.
   const bool supported_version =
      version.is_uint64() && version.as_uint64() == transaction_policy_schema_version;
   if (!supported_version) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_version_unsupported,
         root_field_version,
         "<unsupported>",
         std::to_string(transaction_policy_schema_version));
   }
   return root;
}

/** Parse the four finite policy limits after their enclosing object is schema-validated. */
ethereum_transaction_policy parse_transaction_policy(const fc::variant_object& policy_object,
                                                      std::string              client_id,
                                                      uint32_t                 chain_id) {
   ethereum_transaction_policy policy{
      .client_id = std::move(client_id),
      .chain_id = chain_id,
      .max_priority_fee_per_gas = parse_canonical_uint256_decimal(
         require_string_field(policy_object, policy_field_max_priority_fee_per_gas_wei),
         policy_field_max_priority_fee_per_gas_wei),
      .max_fee_per_gas = parse_canonical_uint256_decimal(
         require_string_field(policy_object, policy_field_max_fee_per_gas_wei),
         policy_field_max_fee_per_gas_wei),
      .max_gas_limit = parse_canonical_uint256_decimal(
         require_string_field(policy_object, policy_field_max_gas_limit), policy_field_max_gas_limit),
      .max_total_native_cost = parse_canonical_uint256_decimal(
         require_string_field(policy_object, policy_field_max_total_native_cost_wei),
         policy_field_max_total_native_cost_wei),
   };
   validate_transaction_policy_configuration(policy);
   return policy;
}

/** Parse a client specification while keeping its credential-bearing URL out of diagnostics. */
ethereum_client_spec parse_client_spec(const std::string& encoded_spec) {
   auto parts = fc::split(encoded_spec, ',');
   if (parts.size() != 3 && parts.size() != 4) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_schema_invalid,
                                         option_name_client,
                                         "field_count=" + std::to_string(parts.size()),
                                         "3 or 4");
   }
   if (parts[0].empty() || parts[1].empty() || parts[2].empty()) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_value_invalid,
         option_name_client,
         "<empty-required-field>");
   }
   if (!is_safe_transaction_policy_identifier(parts[0])) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_value_invalid,
                                         policy_field_client_id,
                                         "<invalid>");
   }

   ethereum_client_spec result{
      .id = parts[0],
      .signature_provider_id = parts[1],
      .url = parts[2],
   };
   if (parts.size() == 4) {
      result.chain_id = require_external_chain_id(parts[3], "client_spec.chain_id");
   }
   return result;
}

/** Return this plugin's logger for appbase log macros. */
[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_ethereum_client_plugin"};
   return log;
}
}

ethereum_transaction_policy make_default_ethereum_transaction_policy(std::string client_id,
                                                                      uint32_t    chain_id) {
   const auto& maximum = maximum_ethereum_transaction_policy_value();
   ethereum_transaction_policy policy{
      .client_id = std::move(client_id),
      .chain_id = chain_id,
      .max_priority_fee_per_gas = maximum,
      .max_fee_per_gas = maximum,
      .max_gas_limit = maximum,
      .max_total_native_cost = maximum,
   };
   validate_transaction_policy_configuration(policy);
   return policy;
}

ethereum_client_configuration_map
load_ethereum_client_configuration_file(const std::filesystem::path& configuration_file) {
   const auto root = load_versioned_configuration_file(
      configuration_file, option_name_client_configuration_file, client_configuration_root_fields);

   const auto& clients_value = root[root_field_clients];
   if (!clients_value.is_array()) {
      throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_schema_invalid,
                                         root_field_clients,
                                         "<non-array>");
   }

   ethereum_client_configuration_map result;
   for (const auto& client_value : clients_value.get_array()) {
      if (!client_value.is_object()) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_schema_invalid,
            "client",
            "<non-object>");
      }
      const auto client_object = client_value.get_object();
      const bool has_explicit_policy = client_object.contains(client_field_transaction_policy);
      if (has_explicit_policy) {
         validate_exact_fields(client_object, client_configuration_fields_with_policy, "client");
      } else {
         validate_exact_fields(client_object, required_client_configuration_fields, "client");
      }

      const auto client_id = require_string_field(client_object, policy_field_client_id);
      if (!is_safe_transaction_policy_identifier(client_id)) {
         throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_value_invalid,
                                            policy_field_client_id,
                                            "<invalid>");
      }
      const auto signature_provider_id =
         require_string_field(client_object, client_field_signature_provider_id);
      if (signature_provider_id.empty()) {
         throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_value_invalid,
                                            client_field_signature_provider_id,
                                            "<empty>");
      }
      const auto rpc_url = require_string_field(client_object, client_field_rpc_url);
      if (rpc_url.empty()) {
         throw_transaction_policy_exception(ethereum_transaction_policy_reason::configuration_value_invalid,
                                            client_field_rpc_url,
                                            "<empty>");
      }
      const auto chain_id = require_external_chain_id(
         require_string_field(client_object, policy_field_chain_id), policy_field_chain_id);

      auto policy = make_default_ethereum_transaction_policy(client_id, chain_id);
      if (has_explicit_policy) {
         const auto& policy_value = client_object[client_field_transaction_policy];
         if (!policy_value.is_object()) {
            throw_transaction_policy_exception(
               ethereum_transaction_policy_reason::configuration_schema_invalid,
               client_field_transaction_policy,
               "<non-object>");
         }
         const auto policy_object = policy_value.get_object();
         validate_exact_fields(policy_object, transaction_policy_fields, client_field_transaction_policy);
         policy = parse_transaction_policy(policy_object, client_id, chain_id);
      }

      ethereum_client_configuration configuration{
         .id = client_id,
         .signature_provider_id = signature_provider_id,
         .url = rpc_url,
         .chain_id = chain_id,
         .policy = std::move(policy),
      };
      if (!result.emplace(client_id, std::move(configuration)).second) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_client_duplicate,
            policy_field_client_id,
            client_id);
      }
   }
   return result;
}

namespace {

/** Resolve the legacy three-field client form exactly as it behaved before local policy binding. */
uint32_t resolve_legacy_chain_id(const ethereum_client_spec& spec) {
   if (spec.chain_id) return *spec.chain_id;

   try {
      auto rpc_client = fc::network::json_rpc::json_rpc_client::create(spec.url);
      const auto chain_id = parse_rpc_quantity(rpc_client.call("eth_chainId", fc::variants{}), "eth_chainId");
      constexpr auto max_external_chain_id = std::numeric_limits<uint32_t>::max();
      if (chain_id == 0 || chain_id > max_external_chain_id) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_value_invalid,
            "eth_chainId",
            chain_id.str(),
            "1.." + std::to_string(max_external_chain_id));
      }
      return chain_id.convert_to<uint32_t>();
   } catch (const ethereum_transaction_policy_exception&) {
      throw;
   } catch (const fc::exception&) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_schema_invalid,
         "client_spec.url",
         "<invalid-or-unavailable>");
   } catch (const std::exception&) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_schema_invalid,
         "client_spec.url",
         "<invalid-or-unavailable>");
   }
}

/** Convert backward-compatible command-line client specs to unified internal configurations. */
ethereum_client_configuration_map
load_legacy_client_configurations(const std::vector<std::string>& encoded_client_specs) {
   if (encoded_client_specs.empty()) {
      throw_transaction_policy_exception(
         ethereum_transaction_policy_reason::configuration_client_missing,
         option_name_client,
         "<empty>");
   }

   ethereum_client_configuration_map result;
   for (const auto& encoded_spec : encoded_client_specs) {
      auto spec = parse_client_spec(encoded_spec);
      const auto chain_id = resolve_legacy_chain_id(spec);
      ethereum_client_configuration configuration{
         .id = spec.id,
         .signature_provider_id = spec.signature_provider_id,
         .url = spec.url,
         .chain_id = chain_id,
         .policy = make_default_ethereum_transaction_policy(spec.id, chain_id),
      };
      if (!result.emplace(spec.id, std::move(configuration)).second) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_client_duplicate,
            policy_field_client_id,
            spec.id);
      }
   }
   return result;
}

} // namespace

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

   ethereum_client_entry_ptr get_client_by_chain_id(uint64_t chain_id) {
      ethereum_client_entry_ptr match;
      for (auto& [id, entry] : _clients) {
         if (entry->chain_id == chain_id) {
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
   try {
      if (options.contains(option_abi_file)) {
         const auto abi_files = options.at(option_abi_file).as<std::vector<std::filesystem::path>>();
         my->load_abi_files(abi_files);
      }
      const bool has_legacy_clients = options.contains(option_name_client);
      const bool has_configuration_file = options.contains(option_name_client_configuration_file);
      if (has_legacy_clients && has_configuration_file) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_schema_invalid,
            option_name_client_configuration_file,
            "<conflicts-with-outpost-ethereum-client>");
      }
      if (!has_legacy_clients && !has_configuration_file) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_client_missing,
            option_name_client,
            "<missing>");
      }

      // The legacy path resolves each omitted chain id and then constructs the permanent clients,
      // whose JSON-RPC transports may resolve DNS again. Keep one aggregate deadline alive across
      // both phases so no legacy endpoint can stall plugin initialization indefinitely.
      std::optional<fc::task::deadline_scope> legacy_chain_id_deadline;
      if (!has_configuration_file) {
         legacy_chain_id_deadline.emplace(
            fc::time_point::now() + fc::seconds(legacy_chain_id_resolution_timeout_seconds));
      }

      ethereum_client_configuration_map configurations;
      if (has_configuration_file) {
         const auto configuration_file =
            options.at(option_name_client_configuration_file).as<std::filesystem::path>();
         configurations = load_ethereum_client_configuration_file(configuration_file);
      } else {
         configurations = load_legacy_client_configurations(
            options.at(option_name_client).as<std::vector<std::string>>());
      }
      if (configurations.empty()) {
         throw_transaction_policy_exception(
            ethereum_transaction_policy_reason::configuration_client_missing,
            has_configuration_file ? root_field_clients : option_name_client,
            "<empty>");
      }

      // This required plugin has already initialized every configured provider, independent of `--plugin` ordering.
      auto& signature_manager = app().get_plugin<signature_provider_manager_plugin>();
      for (auto& [client_id, configuration] : configurations) {
         if (!signature_manager.has_provider(configuration.signature_provider_id)) {
            throw_transaction_policy_exception(
               ethereum_transaction_policy_reason::configuration_schema_invalid,
               "signature_provider",
               "<unavailable>");
         }
         const auto signature_provider =
            signature_manager.get_provider(configuration.signature_provider_id);
         ethereum_client_ptr client;
         try {
            client = std::make_shared<ethereum_client>(
               signature_provider, configuration.url, configuration.policy);
         } catch (const ethereum_transaction_policy_exception&) {
            throw;
         } catch (const std::exception&) {
            throw_transaction_policy_exception(
               ethereum_transaction_policy_reason::configuration_schema_invalid,
               "client_spec.url",
               "<invalid>");
         }
         my->add_client(client_id,
                        std::make_shared<ethereum_client_entry_t>(
                           client_id,
                           signature_provider,
                           std::move(client),
                           configuration.chain_id));

         ilog("Added policy-constrained ethereum client client_id={} chain_id={}",
              client_id,
              configuration.chain_id);
      }
   } catch (const ethereum_transaction_policy_exception& rejection) {
      elog("Ethereum transaction policy configuration rejected reason_code={} field={} observed={} allowed={}",
           reason_code_name(rejection.reason()),
           rejection.field(),
           rejection.observed(),
           rejection.allowed().value_or("n/a"));
      throw;
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
      "Backward-compatible Ethereum client spec. Each client receives the maximum-value default transaction "
      "policy: `<eth-client-id>,<sig-provider-id>,<eth-node-url>[,<eth-chain-id>]`")(
      option_name_client_configuration_file,
      boost::program_options::value<std::filesystem::path>(),
      "Versioned JSON file containing Ethereum clients and optional per-client transaction policies. "
      "Cannot be combined with --outpost-ethereum-client")(
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
   if (entry->chain_id != chain_id) {
      ethereum_transaction_policy_exception rejection{
         ethereum_transaction_policy_reason::configuration_chain_id_mismatch,
         "outpost.chain_id",
         std::to_string(chain_id),
         std::to_string(entry->chain_id),
      };
      elog("Ethereum transaction policy client binding rejected reason_code={} client_id={} field={} "
           "observed={} allowed={}",
           reason_code_name(rejection.reason()),
           entry->id,
           rejection.field(),
           rejection.observed(),
           rejection.allowed().value_or("n/a"));
      throw rejection;
   }

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
