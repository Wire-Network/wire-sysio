#include <sysio/outpost_ethereum_client_plugin.hpp>

#include <sysio/http_client_plugin/http_client_options.hpp>
#include <sysio/opp/config/client_config_loader.hpp>
#include <sysio/outpost_client/rpc_options.hpp>
#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>

#include <fc/log/logger.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/slug_name.hpp>
#include <fc/task/deadline.hpp>
#include <fc/task/retry.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>

namespace sysio {

namespace {

constexpr auto option_name_client = "outpost-ethereum-client";
constexpr auto option_name_client_config_file = "outpost-ethereum-client-config-file";
constexpr auto option_abi_file = "ethereum-abi-file";
constexpr auto chain_id_resolution_timeout = fc::seconds(5);
constexpr auto chain_id_resolution_initial_backoff = fc::milliseconds(200);
constexpr auto chain_id_resolution_max_backoff = fc::seconds(1);
constexpr std::string_view legacy_transaction_policy_client_id = "legacy-client";
constexpr auto unavailable_policy_limit = "n/a";
constexpr std::string_view chain_id_resolution_operation = "ethereum-client:eth_chainId";
constexpr std::string_view outbound_http_failure_prefix = "Outbound HTTP ";
constexpr std::string_view http_status_detail_prefix = "failed with status ";
constexpr size_t http_status_code_width = 3;
constexpr std::string_view retry_budget_exhausted_cause = "retry_budget_exhausted";
constexpr std::string_view standard_exception_cause = "std_exception";
constexpr std::string_view invalid_rpc_response_cause = "invalid_rpc_response";
constexpr std::string_view fc_exception_cause_prefix = "fc_exception:";
constexpr std::string_view configuration_chain_id_mismatch_reason = "configuration_chain_id_mismatch";

namespace transaction_policy_field {
constexpr std::string_view chain_id = "chain_id";
constexpr std::string_view max_priority_fee_per_gas_wei = "max_priority_fee_per_gas_wei";
constexpr std::string_view max_fee_per_gas_wei = "max_fee_per_gas_wei";
constexpr std::string_view max_gas_limit = "max_gas_limit";
constexpr std::string_view max_total_native_cost_wei = "max_total_native_cost_wei";
} // namespace transaction_policy_field

namespace ethereum_rpc_method {
constexpr std::string_view chain_id = "eth_chainId";
} // namespace ethereum_rpc_method

constexpr outbound_http::transport_option_names transport_option_names{
   .additional_ca_file = "outpost-ethereum-additional-ca-file",
   .additional_ca_path = "outpost-ethereum-additional-ca-path",
   .proxy = "outpost-ethereum-proxy",
};

[[maybe_unused]] inline fc::logger& logger() {
   static fc::logger log{"outpost_ethereum_client_plugin"};
   return log;
}

using client_map = std::map<std::string, ethereum_client_entry_ptr>;
namespace client_config = opp::config;
using client_config::EvmClientConfiguration;
using fc::network::ethereum::ethereum_transaction_policy;

/** Whether startup must verify a configured local chain id against `eth_chainId`. */
enum class rpc_chain_id_validation {
   not_required,
   required,
};

/** Parse a positive decimal or Ethereum hex quantity without fixed-width wraparound. */
std::optional<uint32_t> parse_legacy_chain_id(std::string_view text) {
   if (text.empty()) return std::nullopt;

   uint32_t base = 10;
   size_t offset = 0;
   if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      base = 16;
      offset = 2;
   }
   if (offset == text.size()) return std::nullopt;

