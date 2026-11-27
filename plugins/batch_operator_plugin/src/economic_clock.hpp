#pragma once
/**
 * @file economic_clock.hpp
 * @brief Which `sysio.epoch` economic epochs are due for a `tickeconomy` crank. Pure functions over
 *        decoded rows, so the plugin's tests drive them without a chain.
 *
 * The economic clock runs on a fixed grid from `economycfg.start_time` and never waits on outpost
 * consensus. Each `tickeconomy(epoch_index)` opens exactly the next economic epoch once its boundary
 * has passed, so after a stall (or with a start time in the past) the crank catches up one epoch per
 * transaction.
 */

#include <cstdint>
#include <optional>
#include <vector>

#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

namespace sysio::batch_operator_detail {

/// `sysio.epoch` identifiers the economic-clock crank touches.
namespace economy {
   /// Bound the serial `tickeconomy` pushes one poll may make while the clock catches up.
   inline constexpr uint32_t ticks_per_poll = 10;
   constexpr auto account            = "sysio.epoch";
   constexpr auto table_economycfg   = "economycfg";     ///< the `kv::global` config singleton
   constexpr auto table_economystate = "economystate";   ///< the `kv::global` state singleton
   constexpr auto table_netgenesis   = "netgenesis";     ///< the `kv::global` network genesis marker
   constexpr auto action_tickeconomy = "tickeconomy";
   namespace field {
      constexpr auto epoch_duration_sec  = "epoch_duration_sec";
      constexpr auto start_time          = "start_time";
      constexpr auto current_epoch_index = "current_epoch_index";
      constexpr auto next_epoch_start    = "next_epoch_start";
      constexpr auto epoch_index         = "epoch_index";   ///< `tickeconomy`'s argument
   }
}

/// The economic clock as the crank needs it: the last opened epoch and the next boundary.
struct economic_clock_view {
   uint32_t         current_epoch_index = 0;   ///< 0 before the first tick
   fc::time_point   next_boundary;             ///< when economic epoch `current_epoch_index + 1` opens
   fc::microseconds epoch_duration;            ///< the configured economic epoch length
};

/// Decode the `economycfg` value and, once the clock has ticked, the `economystate` value. Before the
/// first tick (`state` absent or index 0) the next boundary is the configured start time. Nullopt
/// when a required field is missing or the duration is zero.
inline std::optional<economic_clock_view> decode_economic_clock(const fc::variant_object& config,
                                                                const fc::variant_object* state) {
   if (!config.contains(economy::field::epoch_duration_sec) || !config.contains(economy::field::start_time)) {
      return std::nullopt;
   }
   const uint64_t duration_sec = config[economy::field::epoch_duration_sec].as_uint64();
   if (duration_sec == 0) return std::nullopt;

   economic_clock_view view;
   view.epoch_duration = fc::seconds(static_cast<int64_t>(duration_sec));

   if (state != nullptr && state->contains(economy::field::current_epoch_index)) {
      view.current_epoch_index = static_cast<uint32_t>((*state)[economy::field::current_epoch_index].as_uint64());
   }
   if (view.current_epoch_index > 0) {
      if (!state->contains(economy::field::next_epoch_start)) return std::nullopt;
      fc::from_variant((*state)[economy::field::next_epoch_start], view.next_boundary);
   } else {
      fc::time_point_sec start;
      fc::from_variant(config[economy::field::start_time], start);
      view.next_boundary = start.to_time_point();
   }
   return view;
}

/// The economic epoch indexes due at `now`, oldest first, at most `limit`. Each later boundary is the
/// previous one plus the configured duration, the same grid `tickeconomy` walks. None before the
/// network genesis (`network_genesis_done` false), which `tickeconomy` waits for; a dispute pause does
/// not hold the economic clock.
inline std::vector<uint32_t> economic_epochs_due(const economic_clock_view& clock, bool network_genesis_done,
                                                 fc::time_point now, uint32_t limit) {
   std::vector<uint32_t> due;
   if (!network_genesis_done) return due;
   fc::time_point boundary = clock.next_boundary;
   for (uint32_t index = clock.current_epoch_index + 1; due.size() < limit && now >= boundary; ++index) {
      due.push_back(index);
      boundary += clock.epoch_duration;
   }
   return due;
}

} // namespace sysio::batch_operator_detail
