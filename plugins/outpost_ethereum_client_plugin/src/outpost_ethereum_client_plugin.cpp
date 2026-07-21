#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>

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
constexpr auto option_name_client                  = "outpost-ethereum-client";
constexpr auto option_name_transaction_policy_file = "outpost-ethereum-transaction-policy-file";
constexpr auto option_abi_file                      = "ethereum-abi-file";

constexpr auto root_field_version  = "version";
constexpr auto root_field_policies = "policies";
constexpr auto policy_field_client_id                    = "client_id";
constexpr auto policy_field_chain_id                     = "chain_id";
constexpr auto policy_field_max_priority_fee_per_gas_wei = "max_priority_fee_per_gas_wei";
constexpr auto policy_field_max_fee_per_gas_wei          = "max_fee_per_gas_wei";
constexpr auto policy_field_max_gas_limit                = "max_gas_limit";
constexpr auto policy_field_max_total_native_cost_wei    = "max_total_native_cost_wei";
constexpr uint64_t transaction_policy_schema_version     = 1;

constexpr std::array<std::string_view, 2> root_fields{
   root_field_version,
   root_field_policies,
};
constexpr std::array<std::string_view, 6> policy_fields{
   policy_field_client_id,
   policy_field_chain_id,
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

/** Throw a structured, sanitized plugin-configuration rejection. */
[[noreturn]] void reject_configuration(ethereum_transaction_policy_reason reason,
                                       std::string_view                    field,
                                       std::string                         observed,
                                       std::optional<std::string>          allowed = std::nullopt) {
   throw ethereum_transaction_policy_exception(
      reason, std::string(field), std::move(observed), std::move(allowed));
}

/** Require an object to contain each expected field exactly once and no unknown fields. */
template <size_t FieldCount>
void validate_exact_fields(const fc::variant_object&                    object,
                           const std::array<std::string_view, FieldCount>& expected,
                           std::string_view                               context) {
   std::set<std::string> observed_fields;
   for (const auto& entry : object) {
      const bool known = std::ranges::find(expected, entry.key()) != expected.end();
      if (!known) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                              context,
                              "<unknown-field>");
      }
      if (!observed_fields.insert(entry.key()).second) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                              context,
                              "<duplicate-field>");
      }
   }

   for (const auto field : expected) {
      if (!observed_fields.contains(std::string(field))) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                              context,
                              "missing=" + std::string(field));
      }
   }
}

/** Read a required JSON string without coercing another JSON type. */
std::string require_string_field(const fc::variant_object& object, std::string_view field) {
   const auto& value = object[std::string(field)];
   if (!value.is_string()) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                           field,
                           "<non-string>");
   }
   return value.as_string();
}

/** Parse an externally registered chain id without truncation, then enforce its uint32 domain. */
uint32_t require_external_chain_id(std::string_view value, std::string_view field) {
   const fc::uint256 chain_id = parse_canonical_uint256_decimal(value, field);
   constexpr auto max_external_chain_id = std::numeric_limits<uint32_t>::max();
   if (chain_id > max_external_chain_id) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_value_invalid,
                           field,
                           chain_id.str(),
                           std::to_string(max_external_chain_id));
   }
   return chain_id.convert_to<uint32_t>();
}

/** Parse a client specification while keeping its credential-bearing URL out of diagnostics. */
ethereum_client_spec parse_client_spec(const std::string& encoded_spec) {
   auto parts = fc::split(encoded_spec, ',');
   if (parts.size() != 3 && parts.size() != 4) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                           option_name_client,
                           "field_count=" + std::to_string(parts.size()),
                           "3 or 4");
   }
   if (parts[0].empty() || parts[1].empty() || parts[2].empty()) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_value_invalid,
                           option_name_client,
                           "<empty-required-field>");
   }
   if (!is_safe_transaction_policy_identifier(parts[0])) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_value_invalid,
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

