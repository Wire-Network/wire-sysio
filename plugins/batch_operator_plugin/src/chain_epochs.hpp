#pragma once
/**
 * @file chain_epochs.hpp
 * @brief Each outpost chain's consensus epoch as one batch operator reads it, and the relay cadence derived from the
 *        chain's epoch duration. Pure functions over decoded `sysio.epoch` rows, so the plugin's tests drive them
 *        without a chain.
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
#include <set>
#include <string>
#include <vector>

#include <fc/slug_name.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/config.hpp>
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
      constexpr auto epoch_duration_sec     = "epoch_duration_sec";   ///< on `epochcfg`
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

/// Polls of a chain per epoch: a 360 s epoch is polled every 15 s.
inline constexpr int64_t polls_per_epoch = 24;
/// Edge buffers per epoch: a 360 s epoch holds deliveries back 2.5 s from either edge of its minimum duration.
inline constexpr int64_t edge_buffers_per_epoch = 144;
/// The shortest poll interval of any chain.
inline constexpr fc::microseconds min_poll_interval = fc::seconds(1);
/// The shortest edge buffer of any chain: one WIRE block.
inline constexpr fc::microseconds min_edge_buffer = fc::microseconds(chain::config::block_interval_us);
/// Delivery timeouts per epoch: a 60 s epoch gives each outpost call 30 s, and leaves the rest for a retry.
inline constexpr int64_t delivery_timeouts_per_epoch = 2;
/// The shortest delivery timeout of any chain: that of sysio.epoch's shortest epoch, 10 s.
inline constexpr fc::microseconds min_delivery_timeout = fc::seconds(5);
/// The longest delivery timeout of any chain, so a hung outpost call holds a long epoch's relay no longer.
inline constexpr fc::microseconds max_delivery_timeout = fc::seconds(60);

/// How the relay paces itself on one chain.
struct epoch_cadence {
   fc::microseconds poll_interval;      ///< between two polls of the chain's epoch state and outpost
   fc::microseconds edge_buffer;        ///< deliveries are held back this close to either edge of an epoch
   fc::microseconds delivery_timeout;   ///< bounds each outpost call, and each depot push, for the chain
};

/// The cadence of a chain whose epochs last `epoch_duration`: a poll every 1/24 of the epoch, at least
/// `min_poll_interval` and at most `max_poll_interval` (`--batch-epoch-poll-ms`) apart; an edge buffer of 1/144 of
/// the epoch, at least `min_edge_buffer`; and a delivery timeout of half the epoch, from `min_delivery_timeout` to
/// `max_delivery_timeout`.
inline epoch_cadence derive_cadence(fc::microseconds epoch_duration, fc::microseconds max_poll_interval) {
   const fc::microseconds poll = std::max(fc::microseconds(epoch_duration.count() / polls_per_epoch),
                                          min_poll_interval);
   const fc::microseconds delivery = std::max(fc::microseconds(epoch_duration.count() / delivery_timeouts_per_epoch),
                                              min_delivery_timeout);
   return {.poll_interval    = std::min(poll, max_poll_interval),
           .edge_buffer      = std::max(fc::microseconds(epoch_duration.count() / edge_buffers_per_epoch),
                                        min_edge_buffer),
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
   bool             decided     = false;   ///< the current epoch has its accepted winner (`outpcons`)
   bool             is_paused   = false;   ///< a dispute holds the chain's advance; nobody is elected on it
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
   view.is_paused      = state[epoch::field::is_paused].as_bool();
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

/// The earliest time `view`'s current epoch can close: its start plus the chain's minimum duration. Zero before the
/// chain's genesis advance.
inline fc::time_point minimum_epoch_end(const chain_epoch_view& view) {
   return view.epoch_index == 0 ? fc::time_point() : view.epoch_start + view.epoch_duration;
}

/// Whether `now` lies in `view`'s safe operating window: at least the chain's edge buffer after its epoch started and
/// not within it before the epoch's minimum end (`minimum_epoch_end`). Once that end has passed the window is open.
inline bool within_epoch_window(const chain_epoch_view& view, fc::time_point now) {
   if (now < view.epoch_start + view.cadence.edge_buffer) return false;
   const fc::time_point minimum_end = minimum_epoch_end(view);
   if (minimum_end != fc::time_point() && now > minimum_end - view.cadence.edge_buffer && now < minimum_end)
      return false;
   return true;
}

/// Whether `now` has passed `view`'s minimum epoch end (`minimum_epoch_end`). False before the chain's genesis
/// advance.
inline bool is_epoch_boundary_past(const chain_epoch_view& view, fc::time_point now) {
   const fc::time_point minimum_end = minimum_epoch_end(view);
   return minimum_end != fc::time_point() && now >= minimum_end;
}

/// Whether `view`'s chain is due its next advance at `now`: it has traffic and its `due_at` has passed. An idle chain
/// is never due.
inline bool is_advance_due(const chain_epoch_view& view, fc::time_point now) {
   return view.due_at != fc::time_point() && now >= view.due_at;
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

/// Whether this operator pushes `sysio.msgch::chkcons` for `view`'s chain at `now`: it is elected on the chain, past
/// its genesis, and either the epoch is decided and the chain is due (`chkcons` advances it), or the epoch is undecided
/// past its minimum end (`chkcons` re-tallies it). A decided epoch on an idle chain gets no push.
inline bool pushes_chkcons(const chain_epoch_view& view, fc::time_point now) {
   if (!view.election.is_elected || view.epoch_index == 0) return false;
   return view.decided ? is_advance_due(view, now) : is_epoch_boundary_past(view, now);
}

/// The `relayed` chains this operator pushes `sysio.msgch::chkcons` for at `now` (`pushes_chkcons`), in chain-code
/// order.
inline std::vector<uint64_t> chkcons_chains(const chain_epochs& views, const std::set<uint64_t>& relayed,
                                            fc::time_point now) {
   std::vector<uint64_t> pushed;
   for (const uint64_t chain_code : relayed) {
      const auto view = views.find(chain_code);
      if (view != views.end() && pushes_chkcons(view->second, now)) pushed.push_back(chain_code);
   }
   return pushed;
}

} // namespace sysio::batch_operator_detail
