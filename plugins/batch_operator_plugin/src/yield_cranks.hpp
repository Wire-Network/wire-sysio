#pragma once
/**
 * @file yield_cranks.hpp
 * @brief The decisions behind the depot's two yield cranks -- which `sysio.swap`
 *        yield pools are worth a `tickyield`, which `sysio.liq` shadows are worth a
 *        `queueyield` -- and the per-key spacing that keeps one operator from
 *        cranking the same pool every poll. Pure functions over decoded rows, so
 *        the plugin's tests drive them without a chain.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <fc/exception/exception.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/symbol.hpp>

namespace sysio::batch_operator_detail {

/// `sysio.swap` identifiers the yield-tick crank touches.
namespace swap {
   constexpr auto account          = "sysio.swap";
   constexpr auto table_stat       = "stat";
   constexpr auto table_reservoirs = "reservoirs";
   constexpr auto action_tickyield = "tickyield";
   namespace field {
      constexpr auto supply                 = "supply";
      constexpr auto yield_leg              = "yield_leg";
      constexpr auto conversion_horizon_sec = "conversion_horizon_sec";
      constexpr auto depth_cap_bps          = "depth_cap_bps";
      constexpr auto clip_floor             = "clip_floor";
      constexpr auto balance                = "balance";
      constexpr auto quantity               = "quantity";
      constexpr auto symbol_code            = "symbol_code";   ///< the kv key of `stat` / `reservoirs`
      constexpr auto pair_token             = "pair_token";    ///< `tickyield`'s argument
   }
}

/// `sysio.liq` identifiers the pending-yield crank touches.
namespace liq {
   constexpr auto account           = "sysio.liq";
   constexpr auto table_liqpending  = "liqpending";
   constexpr auto action_queueyield = "queueyield";
   namespace field {
      constexpr auto quantity    = "quantity";
      constexpr auto symbol_code = "symbol_code";   ///< the kv key of `liqpending`
      constexpr auto sym         = "sym";           ///< `queueyield`'s argument
   }
}

/// The kv row wrapper `get_table_rows` returns when `values_only` is off.
namespace kv_row {
   constexpr auto key   = "key";
   constexpr auto value = "value";
}

/// The amount of the ABI-rendered asset (`"1.000000000 SYM"`) under `field`; 0
/// when the field is absent or does not parse -- a crank decides from what it can
/// read and pushes nothing on a row it cannot.
inline int64_t asset_amount(const fc::variant_object& row, const char* field) {
   auto it = row.find(field);
   if (it == row.end() || !it->value().is_string()) return 0;
   try {
      return chain::asset::from_string(it->value().as_string()).get_amount();
   } catch (const fc::exception&) {
      return 0;
   }
}

/// The kv key of a `stat` / `reservoirs` / `liqpending` row (the `{key, value}`
/// wrapper): the pool or shadow symbol code, or nullopt when the row carries none.
inline std::optional<chain::symbol_code> row_symbol_code(const fc::variant_object& row) {
   auto key = row.find(kv_row::key);
   if (key == row.end() || !key->value().is_object()) return std::nullopt;
   const auto& key_obj = key->value().get_object();
   auto code = key_obj.find(swap::field::symbol_code);
   if (code == key_obj.end()) return std::nullopt;
   return chain::symbol_code{ code->value().as_uint64() };
}

/// The name of a symbol code -- the spelling `tickyield` / `queueyield` take and
/// the logs use.
inline std::string symbol_code_name(chain::symbol_code code) {
   return chain::symbol(code.value << 8).name();
}

/// The `value` of a `{key, value}` row, or nullopt when the row carries none.
inline std::optional<fc::variant_object> row_value(const fc::variant_object& row) {
   auto value = row.find(kv_row::value);
   if (value == row.end() || !value->value().is_object()) return std::nullopt;
   return value->value().get_object();
}

/// True iff a `sysio.swap::stat` row is a yield pool `tickyield` would accept: a
/// shadow `yield_leg` plus all three tick parameters set -- the exact preconditions
/// the action `check()`s, so a crank never pays for a push the contract is
/// guaranteed to reject.
inline bool is_tickable_pool(const fc::variant_object& stat_row) {
   auto positive = [&](const char* field) {
      auto it = stat_row.find(field);
      return it != stat_row.end() && it->value().as_int64() > 0;
   };
   auto leg = stat_row.find(swap::field::yield_leg);
   return leg != stat_row.end() && !leg->value().is_null() &&
          positive(swap::field::conversion_horizon_sec) &&
          positive(swap::field::depth_cap_bps) &&
          positive(swap::field::clip_floor);
}

/// One push per `interval` per key from this operator. `tickyield` is harmless to
/// crank every block, but every ACTIVE operator polls every few seconds and a
/// reservoir drains over a horizon of hours, so without spacing the whole group
/// would push a tick per pool per poll for the length of the sale.
class crank_spacing {
public:
   bool due(const std::string& key, fc::time_point now, fc::microseconds interval) const {
      auto it = _last_push.find(key);
      return it == _last_push.end() || now - it->second >= interval;
   }
   void mark(const std::string& key, fc::time_point now) { _last_push[key] = now; }

private:
   std::map<std::string, fc::time_point> _last_push;
};

} // namespace sysio::batch_operator_detail
