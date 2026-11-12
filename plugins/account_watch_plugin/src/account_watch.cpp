#include <fc/crypto/hex.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/variant.hpp>
#include <sysio/account_watch_plugin/account_watch.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <utility>

namespace sysio::account_watch {

namespace bpo = boost::program_options;

namespace {

/// Labels of a rule's fields in parse errors.
constexpr std::string_view actor_label = "actor";
constexpr std::string_view permission_label = "permission";
constexpr std::string_view receiver_label = "receiver";
constexpr std::string_view contract_label = "contract";
constexpr std::string_view action_label = "action";

/// One option value being parsed as a rule, and what its parse errors name: the option, the value's position among
/// the option's values, the value, and the format it should have.
struct rule_spec {
   std::string_view text;        ///< the value
   std::string_view format;      ///< rule_format or receiver_rule_format
   std::string_view option_name; ///< the option the value was given to
   std::size_t position = 0;     ///< 0-based, command-line values first

   /** Throw chain::plugin_config_exception describing @p problem with this value. */
   [[noreturn]] void fail(std::string_view problem) const {
      SYS_THROW(chain::plugin_config_exception, "--{} value {} '{}': {}; format: {}", option_name, position, text,
                problem, format);
   }

   /** The value split at its one rule_part_separator: the authorization or receiver part, then the action part. */
   std::pair<std::string_view, std::string_view> parts() const {
      const std::size_t split = text.find(rule_part_separator);
      if (split == std::string_view::npos || text.find(rule_part_separator, split + 1) != std::string_view::npos)
         fail(fmt::format("expected exactly one '{}'", rule_part_separator));
      return {text.substr(0, split), text.substr(split + 1)};
   }

   /** One field: wildcard, or a valid, non-empty account name. */
   name_pattern name_field(std::string_view field_text, std::string_view label) const {
      if (field_text == wildcard)
         return std::nullopt;
      if (field_text.empty())
         fail(fmt::format("the {} is empty", label));
      try {
         return chain::name{field_text};
      } catch (const chain::name_type_exception& e) {
         fail(fmt::format("the {} is not a valid name: {}", label, e.top_message()));
      }
   }

