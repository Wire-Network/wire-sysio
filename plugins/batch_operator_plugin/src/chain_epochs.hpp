#pragma once
/**
 * @file chain_epochs.hpp
 * @brief Each outpost chain's consensus epoch as one batch operator reads it, the relay cadence derived from the
 *        chain's epoch duration, and when the operator pushes `sysio.msgch::chkcons`. Pure functions over decoded
 *        `sysio.epoch` and `sysio.msgch` rows, so the plugin's tests drive them without a chain.
 *
 * Every outpost chain runs its own consensus epoch: `epochstate` and `epochcfg` hold one row per chain, keyed by its
 * `sysio.chains` code, and an operator's election on a chain is decided by that chain's own schedule window. A chain's
 * `epoch_duration_sec` is its minimum time between two epochs; it advances only once it is due (`due_at`), which needs
 * traffic, so an idle chain stays in its epoch.
 */

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fc/io/json.hpp>
#include <fc/slug_name.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/name.hpp>

#include "group_election.hpp"

namespace sysio::batch_operator_detail {

/// `sysio.epoch` identifiers of the per-chain consensus epoch tables.
namespace epoch {
   constexpr auto account          = "sysio.epoch";
   constexpr auto table_epochstate = "epochstate";   ///< one row per outpost chain
   constexpr auto table_epochcfg   = "epochcfg";     ///< one row per outpost chain
   namespace field {
      constexpr auto chain_code             = "chain_code";
      constexpr auto current_epoch_index    = "current_epoch_index";
      constexpr auto current_epoch_start    = "current_epoch_start";
      constexpr auto traffic_since          = "traffic_since";   ///< zero while the chain has no traffic
      constexpr auto due_at                 = "due_at";          ///< zero while the chain is idle
      constexpr auto current_batch_op_group = "current_batch_op_group";
      constexpr auto batch_op_groups        = "batch_op_groups";
      constexpr auto is_paused              = "is_paused";
      constexpr auto previous_group         = "previous_group";  ///< the group that served the previous epoch
      constexpr auto epoch_duration_sec     = "epoch_duration_sec";   ///< on `epochcfg`
      constexpr auto late_window_sec        = "late_window_sec";      ///< on `epochcfg`
   }
}

/// `sysio.msgch` identifiers of the per-chain consensus row: a chain's current epoch is decided once its `outpcons`
/// row records that epoch.
namespace consensus {
   constexpr auto account        = "sysio.msgch";
   constexpr auto table_outpcons = "outpcons";   ///< one row per outpost chain
   namespace field {
      constexpr auto chain_code  = "chain_code";
      constexpr auto epoch_index = "epoch_index";   ///< the epoch whose winner the row records
   }
}

/// `sysio.msgch` identifiers of the outpost backlog reports: `reportqueue` keeps each operator's latest report for a
/// chain in `qreports`, keyed by (chain, operator).
namespace backlog {
   constexpr auto account            = consensus::account;
   constexpr auto table_qreports     = "qreports";
   constexpr auto action_reportqueue = "reportqueue";
   namespace field {
      constexpr auto chain_code        = "chain_code";
      constexpr auto account           = "account";             ///< the reporter, on a `qreports` row
      constexpr auto epoch_index       = "epoch_index";         ///< the epoch the report was made in
      constexpr auto batch_op          = "batch_op";            ///< `reportqueue`'s reporter
      constexpr auto pending_count     = "pending_count";
      constexpr auto oldest_pending_ms = "oldest_pending_ms";
   }
}

/// Polls of a chain per epoch: a 360 s epoch is polled every 15 s.
inline constexpr int64_t polls_per_epoch = 24;
/// The shortest poll interval of any chain.
inline constexpr fc::microseconds min_poll_interval = fc::seconds(1);
/// Delivery timeouts per epoch: a 60 s epoch gives each outpost call 30 s, and leaves the rest for a retry.
inline constexpr int64_t delivery_timeouts_per_epoch = 2;
/// The shortest delivery timeout of any chain: that of sysio.epoch's shortest epoch, 10 s.
inline constexpr fc::microseconds min_delivery_timeout = fc::seconds(5);
/// The longest delivery timeout of any chain, so a hung outpost call holds a long epoch's relay no longer.
inline constexpr fc::microseconds max_delivery_timeout = fc::seconds(60);
/// Chain minimums a decided epoch stays due before every operator serving the chain, elected or not, pushes
/// `chkcons` for it: the elected group has that long, many of its polls, to advance it first.
inline constexpr int64_t liveness_grace_minimums = 1;

/// How the relay paces itself on one chain.
struct epoch_cadence {
   fc::microseconds poll_interval;      ///< between two polls of the chain's epoch state and outpost
   fc::microseconds delivery_timeout;   ///< bounds each outpost call, and each depot push, for the chain
};

/// The cadence of a chain whose epochs last at least `epoch_duration`: a poll every 1/24 of it, at least
/// `min_poll_interval` and at most `max_poll_interval` (`--batch-epoch-poll-ms`) apart, and a delivery timeout of
/// half of it, from `min_delivery_timeout` to `max_delivery_timeout`.
inline epoch_cadence derive_cadence(fc::microseconds epoch_duration, fc::microseconds max_poll_interval) {
   const fc::microseconds poll = std::max(fc::microseconds(epoch_duration.count() / polls_per_epoch),
                                          min_poll_interval);
   const fc::microseconds delivery = std::max(fc::microseconds(epoch_duration.count() / delivery_timeouts_per_epoch),
                                              min_delivery_timeout);
   return {.poll_interval    = std::min(poll, max_poll_interval),
           .delivery_timeout = std::min(delivery, max_delivery_timeout)};
}

/// One outpost chain's consensus epoch as this operator last read it.
struct chain_epoch_view {
   uint64_t         chain_code  = 0;
   uint32_t         epoch_index = 0;       ///< 0 before the chain's genesis advance
   fc::time_point   epoch_start;
   fc::time_point   traffic_since;         ///< when the chain's pending traffic began; zero while it has none
   fc::time_point   due_at;                ///< when the chain may advance; zero while it is idle
   fc::microseconds epoch_duration;        ///< the chain's minimum duration, from its `epochcfg` row
   /// How long after the current epoch's start the depot takes a late delivery of the previous one, from `epochcfg`.
   fc::microseconds late_window;
   bool             decided     = false;   ///< the current epoch has its accepted winner (`outpcons`)
   bool             is_paused   = false;   ///< a dispute holds the chain's advance; nobody is elected on it
   /// This operator sat in the group that served the previous epoch, which may still deliver for it late.
   bool             in_previous_group = false;
   group_election   election;              ///< this operator's standing on the chain's own window
   epoch_cadence    cadence;               ///< derived from `epoch_duration`
};

/// Each chain's view, by chain code.
using chain_epochs = std::map<uint64_t, chain_epoch_view>;

/// What one read of `epochstate`, `epochcfg` and `outpcons` decodes to.
struct decoded_chain_epochs {
   chain_epochs             views;
   /// Each row left out, and why: a state row without a config row, or any row that did not decode.
   std::vector<std::string> skipped;
};

/// The epoch each chain's `outpcons` row records a winner for, by chain code.
using decided_epochs = std::map<uint64_t, uint32_t>;

/// Whether `operator_account` is among `members`, an `epochstate` group as the chain renders it.
inline bool group_has(const fc::variants& members, chain::name operator_account) {
   return std::ranges::any_of(
      members, [&](const fc::variant& member) { return chain::name(member.as_string()) == operator_account; });
}

/// Decode one chain's `epochstate` value against its `epochcfg` value and the `decided` epochs. While the chain is
/// paused nobody is elected on it, though this operator's seat in its window still stands. Throws when a field is
/// missing.
inline chain_epoch_view decode_chain_epoch(const fc::variant_object& state, const fc::variant_object& config,
                                           const decided_epochs& decided, chain::name operator_account,
                                           fc::microseconds max_poll_interval) {
   chain_epoch_view view;
   view.chain_code     = state[epoch::field::chain_code].as_uint64();
   view.epoch_index    = state[epoch::field::current_epoch_index].as<uint32_t>();
   view.epoch_start    = state[epoch::field::current_epoch_start].as<fc::time_point>();
   view.traffic_since  = state[epoch::field::traffic_since].as<fc::time_point>();
   view.due_at         = state[epoch::field::due_at].as<fc::time_point>();
   view.epoch_duration = fc::seconds(config[epoch::field::epoch_duration_sec].as<uint32_t>());
   view.late_window    = fc::seconds(config[epoch::field::late_window_sec].as<uint32_t>());
   view.is_paused      = state[epoch::field::is_paused].as_bool();
   view.in_previous_group = group_has(state[epoch::field::previous_group].get_array(), operator_account);
   const auto decided_epoch = decided.find(view.chain_code);
   view.decided  = view.epoch_index > 0 && decided_epoch != decided.end() && decided_epoch->second == view.epoch_index;
   view.election = evaluate_group_election(state[epoch::field::current_batch_op_group].as<uint8_t>(),
                                           state[epoch::field::batch_op_groups].get_array(), operator_account);
   if (view.is_paused) view.election.is_elected = false;
   view.cadence = derive_cadence(view.epoch_duration, max_poll_interval);
   return view;
}

/// Every chain's view from all `epochstate`, `epochcfg` and `outpcons` values. A state row without a config row, or one
/// that does not decode, is left out, so nobody is elected on that chain until it reads cleanly. A chain whose
/// `outpcons` row does not decode reads as undecided.
inline decoded_chain_epochs decode_chain_epochs(const fc::variants& state_rows, const fc::variants& config_rows,
                                                const fc::variants& consensus_rows, chain::name operator_account,
                                                fc::microseconds max_poll_interval) {
   decoded_chain_epochs decoded;
   decided_epochs       decided;
   for (const fc::variant& row : consensus_rows) {
      try {
         const fc::variant_object& consensus_row = row.get_object();
         decided.emplace(consensus_row[consensus::field::chain_code].as_uint64(),
                         consensus_row[consensus::field::epoch_index].as<uint32_t>());
      } catch (const fc::exception& e) {
         decoded.skipped.push_back(std::format("an outpcons row did not decode: {}", e.top_message()));
      }
   }
   std::map<uint64_t, const fc::variant_object*> configs;
   for (const fc::variant& row : config_rows) {
      try {
         const fc::variant_object& config = row.get_object();
         configs.emplace(config[epoch::field::chain_code].as_uint64(), &config);
      } catch (const fc::exception& e) {
         decoded.skipped.push_back(std::format("an epochcfg row did not decode: {}", e.top_message()));
      }
   }
   for (const fc::variant& row : state_rows) {
      try {
         const fc::variant_object& state      = row.get_object();
         const uint64_t            chain_code = state[epoch::field::chain_code].as_uint64();
         const auto                config     = configs.find(chain_code);
         if (config == configs.end()) {
            decoded.skipped.push_back(
               std::format("chain {} has epoch state but no epochcfg row", fc::slug_name{chain_code}.to_string()));
            continue;
         }
         decoded.views.emplace(
            chain_code, decode_chain_epoch(state, *config->second, decided, operator_account, max_poll_interval));
      } catch (const fc::exception& e) {
         decoded.skipped.push_back(std::format("an epochstate row did not decode: {}", e.top_message()));
      }
   }
   return decoded;
}

/// Whether `view`'s chain is due its next advance at `now`: it has traffic and its `due_at` has passed. An idle chain
/// is never due.
inline bool is_advance_due(const chain_epoch_view& view, fc::time_point now) {
   return view.due_at != fc::time_point() && now >= view.due_at;
}

/// Whether this operator may still deliver `view`'s previous epoch late at `now`: it sat in the group that served it,
/// and the chain's late-delivery window, from its current epoch's start, is open. The depot refuses one after it.
inline bool late_delivery_open(const chain_epoch_view& view, fc::time_point now) {
   return view.epoch_index > 1 && view.in_previous_group && now < view.epoch_start + view.late_window;
}

/// Whether this operator pushes `sysio.msgch::chkcons` for `view`'s chain right after its own delivery of the chain's
/// current epoch landed at `now`: the chain is past its genesis, not paused, and due. The delivery may itself have
/// decided the epoch, which the last-read view cannot show yet; on an epoch it left undecided, `chkcons` re-tallies.
inline bool pushes_chkcons_after_delivery(const chain_epoch_view& view, fc::time_point now) {
   return view.epoch_index > 0 && !view.is_paused && is_advance_due(view, now);
}

/// Why this operator pushes `sysio.msgch::chkcons` for a chain on the epoch tick.
enum class chkcons_reason {
   /// Elected, and the decided epoch is due: `chkcons` advances it.
   due,
   /// Elected, and the epoch has been undecided past the chain minimum: `chkcons` re-tallies it, since its eligible
   /// members can change with no new delivery. At most once per chain minimum from this operator.
   retally,
   /// Serving the chain, elected or not, and the decided epoch has stayed due past the liveness grace: `chkcons`
   /// advances it while its elected group does not.
   liveness
};

/// One `chkcons` push the epoch tick makes, and why.
struct chkcons_push {
   uint64_t       chain_code = 0;
   chkcons_reason reason     = chkcons_reason::due;