   uint32_t value = 0;
   for (; offset < text.size(); ++offset) {
      const char c = text[offset];
      uint32_t digit = 0;
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

/** Construct the compatibility policy used when a client has no explicit expenditure limits. */
ethereum_transaction_policy maximum_policy(std::string client_id, uint32_t chain_id) {
   const auto& maximum = fc::network::ethereum::maximum_ethereum_transaction_policy_value();
   return ethereum_transaction_policy{
      .client_id = std::move(client_id),
      .chain_id = chain_id,
      .max_priority_fee_per_gas = maximum,
      .max_fee_per_gas = maximum,
      .max_gas_limit = maximum,
      .max_total_native_cost = maximum,
   };
}

/** Preserve legacy client names while keeping the new structured policy log label safe. */
std::string transaction_policy_client_label(std::string_view client_id) {
   if (fc::network::ethereum::is_safe_transaction_policy_identifier(client_id)) {
      return std::string(client_id);
   }
   return std::string(legacy_transaction_policy_client_id);
}

/** Convert a validated protobuf client policy to the runtime transaction-policy model. */
ethereum_transaction_policy policy_from_configuration(const EvmClientConfiguration& client) {
   const auto& connection = client.connection();
   if (!client.has_transaction_policy()) {
      return maximum_policy(connection.client_id(), client.chain_id());
   }

   const auto& policy = client.transaction_policy();
   return ethereum_transaction_policy{
      .client_id = connection.client_id(),
      .chain_id = client.chain_id(),
      .max_priority_fee_per_gas = fc::network::ethereum::parse_canonical_uint256_decimal(
         policy.max_priority_fee_per_gas_wei(), transaction_policy_field::max_priority_fee_per_gas_wei),
      .max_fee_per_gas = fc::network::ethereum::parse_canonical_uint256_decimal(
         policy.max_fee_per_gas_wei(), transaction_policy_field::max_fee_per_gas_wei),
      .max_gas_limit = fc::network::ethereum::parse_canonical_uint256_decimal(
         policy.max_gas_limit(), transaction_policy_field::max_gas_limit),
      .max_total_native_cost = fc::network::ethereum::parse_canonical_uint256_decimal(
         policy.max_total_native_cost_wei(), transaction_policy_field::max_total_native_cost_wei),
   };
}

/** Report a sanitized client-construction failure without exposing the endpoint or credentials. */
[[noreturn]] void throw_client_initialization_failure(const std::string& client_id) {
   FC_THROW_EXCEPTION(chain::plugin_config_exception,
                      "Failed to initialize outpost Ethereum client '{}'",
                      client_id);
}

/** Return a bounded transport category without reflecting response bodies or endpoint credentials. */
std::string sanitized_chain_id_failure_cause(const fc::exception& failure) {
   const auto message = failure.top_message();
   for (const auto failure_kind : magic_enum::enum_values<fc::http::failure_kind>()) {
      const auto failure_name = fc::http::failure_kind_name(failure_kind);
      const auto marker = std::string(outbound_http_failure_prefix) + std::string(failure_name) + ":";
      const auto marker_position = message.find(marker);
      if (marker_position == std::string::npos) continue;

      std::string cause(failure_name);
      if (failure_kind == fc::http::failure_kind::http_status) {
         const auto status_position = message.find(
            http_status_detail_prefix, marker_position + marker.size());
         if (status_position != std::string::npos) {
            const auto code_begin = status_position + http_status_detail_prefix.size();
            const auto code_end = std::min(code_begin + http_status_code_width, message.size());
            if (code_end - code_begin == http_status_code_width &&
                std::ranges::all_of(message.substr(code_begin, http_status_code_width),
                                    [](char digit) { return digit >= '0' && digit <= '9'; })) {
               cause += ":" + message.substr(code_begin, http_status_code_width);
            }
         }
      }
      return cause;
   }
   return std::string{fc_exception_cause_prefix} + failure.name();
}

/** Report a startup chain-id lookup failure with only sanitized diagnostic context. */
[[noreturn]] void throw_chain_id_resolution_failure(const std::string& client_id,
                                                    const std::string& endpoint,
                                                    std::string_view last_failure) {
   FC_THROW_EXCEPTION(chain::plugin_config_exception,
                      "Unable to resolve or validate chain id for outpost Ethereum client '{}' "
                      "within the bounded RPC startup grace (endpoint={},last_failure={})",
                      client_id,
                      endpoint,
                      last_failure);
}

/** Return the retry envelope used independently by each startup chain-id probe. */
fc::task::retry_options chain_id_resolution_retry_options() {
   fc::task::retry_options options;
   options.initial_backoff = chain_id_resolution_initial_backoff;
   options.max_backoff = chain_id_resolution_max_backoff;
   options.total_timeout = chain_id_resolution_timeout;
   return options;
}

/** Resolve and bound one RPC chain id after retrying transient transport failures. */
uint32_t resolve_rpc_chain_id(
   const std::string& client_id,
   const std::string& url,
   const fc::network::json_rpc::client_options& rpc_options) {
   const auto endpoint = fc::http::sanitized_endpoint(fc::url(url));
   std::string last_failure(retry_budget_exhausted_cause);
   try {
      fc::task::deadline_scope deadline(fc::time_point::now() + chain_id_resolution_timeout);
      return fc::task::retry_until<uint32_t>(
         chain_id_resolution_operation,
         chain_id_resolution_retry_options(),
         [&]() -> std::optional<uint32_t> {
            fc::variant response;
            try {
               auto rpc = fc::network::json_rpc::json_rpc_client::create(url, rpc_options);
               response = rpc.call_idempotent(
                  std::string(ethereum_rpc_method::chain_id), fc::variants{});
            } catch (const fc::exception& failure) {
               const auto cause = sanitized_chain_id_failure_cause(failure);
               const auto total_timeout_cause =
                  fc::http::failure_kind_name(fc::http::failure_kind::timeout_total);
               if (cause != total_timeout_cause || last_failure == retry_budget_exhausted_cause) {
                  last_failure = cause;
               }
               return std::nullopt;
            } catch (const std::exception&) {
               last_failure = standard_exception_cause;
               return std::nullopt;
            }

            const auto chain_id = fc::network::ethereum::parse_rpc_quantity(
               response, transaction_policy_field::chain_id);
            if (chain_id == 0 || chain_id > std::numeric_limits<uint32_t>::max()) {
               throw_chain_id_resolution_failure(client_id, endpoint, invalid_rpc_response_cause);
            }
            return chain_id.convert_to<uint32_t>();
         });
   } catch (const chain::plugin_config_exception&) {
      throw;
   } catch (const fc::network::ethereum::ethereum_transaction_policy_exception&) {
      throw_chain_id_resolution_failure(client_id, endpoint, invalid_rpc_response_cause);
   } catch (const fc::exception&) {
      throw_chain_id_resolution_failure(client_id, endpoint, last_failure);
   } catch (const std::exception&) {
      throw_chain_id_resolution_failure(client_id, endpoint, last_failure);
   }
}

/** Resolve an explicitly named Ethereum signature provider for one configured client. */
fc::crypto::signature_provider_ptr resolve_signature_provider(
   signature_provider_manager_plugin& signature_provider_manager,
   const std::string& client_id,
   const std::string& signature_provider_id) {
   SYS_ASSERT(signature_provider_manager.is_explicitly_configured_provider(signature_provider_id),
              chain::plugin_config_exception,
              "Outpost Ethereum client '{}' references an unavailable explicitly named signature provider",
              client_id);

   const auto provider = signature_provider_manager.get_provider(signature_provider_id);
   SYS_ASSERT(provider->target_chain == fc::crypto::chain_kind_ethereum &&
                 provider->key_type == fc::crypto::chain_key_type_ethereum,
              chain::plugin_config_exception,
              "Outpost Ethereum client '{}' signature provider must use chain=ethereum and key-type=ethereum",
              client_id);
   return provider;
}

/** Construct a policy-enforcing Ethereum client and sanitize construction failures. */
ethereum_client_ptr create_client(
   const fc::crypto::signature_provider_ptr& signature_provider,
   const std::string& url,
   ethereum_transaction_policy policy,
   const fc::network::json_rpc::client_options& rpc_options) {
   const auto client_id = policy.client_id;
   try {
      return std::make_shared<ethereum_client>(signature_provider, url, std::move(policy), rpc_options);
   } catch (const fc::exception&) {
      throw_client_initialization_failure(client_id);
   } catch (const std::exception&) {
      throw_client_initialization_failure(client_id);
   }
}

/** Add one fully initialized and optionally RPC-validated client before publication. */
void add_client(client_map& clients,
                const std::string& client_id,
                const std::string& url,
                const fc::crypto::signature_provider_ptr& signature_provider,
                ethereum_transaction_policy policy,
                const fc::network::json_rpc::client_options& rpc_options,
                rpc_chain_id_validation chain_id_validation) {
   const auto chain_id = policy.chain_id;
   auto client = create_client(signature_provider, url, std::move(policy), rpc_options);
   if (chain_id_validation == rpc_chain_id_validation::required) {
      const auto remote_chain_id = resolve_rpc_chain_id(client_id, url, rpc_options);
      SYS_ASSERT(remote_chain_id == chain_id,
                 chain::plugin_config_exception,
                 "Chain id mismatch for outpost Ethereum client '{}': configured {}, RPC endpoint reports {}",
                 client_id,
                 chain_id,
                 remote_chain_id);
   }
   const bool inserted = clients.emplace(
      client_id,
      std::make_shared<ethereum_client_entry_t>(
         client_id, signature_provider, std::move(client), chain_id)).second;
   SYS_ASSERT(inserted,
              chain::plugin_config_exception,
              "Duplicate outpost Ethereum client id '{}'",
              client_id);
   ilog("Added Ethereum client (id={},endpoint={},chain_id={})",
        client_id,
        fc::http::sanitized_endpoint(fc::url(url)),
        chain_id);
}

/** Load every protobuf-configured client into a map that is published only on complete success. */
client_map load_file_clients(
   const std::filesystem::path& configuration_file,
   signature_provider_manager_plugin& signature_provider_manager,
   const fc::network::json_rpc::client_options& rpc_options) {
   const auto configuration = client_config::load_evm_client_configuration_file(configuration_file);
   client_map clients;
   for (const auto& configured_client : configuration.clients()) {
      const auto& connection = configured_client.connection();
      auto provider = resolve_signature_provider(
         signature_provider_manager, connection.client_id(), connection.signature_provider_id());
      add_client(clients,
                 connection.client_id(),
                 connection.rpc_url(),
                 provider,
                 policy_from_configuration(configured_client),
                 rpc_options,
                 rpc_chain_id_validation::required);
   }
   return clients;
}

/** Load legacy command-line client specifications with maximum compatibility policies. */
client_map load_legacy_clients(
   const std::vector<std::string>& client_specs,
   signature_provider_manager_plugin& signature_provider_manager,
   const fc::network::json_rpc::client_options& rpc_options) {
   client_map clients;
   for (const auto& client_spec : client_specs) {
      const auto parts = fc::split(client_spec, ',');
      SYS_ASSERT(parts.size() == 3 || parts.size() == 4,
                 chain::plugin_config_exception,
                 "Invalid {} spec (expected: <client-id>,<signature-provider-id>,<rpc-url>[,<chain-id>])",
                 option_name_client);

      const auto& client_id = parts[0];
      const auto& signature_provider_id = parts[1];
      const auto& url = parts[2];
      SYS_ASSERT(!client_id.empty(),
                 chain::plugin_config_exception,
                 "Invalid {} spec: Ethereum client id must not be empty",
                 option_name_client);
      SYS_ASSERT(!signature_provider_id.empty(),
                 chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': signature provider id must not be empty",
                 option_name_client,
                 client_id);
      SYS_ASSERT(!url.empty(),
                 chain::plugin_config_exception,
                 "Invalid {} spec for client '{}': RPC URL must not be empty",
                 option_name_client,
                 client_id);

      uint32_t chain_id = 0;
      if (parts.size() == 4) {
         const auto parsed = parse_legacy_chain_id(parts[3]);
         SYS_ASSERT(parsed,
                    chain::plugin_config_exception,
                    "Invalid {} spec for client '{}': chain id must be a positive 32-bit decimal or hex integer",
                    option_name_client,
                    client_id);
         chain_id = *parsed;
      } else {
         chain_id = resolve_rpc_chain_id(client_id, url, rpc_options);
      }

      auto provider = resolve_signature_provider(
         signature_provider_manager, client_id, signature_provider_id);
      add_client(clients,
                 client_id,
                 url,
                 provider,
                 maximum_policy(transaction_policy_client_label(client_id), chain_id),
                 rpc_options,
                 parts.size() == 4 ? rpc_chain_id_validation::required
                                   : rpc_chain_id_validation::not_required);
   }
   return clients;
}

} // namespace

/** Private plugin state and lookup operations for fully initialized Ethereum clients. */
class outpost_ethereum_client_plugin_impl {
   std::map<std::string, ethereum_client_entry_ptr> _clients{};
   using file_abi_contracts_t =
      std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>;
   std::vector<file_abi_contracts_t> _abi_files{};

public:
   /** Load and de-duplicate ABI files while preserving their parsed contract definitions. */
   std::vector<file_abi_contracts_t>
   load_abi_files(const std::vector<std::filesystem::path>& file_names) {
      for (const auto& filename : file_names) {
         FC_ASSERT_FMT(exists(filename), "File does not exist: {}", filename.string());
         auto file_path = std::filesystem::absolute(filename);
         ilog("Loading ABI file: {}", file_path.string());
         if (!std::ranges::none_of(_abi_files, [&](const auto& file) { return file.first == file_path; })) {
            wlog("Already registered ABI file: {}", file_path.string());
            continue;
         }
         _abi_files.emplace_back(file_path, fc::network::ethereum::abi::parse_contracts(file_path));
      }
      return _abi_files;
   }

