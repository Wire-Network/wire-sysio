#pragma once

#include <fc/time.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/action.hpp>
#include <sysio/chain/block.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/types.hpp>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sysio::chain {
class controller;
}

namespace sysio::account_watch {

/// Name of the fc::logger every record is written to.
inline constexpr std::string_view logger_name = "account_watch";

/// Program-option names; all are `cfg` options (command line and config.ini), fixed at startup. std::string, as
/// boost::program_options and the variables_map take them.
namespace option {
/// Repeatable authorization rule; at least one of these or of the receiver rules is required.
inline const std::string include = "account-watch-include";
/// Repeatable rule that removes authorizations an include rule matched; needs an include rule.
inline const std::string exclude = "account-watch-exclude";
/// Repeatable receiver rule: reports actions executed or notified at the account it names.
inline const std::string receiver = "account-watch-receiver";
/// Largest action data, in bytes, that a match record decodes.
inline const std::string max_data_bytes = "account-watch-max-data-bytes";
/// Largest packed contract ABI, in bytes, loaded to decode action data.
inline const std::string max_abi_bytes = "account-watch-max-abi-bytes";
/// Seconds between heartbeat records; 0 disables them.
inline const std::string heartbeat_seconds = "account-watch-heartbeat-seconds";
} // namespace option

/// Default of --account-watch-max-data-bytes: routine actions fit, setcode and setabi do not.
inline constexpr uint32_t default_max_data_bytes = 1024;
/// Default of --account-watch-max-abi-bytes: every contract ABI in the tree fits several times over. Unpacking and
/// indexing an ABI run outside the decode deadline, so this bounds what one load can add past it.
inline constexpr uint32_t default_max_abi_bytes = 64 * 1024;
/// Default of --account-watch-heartbeat-seconds.
inline constexpr uint32_t default_heartbeat_seconds = 600;

/// Format of an include or exclude rule, named in help text and errors.
inline constexpr std::string_view rule_format = "<actor>[@<permission>],<contract>::<action>";
/// Format of a receiver rule, named in help text and errors.
inline constexpr std::string_view receiver_rule_format = "<receiver>,<contract>::<action>";
/// A rule field that matches every name.
inline constexpr std::string_view wildcard = "*";
/// Separates the authorization part of a rule from its action part.
inline constexpr char rule_part_separator = ',';
/// Separates the actor from the permission in the authorization part of a rule.
inline constexpr char permission_separator = '@';
/// Separates the contract from the action in the action part of a rule.
inline constexpr std::string_view action_separator = "::";

/// Member names of the records. std::string, as fc::mutable_variant_object and fc::variant_object take them.
namespace field {
inline const std::string event = "event";
inline const std::string block_num = "block_num";
inline const std::string block_id = "block_id";
inline const std::string block_time = "block_time";
inline const std::string producer = "producer";
inline const std::string lib_num = "lib_num";
inline const std::string trx_id = "trx_id";
inline const std::string action_ordinal = "action_ordinal";
inline const std::string global_sequence = "global_sequence";
inline const std::string contract = "contract";
inline const std::string action = "action";
inline const std::string receiver = "receiver";
inline const std::string authorization = "authorization";
inline const std::string matched = "matched";
inline const std::string data_size = "data_size";
inline const std::string data = "data";
inline const std::string data_hex = "data_hex";
inline const std::string data_sha256 = "data_sha256";
inline const std::string matches = "matches";
inline const std::string include = "include";
inline const std::string exclude = "exclude";
inline const std::string max_data_bytes = "max_data_bytes";
inline const std::string max_abi_bytes = "max_abi_bytes";
inline const std::string heartbeat_seconds = "heartbeat_seconds";
} // namespace field

/// What a record reports; its name is the record's `event` member.
enum class event {
   start,     ///< the plugin is watching; carries the configuration
   match,     ///< an action in an accepted block matched the rules
   heartbeat, ///< the plugin is alive and blocks are being accepted
   stop,      ///< the plugin stopped; carries the match count
};

/// One name field of a rule: a specific name, or nullopt for wildcard.
using name_pattern = std::optional<chain::name>;

/// One --account-watch-include or --account-watch-exclude value: an authorization and an action, any field of which
/// may be the wildcard.
struct rule {
   name_pattern actor;      ///< the authorizing account
   name_pattern permission; ///< its permission; wildcard when the rule names no permission
   name_pattern contract;   ///< the account whose action it is
   name_pattern action;     ///< the action name

   /** True when @p auth, one authorization of @p act, satisfies every field. */
   bool matches(const chain::permission_level& auth, const chain::action& act) const;
   /** The rule in rule_format, with wildcard for each wildcard field and the permission always spelled out. */
   std::string to_string() const;
};

/**
 * Parse one rule in rule_format. Each field is wildcard or a valid account name; a missing `@<permission>` is the
 * wildcard. Throws chain::plugin_config_exception naming @p option_name, the value's @p position among the option's
 * values (command-line values first), the value, and what is wrong with it.
 */
rule parse_rule(std::string_view spec, std::string_view option_name, std::size_t position);

/// One --account-watch-receiver value: the account an action is executed at -- its contract, or an account it
/// notifies -- and the action, any field of which may be the wildcard.
struct receiver_rule {
   name_pattern receiver; ///< the account the action runs at
   name_pattern contract; ///< the account whose action it is
   name_pattern action;   ///< the action name