   bool operator==(const chkcons_push&) const = default;
};

/// When this operator last pushed a re-tally for each chain, by chain code.
using retally_pushes = std::map<uint64_t, fc::time_point>;

/// `count` times a chain's minimum duration `minimum`.
inline fc::microseconds chain_minimums(fc::microseconds minimum, int64_t count) {
   return fc::microseconds(minimum.count() * count);
}

/// How long past a decided epoch's `due_at` every serving operator pushes `chkcons` for it: `liveness_grace_minimums`
/// of the chain's minimum duration.
inline fc::microseconds liveness_grace(const chain_epoch_view& view) {
   return chain_minimums(view.epoch_duration, liveness_grace_minimums);
}

/// Why this operator pushes `sysio.msgch::chkcons` for `view`'s chain at `now`, or nullopt when it does not. Nothing
/// is pushed before the chain's genesis advance or while a dispute pauses it.
///
/// @param last_retally    when this operator last pushed a re-tally for the chain; zero when it never did.
/// @param serves_actively this operator serves the chain and its status is ACTIVE, which the liveness push needs.
inline std::optional<chkcons_reason> chkcons_reason_of(const chain_epoch_view& view, fc::time_point now,
                                                       fc::time_point last_retally, bool serves_actively) {
   if (view.epoch_index == 0 || view.is_paused) return std::nullopt;
   if (view.election.is_elected) {
      if (view.decided) return is_advance_due(view, now) ? std::optional(chkcons_reason::due) : std::nullopt;
      // Undecided a chain minimum past its start, and a chain minimum past this operator's last re-tally.
      const fc::time_point retally_at = std::max(view.epoch_start, last_retally) + view.epoch_duration;
      return now >= retally_at ? std::optional(chkcons_reason::retally) : std::nullopt;
   }
   if (serves_actively && view.decided && is_advance_due(view, now) && now >= view.due_at + liveness_grace(view)) {
      return chkcons_reason::liveness;
   }
   return std::nullopt;
}

/// The `relayed` chains this operator pushes `sysio.msgch::chkcons` for at `now`, in chain-code order, and why
/// (`chkcons_reason_of`, with each chain's entry of `retallied` as its last re-tally). `serves_actively` is whether
/// the operator's status is ACTIVE, which the liveness push needs.
inline std::vector<chkcons_push> chkcons_chains(const chain_epochs& views, const std::set<uint64_t>& relayed,
                                                fc::time_point now, const retally_pushes& retallied,
                                                bool serves_actively) {
   std::vector<chkcons_push> pushed;
   for (const uint64_t chain_code : relayed) {
      const auto view = views.find(chain_code);
      if (view == views.end()) continue;
      const auto           last_retally = retallied.find(chain_code);
      const fc::time_point last         = last_retally == retallied.end() ? fc::time_point() : last_retally->second;
      if (const std::optional<chkcons_reason> reason = chkcons_reason_of(view->second, now, last, serves_actively)) {
         pushed.push_back({.chain_code = chain_code, .reason = *reason});
      }
   }
   return pushed;
}

/// Record `now` in `retallied` as the last re-tally of each chain `pushes` re-tallies, so `chkcons_chains` holds this
/// operator's next re-tally of it back a chain minimum.
inline void record_retallies(retally_pushes& retallied, const std::vector<chkcons_push>& pushes, fc::time_point now) {
   for (const chkcons_push& push : pushes) {
      if (push.reason == chkcons_reason::retally) retallied[push.chain_code] = now;
   }
}

/// The exact-lookup key of `account`'s `sysio.msgch::qreports` row for `chain_code`, as chain_plugin's key codec
/// expects it.
inline std::string queue_report_find_bound(uint64_t chain_code, chain::name account) {
   return fc::json::to_string(fc::mutable_variant_object()(backlog::field::chain_code, chain_code)(
                                 backlog::field::account, account.to_string()),
                              fc::json::yield_function_t{});
}

/// Whether `rows`, the result of `account`'s exact `qreports` lookup for `chain_code`, hold its report made in
/// `epoch_index`. Throws when the row does not decode.
inline bool reported_in_epoch(const fc::variants& rows, uint64_t chain_code, chain::name account,
                              uint32_t epoch_index) {
   return std::ranges::any_of(rows, [&](const fc::variant& row) {
      const fc::variant_object& report = row.get_object();
      return report[backlog::field::chain_code].as_uint64() == chain_code &&
             chain::name(report[backlog::field::account].as_string()) == account &&
             report[backlog::field::epoch_index].as<uint32_t>() == epoch_index;
   });
}

/// How often the relay polls `chain_code`: its cadence's poll interval, or `max_poll_interval` while it has no view.
inline fc::microseconds poll_interval_of(const chain_epochs& views, uint64_t chain_code,
                                         fc::microseconds max_poll_interval) {
   const auto view = views.find(chain_code);
   return view == views.end() ? max_poll_interval : view->second.cadence.poll_interval;
}

/// How long each outpost call of `chain_code`'s relay, and each of its depot pushes, may take: its cadence's delivery
/// timeout, or `max_delivery_timeout` while it has no view.
inline fc::microseconds delivery_timeout_of(const chain_epochs& views, uint64_t chain_code) {
   const auto view = views.find(chain_code);
   return view == views.end() ? max_delivery_timeout : view->second.cadence.delivery_timeout;
}

/// How often the epoch tick reads the chains: the shortest poll interval of the `relayed` chains, or
/// `max_poll_interval` when there is none.
inline fc::microseconds epoch_tick_interval(const chain_epochs& views, const std::set<uint64_t>& relayed,
                                            fc::microseconds max_poll_interval) {
   fc::microseconds interval = max_poll_interval;
   for (const uint64_t chain_code : relayed) {
      interval = std::min(interval, poll_interval_of(views, chain_code, max_poll_interval));
   }
   return interval;
}

/// The epoch tick's cron step: it fires every `min_poll_interval`, or every `max_poll_interval` when that is shorter.
inline fc::microseconds epoch_tick_step(fc::microseconds max_poll_interval) {
   return std::min(min_poll_interval, max_poll_interval);
}

/// When the epoch tick, firing every `tick_step`, next reads after a read at `read_at`: `interval` later, less half a
/// step, so a tick that fires a little before that mark reads then rather than one step late.
inline fc::time_point next_epoch_read_at(fc::time_point read_at, fc::microseconds interval,
                                         fc::microseconds tick_step) {
   return read_at + interval - fc::microseconds(tick_step.count() / 2);
}

} // namespace sysio::batch_operator_detail