   /** Atomically replace the published client map after initialization succeeds. */
   void set_clients(client_map clients) { _clients = std::move(clients); }

   /** Return every published client in deterministic identifier order. */
   std::vector<ethereum_client_entry_ptr> get_clients() {
      return std::views::values(_clients) | std::ranges::to<std::vector>();
   }

   /** Return the published client identified by @p id. */
   ethereum_client_entry_ptr get_client(const std::string& id) { return _clients.at(id); }

   /** Return the unique client for @p chain_id, or null when the id is ambiguous. */
   ethereum_client_entry_ptr get_client_by_chain_id(uint32_t chain_id) {
      ethereum_client_entry_ptr match;
      for (const auto& entry : std::views::values(_clients)) {
         if (entry->chain_id != chain_id) continue;
         if (match) return nullptr;
         match = entry;
      }
      return match;
   }

   /** Return all loaded ABI files and their parsed contracts. */
   const std::vector<file_abi_contracts_t>& get_abi_files() const { return _abi_files; }
};

void outpost_ethereum_client_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_abi_file)) {
      my->load_abi_files(options.at(option_abi_file).as<std::vector<std::filesystem::path>>());
   }

   const bool has_file = options.contains(option_name_client_config_file);
   const bool has_legacy = options.contains(option_name_client);
   SYS_ASSERT(has_file != has_legacy,
              chain::plugin_config_exception,
              "Configure exactly one of --{} or --{}",
              option_name_client_config_file,
              option_name_client);

   auto& signature_provider_manager = app().get_plugin<signature_provider_manager_plugin>();
   const auto rpc_options = outpost_rpc::rpc_options(options, transport_option_names);

   try {
      if (has_file) {
         my->set_clients(load_file_clients(
            options.at(option_name_client_config_file).as<std::filesystem::path>(),
            signature_provider_manager,
            rpc_options));
      } else {
         my->set_clients(load_legacy_clients(
            options.at(option_name_client).as<std::vector<std::string>>(),
            signature_provider_manager,
            rpc_options));
      }
   } catch (const client_config::client_config_exception& rejection) {
      elog("Rejected Ethereum client configuration (reason_code={},field={},observed={},allowed={})",
           client_config::client_config_reason_name(rejection.reason()),
           rejection.field(),
           rejection.observed(),
           rejection.allowed().value_or(unavailable_policy_limit));
      throw;
   } catch (const fc::network::ethereum::ethereum_transaction_policy_exception& rejection) {
      elog("Rejected Ethereum client policy (reason_code={},field={},observed={},allowed={})",
           fc::network::ethereum::reason_code_name(rejection.reason()),
           rejection.field(),
           rejection.observed(),
           rejection.allowed().value_or(unavailable_policy_limit));
      throw;
   }
}