   /** True when @p act, executed at @p receiver_account, satisfies every field. */
   bool matches(chain::name receiver_account, const chain::action& act) const;
   /** The rule in receiver_rule_format, with wildcard for each wildcard field. */
   std::string to_string() const;
};

/**
 * Parse one receiver rule in receiver_rule_format. Each field is wildcard or a valid account name. Throws
 * chain::plugin_config_exception as parse_rule does.
 */
receiver_rule parse_receiver_rule(std::string_view spec, std::string_view option_name, std::size_t position);

/// Rules grouped by the account one of their fields names, so a lookup checks only the rules that can match that
/// account: those naming it, and those whose field is the wildcard.
template <typename Rule>
class rule_index {
public:
   /** Add @p r under @p account, the rule's field this index is keyed by. */
   void add(const name_pattern& account, const Rule& r) {
      if (account)
         _by_account[*account].push_back(r);
      else
         _any_account.push_back(r);
   }

   /** True when a rule that can match @p account satisfies @p pred. */
   template <typename Pred>
   bool any_of(chain::name account, Pred&& pred) const {
      if (std::ranges::any_of(_any_account, pred))
         return true;
      if (_by_account.empty())
         return false;
      const auto found = _by_account.find(account);
      return found != _by_account.end() && std::ranges::any_of(found->second, pred);
   }

   /** True when no rule was added. */
   bool empty() const { return _by_account.empty() && _any_account.empty(); }

private:
   boost::unordered_flat_map<chain::name, std::vector<Rule>, std::hash<chain::name>> _by_account; ///< by the account
   std::vector<Rule> _any_account; ///< the rules whose field is the wildcard
};

/// The include, exclude and receiver rules, indexed for matching. An action by an account no rule names is rejected
/// with one lookup, whatever the number of rules.
class rule_set {
public:
   rule_set() = default;
   /** The rules of the three options, each in option order. */
   rule_set(std::vector<rule> includes, std::vector<rule> excludes, std::vector<receiver_rule> receivers);

   /** --account-watch-include, in option order. */
   const std::vector<rule>& includes() const { return _includes; }
   /** --account-watch-exclude, in option order. */
   const std::vector<rule>& excludes() const { return _excludes; }
   /** --account-watch-receiver, in option order. */
   const std::vector<receiver_rule>& receivers() const { return _receivers; }

   /**
    * The authorizations of @p act that make it reportable: each matches an include rule and no exclude rule, so an
    * exclude rule covers only the authorization it names. An explicit payer (`<actor>@sysio.payer`) beside a real
    * authorization of the same actor is skipped: it only names who pays, and the real one is matched on its own. A
    * payer with no such partner, which only a privileged contract's inline action can carry, is matched. Empty when the
    * action is not reported.
    */
   std::vector<chain::permission_level> matched_authorizations(const chain::action& act) const;
   /** True when a receiver rule matches @p act executed at @p receiver_account. Excludes do not apply. */
   bool matches_receiver(chain::name receiver_account, const chain::action& act) const;

private:
   std::vector<rule> _includes;               ///< as given, for the start record
   std::vector<rule> _excludes;               ///< as given; checked only for an authorization an include matched
   std::vector<receiver_rule> _receivers;     ///< as given, for the start record
   rule_index<rule> _include_index;           ///< the include rules by actor
   rule_index<receiver_rule> _receiver_index; ///< the receiver rules by receiver
};

/// Everything the plugin needs, parsed once from its options.
struct config {
   rule_set rules;                                   ///< what is reported
   uint32_t max_data_bytes = default_max_data_bytes; ///< action data above this many bytes is hashed, not decoded
   uint32_t max_abi_bytes = default_max_abi_bytes;   ///< a contract ABI above this many bytes is never loaded
   fc::microseconds heartbeat_interval = fc::seconds(default_heartbeat_seconds); ///< zero: no heartbeat
};

/** Register the plugin's options on @p cfg (command line and config.ini). */
void add_options(boost::program_options::options_description& cfg);

/**
 * Parse the plugin's options. Throws chain::plugin_config_exception for a rule that does not parse (see parse_rule),
 * when neither an --account-watch-include nor an --account-watch-receiver rule is given (an enabled plugin with
 * nothing to watch is a configuration error), and for --account-watch-exclude rules without an include rule to apply
 * to.
 */
config parse_config(const boost::program_options::variables_map& options);

/** @p record as one line of JSON. */
std::string to_json_line(const fc::variant_object& record);

/// Receives every record a watcher produces, on the thread that drives the watcher.
using record_sink = std::function<void(event, const fc::variant_object&)>;
/// The wall clock a watcher times heartbeats by.
using clock_function = std::function<fc::time_point()>;

/**
 * Reports actions that match the rules, once per accepted block that contains them.
 *
 * Driven by the controller's block_start, applied_transaction and accepted_block signals, all on one thread. Matches
 * found as transactions apply are held by transaction id until the block is accepted, then reported for the block's
 * transactions in block order, so a transaction that fails, never makes it into a block, or is executed
 * speculatively and dropped is never reported. The authorization rules check an action once, at its own execution
 * (receiver == contract), never its notifications; the receiver rules check every execution, notifications included.
 * Inline actions count. Implicit transactions (onblock) carry no signature and are never reported. A fork switch
 * re-applies blocks, so an action can be reported once per block it appears in.
 */
class watcher {
public:
   /**
    * @param cfg the rules and limits
    * @param chain the controller whose signals drive this watcher; read for ABIs and the irreversible block
    * @param abi_serializer_max_time the time decoding may take per block, ABI loads included; once it is spent the
    *        block's remaining match records carry hex
    * @param sink receives every record
    * @param clock times the heartbeat
    */
   watcher(config cfg, const chain::controller& chain, fc::microseconds abi_serializer_max_time, record_sink sink,
           clock_function clock);