   /** The action part, `<contract>::<action>`: the contract and the action. */
   std::pair<name_pattern, name_pattern> action_part(std::string_view act) const {
      const std::size_t split = act.find(action_separator);
      if (split == std::string_view::npos)
         fail(fmt::format("the action part needs '{}'", action_separator));
      return {name_field(act.substr(0, split), contract_label),
              name_field(act.substr(split + action_separator.size()), action_label)};
   }
};

/// A field of a rule as the rules' to_string spells it.
std::string pattern_to_string(const name_pattern& pattern) {
   return pattern ? pattern->to_string() : std::string{wildcard};
}

/// True when @p pattern is the wildcard or names @p value.
bool pattern_matches(const name_pattern& pattern, chain::name value) {
   return !pattern || *pattern == value;
}

/// Every value of the repeatable option @p option_name, parsed with @p parse.
template <typename Rule>
std::vector<Rule> parse_rules(const bpo::variables_map& options, const std::string& option_name,
                              Rule (*parse)(std::string_view, std::string_view, std::size_t)) {
   std::vector<Rule> rules;
   if (!options.count(option_name))
      return rules;
   const std::vector<std::string>& specs = options.at(option_name).as<std::vector<std::string>>();
   rules.reserve(specs.size());
   for (std::size_t position = 0; position < specs.size(); ++position)
      rules.push_back(parse(specs[position], option_name, position));
   return rules;
}

/// True when @p act carries an authorization of @p actor other than an explicit payer.
bool has_real_authorization(const chain::action& act, chain::name actor) {
   return std::ranges::any_of(act.authorization, [&](const chain::permission_level& auth) {
      return auth.actor == actor && auth.permission != chain::config::sysio_payer_name;
   });
}

/// @p account's ABI, validated under @p yield; nullopt when the account has none or it is larger than @p max_abi_bytes.
/// Throws when the ABI does not unpack or validation runs past the yield's deadline.
std::optional<chain::abi_serializer> load_abi(const chain::controller& chain, chain::name account,
                                              uint32_t max_abi_bytes,
                                              const chain::abi_serializer::yield_function_t& yield) {
   const chain::account_metadata_object* metadata = chain.find_account_metadata(account);
   if (metadata == nullptr || metadata->abi.size() > max_abi_bytes)
      return std::nullopt;
   chain::abi_def abi;
   if (!chain::abi_serializer::to_abi(metadata->abi, abi))
      return std::nullopt;
   return chain::abi_serializer(std::move(abi), yield);
}

/// Each rule's to_string, for the start record.
template <typename Rule>
fc::variants rules_to_variants(const std::vector<Rule>& rules) {
   fc::variants out;
   out.reserve(rules.size());
   for (const Rule& r : rules)
      out.emplace_back(r.to_string());
   return out;
}

} // anonymous namespace

bool rule::matches(const chain::permission_level& auth, const chain::action& act) const {
   return pattern_matches(actor, auth.actor) && pattern_matches(permission, auth.permission) &&
          pattern_matches(contract, act.account) && pattern_matches(action, act.name);
}

std::string rule::to_string() const {
   return fmt::format("{}{}{}{}{}{}{}", pattern_to_string(actor), permission_separator, pattern_to_string(permission),
                      rule_part_separator, pattern_to_string(contract), action_separator, pattern_to_string(action));
}

rule parse_rule(std::string_view spec, std::string_view option_name, std::size_t position) {
   const rule_spec value{.text = spec, .format = rule_format, .option_name = option_name, .position = position};
   const auto [authority, act] = value.parts();

   const std::size_t permission_split = authority.find(permission_separator);
   if (permission_split != std::string_view::npos &&
       authority.find(permission_separator, permission_split + 1) != std::string_view::npos)
      value.fail(fmt::format("expected at most one '{}'", permission_separator));
   const std::string_view permission_text =
      permission_split == std::string_view::npos ? wildcard : authority.substr(permission_split + 1);

   name_pattern actor = value.name_field(authority.substr(0, permission_split), actor_label);
   name_pattern permission = value.name_field(permission_text, permission_label);
   auto [contract, action] = value.action_part(act);
   return rule{.actor = std::move(actor),
               .permission = std::move(permission),
               .contract = std::move(contract),
               .action = std::move(action)};
}

bool receiver_rule::matches(chain::name receiver_account, const chain::action& act) const {
   return pattern_matches(receiver, receiver_account) && pattern_matches(contract, act.account) &&
          pattern_matches(action, act.name);
}

std::string receiver_rule::to_string() const {
   return fmt::format("{}{}{}{}{}", pattern_to_string(receiver), rule_part_separator, pattern_to_string(contract),
                      action_separator, pattern_to_string(action));
}

receiver_rule parse_receiver_rule(std::string_view spec, std::string_view option_name, std::size_t position) {
   const rule_spec value{
      .text = spec, .format = receiver_rule_format, .option_name = option_name, .position = position};
   const auto [receiver_text, act] = value.parts();
   name_pattern receiver = value.name_field(receiver_text, receiver_label);
   auto [contract, action] = value.action_part(act);
   return receiver_rule{.receiver = std::move(receiver), .contract = std::move(contract), .action = std::move(action)};
}

rule_set::rule_set(std::vector<rule> includes, std::vector<rule> excludes, std::vector<receiver_rule> receivers)
   : _includes(std::move(includes))
   , _excludes(std::move(excludes))
   , _receivers(std::move(receivers)) {
   for (const rule& r : _includes)
      _include_index.add(r.actor, r);
   for (const receiver_rule& r : _receivers)
      _receiver_index.add(r.receiver, r);
}

std::vector<chain::permission_level> rule_set::matched_authorizations(const chain::action& act) const {
   std::vector<chain::permission_level> matched;
   if (_include_index.empty())
      return matched;
   for (const chain::permission_level& auth : act.authorization) {
      const auto matches_auth = [&](const rule& r) { return r.matches(auth, act); };
      // Most authorizations match nothing: the index rejects them before the payer check and the excludes run.
      if (!_include_index.any_of(auth.actor, matches_auth))
         continue;
      if (auth.permission == chain::config::sysio_payer_name && has_real_authorization(act, auth.actor))
         continue;
      if (std::ranges::none_of(_excludes, matches_auth))
         matched.push_back(auth);
   }
   return matched;
}

bool rule_set::matches_receiver(chain::name receiver_account, const chain::action& act) const {
   return _receiver_index.any_of(receiver_account,
                                 [&](const receiver_rule& r) { return r.matches(receiver_account, act); });
}

void add_options(bpo::options_description& cfg) {
   static const std::string include_description = fmt::format(
      "Report every action in an accepted block that one of these authorizations satisfies. Repeatable. Format: {}. "
      "Each field is an account name or '{}'; a rule without '{}<permission>' matches every permission. At least one "
      "of these or of the --{} rules is required.",
      rule_format, wildcard, permission_separator, option::receiver);
   static const std::string exclude_description = fmt::format(
      "Do not report an authorization an include rule matched when it also matches one of these rules. Repeatable, "
      "same format as --{}. An exclude covers only the authorization it matches: another matched authorization of the "
      "same action is still reported. Receiver rules are not affected.",
      option::include);
   static const std::string max_abi_bytes_description = fmt::format(
      "Largest packed contract ABI, in bytes, loaded to decode action data. Data of a contract with a larger ABI is "
      "reported as hex. Loading an ABI is the one step of decoding outside the block's decode time; this bounds it. "
      "Default {}.",
      default_max_abi_bytes);
   static const std::string receiver_description = fmt::format(
      "Report every action in an accepted block executed at this account: its own contract's actions, and every "
      "action it is notified of, such as a token transfer it sends or receives. Repeatable. Format: {}, each field an "
      "account name or '{}'. Anyone can send an account a notification.",
      receiver_rule_format, wildcard);
   cfg.add_options()(option::include.c_str(), bpo::value<std::vector<std::string>>()->composing(),
                     include_description.c_str())(
      option::exclude.c_str(), bpo::value<std::vector<std::string>>()->composing(), exclude_description.c_str())(
      option::receiver.c_str(), bpo::value<std::vector<std::string>>()->composing(), receiver_description.c_str())(
      option::max_data_bytes.c_str(), bpo::value<uint32_t>()->default_value(default_max_data_bytes),
      "Largest action data, in bytes, a match record decodes with the contract's ABI. Larger data is reported by size "
      "and sha256 only.")(option::max_abi_bytes.c_str(), bpo::value<uint32_t>()->default_value(default_max_abi_bytes),
                          max_abi_bytes_description.c_str())(
      option::heartbeat_seconds.c_str(), bpo::value<uint32_t>()->default_value(default_heartbeat_seconds),
      "Seconds between heartbeat records, written as blocks are accepted. 0 disables them.");
}

config parse_config(const bpo::variables_map& options) {
   rule_set rules(parse_rules(options, option::include, &parse_rule),
                  parse_rules(options, option::exclude, &parse_rule),
                  parse_rules(options, option::receiver, &parse_receiver_rule));
   SYS_ASSERT(!rules.includes().empty() || !rules.receivers().empty(), chain::plugin_config_exception,
              "at least one --{} or --{} is required", option::include, option::receiver);
   SYS_ASSERT(rules.excludes().empty() || !rules.includes().empty(), chain::plugin_config_exception,
              "--{} applies only to --{} rules, and none is given", option::exclude, option::include);
   return config{
      .rules = std::move(rules),
      .max_data_bytes = options.at(option::max_data_bytes).as<uint32_t>(),
      .max_abi_bytes = options.at(option::max_abi_bytes).as<uint32_t>(),
      .heartbeat_interval = fc::seconds(options.at(option::heartbeat_seconds).as<uint32_t>()),
   };
}

std::string to_json_line(const fc::variant_object& record) {
   return fc::json::to_string(fc::variant{record}, fc::time_point::maximum());
}

watcher::block_decoder::block_decoder(const chain::controller& chain, fc::microseconds max_time, uint32_t max_abi_bytes)
   : deadline(fc::time_point::now().safe_add(max_time))
   , yield(chain::abi_serializer::create_yield_function(max_time))
   , abis([&chain, max_abi_bytes, yield = yield](const chain::account_name& account) {
      return load_abi(chain, account, max_abi_bytes, yield);
   }) {}

watcher::watcher(config cfg, const chain::controller& chain, fc::microseconds abi_serializer_max_time, record_sink sink,
                 clock_function clock)
   : _cfg(std::move(cfg))
   , _chain(chain)
   , _abi_serializer_max_time(abi_serializer_max_time)
   , _sink(std::move(sink))
   , _clock(std::move(clock)) {}

void watcher::emit(event kind, const fc::mutable_variant_object& body) const {
   fc::mutable_variant_object record;
   record(field::event, magic_enum::enum_name(kind))(body);
   _sink(kind, record);
}

void watcher::report_start() const {
   fc::mutable_variant_object body;
   body(field::include, rules_to_variants(_cfg.rules.includes()))(field::exclude,
                                                                  rules_to_variants(_cfg.rules.excludes()))(
      field::receiver, rules_to_variants(_cfg.rules.receivers()))(field::max_data_bytes, _cfg.max_data_bytes)(
      field::max_abi_bytes, _cfg.max_abi_bytes)(field::heartbeat_seconds, _cfg.heartbeat_interval.to_seconds());
   emit(event::start, body);
}

void watcher::report_stop() const {
   emit(event::stop, fc::mutable_variant_object()(field::matches, _matches_reported));
}

void watcher::on_block_start(uint32_t) {
   if (!_held.empty())
      _held.clear();
}

void watcher::on_applied_transaction(const chain::transaction_trace_ptr& trace) {
   // Only transactions in block->transactions are reported (see on_accepted_block). A failed transaction and the
   // implicit onblock transaction are never there, so they are not held at all.
   if (!trace || !trace->receipt || trace->except || chain::is_onblock(*trace))
      return;
   std::vector<held_action> actions;
   for (std::size_t index = 0; index < trace->action_traces.size(); ++index) {
      const chain::action_trace& at = trace->action_traces[index];
      if (!at.receipt)
         continue;
      // A notification repeats its action under another receiver: the authorization rules check only the action's
      // own execution, the receiver rules every execution.
      std::vector<chain::permission_level> matched;
      if (at.receiver == at.act.account)
         matched = _cfg.rules.matched_authorizations(at.act);
      if (!matched.empty() || _cfg.rules.matches_receiver(at.receiver, at.act))
         actions.push_back(held_action{.trace_index = index, .matched = std::move(matched)});
   }
   if (!actions.empty())
      _held.emplace(trace->id, held_transaction{.trace = trace, .actions = std::move(actions)});
}

void watcher::on_accepted_block(const chain::signed_block_ptr& block, const chain::block_id_type& id) {
   if (!_held.empty()) {
      block_decoder decoder(_chain, _abi_serializer_max_time, _cfg.max_abi_bytes);
      for (const chain::transaction_receipt& receipt : block->transactions) {
         const auto held = _held.find(receipt.trx.id());
         if (held == _held.end())
            continue;
         for (const held_action& action : held->second.actions) {
            ++_matches_reported;
            emit(event::match, match_record(*block, id, *held->second.trace, action, decoder));
         }
      }
      _held.clear();
   }
   maybe_report_heartbeat(*block, id);
}

fc::mutable_variant_object watcher::match_record(const chain::signed_block& block, const chain::block_id_type& id,
                                                 const chain::transaction_trace& trace, const held_action& held,
                                                 block_decoder& decoder) const {
   const chain::action_trace& at = trace.action_traces.at(held.trace_index);
   fc::mutable_variant_object record;
   record(field::block_num, block.block_num())(field::block_id, id)(field::block_time, block.timestamp)(
      field::producer, block.producer)(field::lib_num, lib_num(block))(field::trx_id, trace.id)(
      field::action_ordinal, static_cast<uint32_t>(at.action_ordinal))
      // A decimal string: JSON writers quote 64-bit integers above 2^32, so a number would change type over time.
      (field::global_sequence, std::to_string(at.receipt->global_sequence))(field::contract, at.act.account)(
         field::action, at.act.name)(field::receiver, at.receiver)(field::authorization,
                                                                   at.act.authorization)(field::matched, held.matched);
   add_data(record, at.act, decoder);
   return record;
}

void watcher::add_data(fc::mutable_variant_object& record, const chain::action& act, block_decoder& decoder) const {
   record(field::data_size, act.data.size());
   if (act.data.size() > _cfg.max_data_bytes) {
      record(field::data_sha256, fc::sha256::hash(act.data.data(), act.data.size()));
      return;
   }
   if (std::optional<fc::variant> decoded = decode(act, decoder)) {
      record(field::data, std::move(*decoded));
      return;
   }
   record(field::data_hex, fc::to_hex(act.data.data(), static_cast<uint32_t>(act.data.size())));
}

std::optional<fc::variant> watcher::decode(const chain::action& act, block_decoder& decoder) const {
   if (fc::time_point::now() >= decoder.deadline) {
      tlog("{}: {}::{} data not decoded: the block's decode time is spent", logger_name, act.account, act.name);
      return std::nullopt;
   }
   try {
      const std::optional<std::reference_wrapper<const chain::abi_serializer>> abi = decoder.abis(act.account);
      if (!abi)
         return std::nullopt;
      const std::string type = abi->get().get_action_type(act.name);
      if (type.empty())
         return std::nullopt;
      return abi->get().binary_to_variant(type, act.data, decoder.yield);
   } catch (const fc::exception& e) {
      tlog("{}: {}::{} data not decoded: {}", logger_name, act.account, act.name, e.top_message());
   } catch (const std::exception& e) {
      tlog("{}: {}::{} data not decoded: {}", logger_name, act.account, act.name, e.what());
   }
   return std::nullopt;
}

uint32_t watcher::lib_num(const chain::signed_block& block) const {
   const uint32_t root = _chain.fork_db_has_root() ? _chain.fork_db_root().block_num() : 0;
   return _chain.get_read_mode() == chain::db_read_mode::IRREVERSIBLE ? std::max(root, block.block_num()) : root;
}

void watcher::maybe_report_heartbeat(const chain::signed_block& block, const chain::block_id_type& id) {
   if (_cfg.heartbeat_interval <= fc::microseconds{})
      return;
   const fc::time_point now = _clock();
   if (_last_heartbeat && now >= *_last_heartbeat && now - *_last_heartbeat < _cfg.heartbeat_interval)
      return;
   _last_heartbeat = now;
   fc::mutable_variant_object body;
   body(field::block_num, block.block_num())(field::block_id, id)(field::block_time, block.timestamp)(
      field::lib_num, lib_num(block))(field::matches, _matches_reported);
   emit(event::heartbeat, body);
}

} // namespace sysio::account_watch