void outpost_ethereum_client_plugin::plugin_startup() {
   ilog("Starting outpost Ethereum client plugin");
}

outpost_ethereum_client_plugin::outpost_ethereum_client_plugin()
   : my(std::make_unique<outpost_ethereum_client_plugin_impl>()) {}

outpost_ethereum_client_plugin::~outpost_ethereum_client_plugin() = default;

void outpost_ethereum_client_plugin::set_program_options(options_description& cli,
                                                         options_description& cfg) {
   cfg.add_options()
      (option_name_client,
       boost::program_options::value<std::vector<std::string>>()->multitoken(),
       "Legacy outpost Ethereum client spec: "
       "<client-id>,<signature-provider-id>,<rpc-url>[,<chain-id>]. A three-field spec resolves "
       "eth_chainId during startup; a four-field chain id is locally authoritative.")
      (option_name_client_config_file,
       boost::program_options::value<std::filesystem::path>(),
       "Versioned protobuf-JSON outpost Ethereum client configuration file. Cannot be combined "
       "with --outpost-ethereum-client.")
      (option_abi_file,
       boost::program_options::value<std::vector<std::filesystem::path>>()->multitoken(),
       "Ethereum contract ABI file(s). Expects a JSON array of ABI-compliant contract definitions.");
   outbound_http::add_transport_program_options(cfg, transport_option_names, "Ethereum RPC");
}