   /** Report the start record. */
   void report_start() const;
   /** Report the stop record. */
   void report_stop() const;

   /**
    * Drop the matches held since the last block start: their block was accepted and reported, or aborted. A
    * transaction executes at most once between two block starts, so a speculative execution's matches never reach the
    * block that later executes the transaction again.
    */
   void on_block_start(uint32_t block_num);
   /** Hold the matches of @p trace's executed actions until its block is accepted. */
   void on_applied_transaction(const chain::transaction_trace_ptr& trace);
   /** Report the matches of @p block's transactions, then a heartbeat when one is due. */
   void on_accepted_block(const chain::signed_block_ptr& block, const chain::block_id_type& id);

   /** Match records reported since construction. */
   uint64_t matches_reported() const { return _matches_reported; }

private:
   /// One matching action of a held transaction.
   struct held_action {
      /// index into the transaction's action_traces
      std::size_t trace_index = 0;
      /// matching authorizations; empty when only a receiver rule matched
      std::vector<chain::permission_level> matched;
   };
   /// A transaction with at least one matching action.
   struct held_transaction {
      chain::transaction_trace_ptr trace; ///< the execution that will be reported if its block is accepted
      std::vector<held_action> actions;   ///< its matching actions, in trace order
   };
   /// ABI lookups and the decode time budget of one accepted block. A wildcard-contract rule lets anyone choose the
   /// ABI, and an ABI can make a few bytes of data decode until the deadline, so ABI validation and every decode of
   /// the block share one deadline. Unpacking and indexing an ABI check no deadline; max_abi_bytes bounds them.
   struct block_decoder {
      block_decoder(const chain::controller& chain, fc::microseconds max_time, uint32_t max_abi_bytes);
      fc::time_point deadline;                       ///< when the budget is spent
      chain::abi_serializer::yield_function_t yield; ///< enforces deadline inside ABI validation and decodes
      chain::caching_resolver abis;                  ///< each contract's ABI, loaded at most once per block
   };

   /** Pass @p body, prefixed with the `event` member naming @p kind, to the sink. */
   void emit(event kind, const fc::mutable_variant_object& body) const;
   /** The match record of action @p held of @p trace in @p block, without its event member. */
   fc::mutable_variant_object match_record(const chain::signed_block& block, const chain::block_id_type& id,
                                           const chain::transaction_trace& trace, const held_action& held,
                                           block_decoder& decoder) const;
   /** Add data_size and one of data, data_hex or data_sha256 for @p act to @p record. */
   void add_data(fc::mutable_variant_object& record, const chain::action& act, block_decoder& decoder) const;
   /**
    * @p act's data decoded with its contract's ABI; nullopt once the block's budget is spent, without an ABI or an
    * action type, or on a failed decode.
    */
   std::optional<fc::variant> decode(const chain::action& act, block_decoder& decoder) const;
   /**
    * The last irreversible block number as @p block is accepted, 0 before one exists. In irreversible mode a block is
    * applied only once irreversible, before the fork database root moves past it, so it is at least the block's.
    */
   uint32_t lib_num(const chain::signed_block& block) const;
   /**
    * Report a heartbeat for @p block when heartbeat_interval has passed since the last one, or the wall clock has
    * stepped back past it.
    */
   void maybe_report_heartbeat(const chain::signed_block& block, const chain::block_id_type& id);

   config _cfg;                               ///< the rules and limits
   const chain::controller& _chain;           ///< read for ABIs, the read mode, and the irreversible block
   fc::microseconds _abi_serializer_max_time; ///< decode budget per accepted block
   record_sink _sink;                         ///< receives every record
   clock_function _clock;                     ///< times the heartbeat
   std::unordered_map<chain::transaction_id_type, held_transaction> _held; ///< matches of the block being applied
   std::optional<fc::time_point> _last_heartbeat;                          ///< nullopt until the first heartbeat
   uint64_t _matches_reported = 0;                                         ///< match records since construction
};

} // namespace sysio::account_watch