ethereum_transaction_policy_map
load_ethereum_transaction_policy_file(const std::filesystem::path& policy_file) {
   std::ifstream readable_policy_file{policy_file};
   if (!readable_policy_file.is_open()) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_file_unreadable,
                           option_name_transaction_policy_file,
                           "<unreadable>");
   }

   fc::variant document;
   try {
      document = fc::json::from_file(policy_file, fc::json::parse_type::strict_parser);
   } catch (const fc::exception&) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                           option_name_transaction_policy_file,
                           "<invalid-json>");
   } catch (const std::exception&) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                           option_name_transaction_policy_file,
                           "<invalid-json>");
   }

   if (!document.is_object()) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                           "root",
                           "<non-object>");
   }
   const auto root = document.get_object();
   validate_exact_fields(root, root_fields, "root");

   const auto& version = root[root_field_version];
   const bool supported_version =
      (version.is_uint64() && version.as_uint64() == transaction_policy_schema_version) ||
      (version.is_int64() && version.as_int64() == transaction_policy_schema_version);
   if (!supported_version) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_version_unsupported,
                           root_field_version,
                           "<unsupported>",
                           std::to_string(transaction_policy_schema_version));
   }

   const auto& policies_value = root[root_field_policies];
   if (!policies_value.is_array()) {
      reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                           root_field_policies,
                           "<non-array>");
   }

   ethereum_transaction_policy_map result;
   for (const auto& policy_value : policies_value.get_array()) {
      if (!policy_value.is_object()) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                              "policy",
                              "<non-object>");
      }
      const auto policy_object = policy_value.get_object();
      validate_exact_fields(policy_object, policy_fields, "policy");

      const auto client_id = require_string_field(policy_object, policy_field_client_id);
      ethereum_transaction_policy policy{
         .client_id = client_id,
         .chain_id = parse_canonical_uint256_decimal(
            require_string_field(policy_object, policy_field_chain_id), policy_field_chain_id),
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
      require_external_chain_id(policy.chain_id.str(), policy_field_chain_id);

      if (!result.emplace(client_id, std::move(policy)).second) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_client_duplicate,
                              policy_field_client_id,
                              client_id);
      }
   }
   return result;
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
      if (!options.contains(option_name_client)) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_client_missing,
                              option_name_client,
                              "<missing>");
      }
      if (!options.contains(option_name_transaction_policy_file)) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_file_unreadable,
                              option_name_transaction_policy_file,
                              "<missing>");
      }

      const auto policy_file = options.at(option_name_transaction_policy_file).as<std::filesystem::path>();
      auto policies = load_ethereum_transaction_policy_file(policy_file);

      // This required plugin has already initialized every configured provider, independent of `--plugin` ordering.
      auto& signature_manager = app().get_plugin<signature_provider_manager_plugin>();
      const auto encoded_client_specs = options.at(option_name_client).as<std::vector<std::string>>();
      if (encoded_client_specs.empty()) {
         reject_configuration(ethereum_transaction_policy_reason::configuration_client_missing,
                              option_name_client,
                              "<empty>");
      }

      std::vector<ethereum_client_spec> client_specs;
      std::set<std::string>             configured_client_ids;
      client_specs.reserve(encoded_client_specs.size());
      for (const auto& encoded_spec : encoded_client_specs) {
         auto spec = parse_client_spec(encoded_spec);
         if (!configured_client_ids.insert(spec.id).second) {
            reject_configuration(ethereum_transaction_policy_reason::configuration_client_duplicate,
                                 policy_field_client_id,
                                 spec.id);
         }
         client_specs.emplace_back(std::move(spec));
      }

      for (const auto& [client_id, policy] : policies) {
         if (!configured_client_ids.contains(client_id)) {
            reject_configuration(ethereum_transaction_policy_reason::configuration_client_unknown,
                                 policy_field_client_id,
                                 client_id);
         }
      }

      for (const auto& spec : client_specs) {
         const auto policy_iterator = policies.find(spec.id);
         if (policy_iterator == policies.end()) {
            reject_configuration(ethereum_transaction_policy_reason::configuration_client_missing,
                                 policy_field_client_id,
                                 spec.id);
         }
         const auto& policy = policy_iterator->second;
         if (spec.chain_id && fc::uint256{*spec.chain_id} != policy.chain_id) {
            reject_configuration(ethereum_transaction_policy_reason::configuration_chain_id_mismatch,
                                 "client_spec.chain_id",
                                 std::to_string(*spec.chain_id),
                                 policy.chain_id.str());
         }

         if (!signature_manager.has_provider(spec.signature_provider_id)) {
            reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                                 "signature_provider",
                                 "<unavailable>");
         }
         const auto signature_provider = signature_manager.get_provider(spec.signature_provider_id);
         const auto chain_id = policy.chain_id.convert_to<uint32_t>();
         ethereum_client_ptr client;
         try {
            client = std::make_shared<ethereum_client>(signature_provider, spec.url, policy);
         } catch (const ethereum_transaction_policy_exception&) {
            throw;
         } catch (const std::exception&) {
            reject_configuration(ethereum_transaction_policy_reason::configuration_schema_invalid,
                                 "client_spec.url",
                                 "<invalid>");
         }
         my->add_client(spec.id,
                        std::make_shared<ethereum_client_entry_t>(
                           spec.id,
                           signature_provider,
                           std::move(client),
                           chain_id));

         ilog("Added policy-constrained ethereum client client_id={} chain_id={}", spec.id, chain_id);
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
      "Outpost Ethereum Client spec, the plugin supports 1 to many clients in a given process"
      "`<eth-client-id>,<sig-provider-id>,<eth-node-url>[,<eth-chain-id>]`")(
      option_name_transaction_policy_file,
      boost::program_options::value<std::filesystem::path>(),
      "Versioned JSON file containing exactly one finite transaction expenditure policy per configured "
      "Ethereum client id and chain id")(
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