void outpost_ethereum_client_plugin::plugin_shutdown() {
   ilog("Shutdown outpost Ethereum client plugin");
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

const std::vector<std::pair<std::filesystem::path,
                            std::vector<fc::network::ethereum::abi::contract>>>&
outpost_ethereum_client_plugin::get_abi_files() {
   return my->get_abi_files();
}

std::shared_ptr<outpost_client>
outpost_ethereum_client_plugin::create_outpost_client(const std::string& eth_client_id,
                                                       uint64_t chain_code,
                                                       uint32_t chain_id,
                                                       const std::string& opp_addr,
                                                       const std::string& opp_inbound_addr,
                                                       const std::string& operator_registry_addr) {
   const auto entry = my->get_client(eth_client_id);
   FC_ASSERT(entry, "Unknown ethereum client id: {}", eth_client_id);
   const auto chain_name = fc::slug_name{chain_code}.to_string();
   SYS_ASSERT(entry->chain_id == chain_id,
              chain::plugin_config_exception,
              "Outpost Ethereum client configuration rejected "
              "(reason_code={},chain={},client_id={},registry_chain_id={},client_chain_id={})",
              configuration_chain_id_mismatch_reason,
              chain_name,
              eth_client_id,
              chain_id,
              entry->chain_id);

   std::vector<fc::network::ethereum::abi::contract> all_abis;
   for (const auto& [path, contracts] : my->get_abi_files()) {
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
