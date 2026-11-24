#pragma once
/**
 * @file chain_epochs.hpp
 * @brief Each outpost chain's consensus epoch as one batch operator reads it, and the relay cadence derived from the
 *        chain's epoch duration. Pure functions over decoded `sysio.epoch` rows, so the plugin's tests drive them
 *        without a chain.
 *
 * Every outpost chain runs its own consensus epoch: `epochstate` and `epochcfg` hold one row per chain, keyed by its
 * `sysio.chains` code, and an operator's election on a chain is decided by that chain's own schedule window.
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
      constexpr auto next_epoch_start       = "next_epoch_start";
      constexpr auto current_batch_op_group = "current_batch_op_group";
      constexpr auto batch_op_groups        = "batch_op_groups";
      constexpr auto is_paused              = "is_paused";
      constexpr auto epoch_duration_sec     = "epoch_duration_sec";   ///< on `epochcfg`
   }
}

/// Polls of a chain per epoch: a 360 s epoch is polled every 15 s.
inline constexpr int64_t polls_per_epoch = 24;
/// Edge buffers per epoch: a 360 s epoch holds deliveries back 2.5 s from either edge.
inline constexpr int64_t edge_buffers_per_epoch = 144;
/// The shortest poll interval of any chain.
inline constexpr fc::microseconds min_poll_interval = fc::seconds(1);
/// The shortest edge buffer of any chain: one WIRE block.
inline constexpr fc::microseconds min_edge_buffer = fc::microseconds(chain::config::block_interval_us);

/// How the relay paces itself on one chain.
struct epoch_cadence {
   fc::microseconds poll_interval;   ///< between two polls of the chain's epoch state and outpost
   fc::microseconds edge_buffer;     ///< deliveries are held back this close to either edge of an epoch
};

/// The cadence of a chain whose epochs last `epoch_duration`: a poll every 1/24 of the epoch, at least
/// `min_poll_interval` and at most `max_poll_interval` (`--batch-epoch-poll-ms`) apart, and an edge buffer of 1/144 of
/// the epoch, at least `min_edge_buffer`.
inline epoch_cadence derive_cadence(fc::microseconds epoch_duration, fc::microseconds max_poll_interval) {
   const fc::microseconds poll = std::max(fc::microseconds(epoch_duration.count() / polls_per_epoch),
                                          min_poll_interval);
   return {.poll_interval = std::min(poll, max_poll_interval),
           .edge_buffer   = std::max(fc::microseconds(epoch_duration.count() / edge_buffers_per_epoch),
                                     min_edge_buffer)};
}

/// One outpost chain's consensus epoch as this operator last read it.
struct chain_epoch_view {
   uint64_t         chain_code  = 0;
   uint32_t         epoch_index = 0;       ///< 0 before the chain's genesis advance
   fc::time_point   epoch_start;
   fc::time_point   next_epoch_start;
   fc::microseconds epoch_duration;        ///< from the chain's `epochcfg` row
   bool             is_paused   = false;   ///< a dispute holds the chain's advance; nobody is elected on it
   group_election   election;              ///< this operator's standing on the chain's own window
   epoch_cadence    cadence;               ///< derived from `epoch_duration`
};

/// Each chain's view, by chain code.
using chain_epochs = std::map<uint64_t, chain_epoch_view>;

/// What one read of `epochstate` and `epochcfg` decodes to.
struct decoded_chain_epochs {
   chain_epochs             views;
   /// Each state row left out, and why: one without a config row, or one that did not decode.
   std::vector<std::string> skipped;
};

/// Decode one chain's `epochstate` value against its `epochcfg` value. While the chain is paused nobody is elected
/// on it, though this operator's seat in its window still stands. Throws when a field is missing.
inline chain_epoch_view decode_chain_epoch(const fc::variant_object& state, const fc::variant_object& config,
                                           chain::name operator_account, fc::microseconds max_poll_interval) {
   chain_epoch_view view;
   view.chain_code       = state[epoch::field::chain_code].as_uint64();
   view.epoch_index      = state[epoch::field::current_epoch_index].as<uint32_t>();
   view.epoch_start      = state[epoch::field::current_epoch_start].as<fc::time_point>();
   view.next_epoch_start = state[epoch::field::next_epoch_start].as<fc::time_point>();
   view.epoch_duration   = fc::seconds(config[epoch::field::epoch_duration_sec].as<uint32_t>());
   view.is_paused        = state[epoch::field::is_paused].as_bool();
   view.election         = evaluate_group_election(state[epoch::field::current_batch_op_group].as<uint8_t>(),
                                                   state[epoch::field::batch_op_groups].get_array(), operator_account);
   if (view.is_paused) view.election.is_elected = false;
   view.cadence = derive_cadence(view.epoch_duration, max_poll_interval);
   return view;
}

/// Every chain's view from all `epochstate` and `epochcfg` values. A state row without a config row, or one that does
/// not decode, is left out, so nobody is elected on that chain until it reads cleanly.
inline decoded_chain_epochs decode_chain_epochs(const fc::variants& state_rows, const fc::variants& config_rows,
                                                chain::name operator_account, fc::microseconds max_poll_interval) {
   decoded_chain_epochs decoded;
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
         decoded.views.emplace(chain_code,
                               decode_chain_epoch(state, *config->second, operator_account, max_poll_interval));
      } catch (const fc::exception& e) {
         decoded.skipped.push_back(std::format("an epochstate row did not decode: {}", e.top_message()));
      }
   }
   return decoded;
}

/// Whether `now` lies in `view`'s safe operating window: at least the chain's edge buffer after its epoch started and
/// not within it before the next epoch starts. Once that start has passed the epoch is overdue and the window is open.
inline bool within_epoch_window(const chain_epoch_view& view, fc::time_point now) {
   if (now < view.epoch_start + view.cadence.edge_buffer) return false;
   if (view.next_epoch_start != fc::time_point() && now > view.next_epoch_start - view.cadence.edge_buffer &&
       now < view.next_epoch_start)
      return false;
   return true;
}

/// Whether `now` has passed `view`'s next epoch start. False while the chain has none.
inline bool is_epoch_boundary_past(const chain_epoch_view& view, fc::time_point now) {
   return view.next_epoch_start != fc::time_point() && now >= view.next_epoch_start;
}

/// How often the relay polls `chain_code`: its cadence's poll interval, or `max_poll_interval` while it has no view.
inline fc::microseconds poll_interval_of(const chain_epochs& views, uint64_t chain_code,
                                         fc::microseconds max_poll_interval) {
   const auto view = views.find(chain_code);
   return view == views.end() ? max_poll_interval : view->second.cadence.poll_interval;
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

/// The `relayed` chains this operator is elected on whose epoch boundary has passed at `now`, in chain-code order: the
/// chains it pushes `sysio.msgch::chkcons` for, which advances a chain only past its boundary.
inline std::vector<uint64_t> chkcons_chains(const chain_epochs& views, const std::set<uint64_t>& relayed,
                                            fc::time_point now) {
   std::vector<uint64_t> due;
   for (const uint64_t chain_code : relayed) {
      const auto view = views.find(chain_code);
      if (view != views.end() && view->second.election.is_elected && is_epoch_boundary_past(view->second, now)) {
         due.push_back(chain_code);
      }
   }
   return due;
}

} // namespace sysio::batch_operator_detail
