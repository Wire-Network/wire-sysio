#pragma once
/** @file kicker_tester.hpp
 * @brief Shared real-contract fixture for deterministic and differential kicker tests.
 */
#include "sysio_liq_tester.hpp"
#include <sysio.kicker/kicker_math.hpp>
#include "kicker_test_reference.hpp"
namespace kicker_test_support {
namespace math = sysio::kicker_math;
using kicker_test_reference::wide;
using kicker_test_reference::reference;
/// Real LIQ/swap/kicker plus emissions and epoch contracts; treasury state can be stressed explicitly.
class kicker_tester : public sysio_liq_tester {
public:
   static constexpr auto KICKER = "sysio.kicker"_n;
   static constexpr auto EPOCH = "sysio.epoch"_n;
   static constexpr uint64_t seed = 75'000 * UNIT;
   static constexpr uint64_t holder = 25'000 * UNIT;
   static constexpr uint64_t budget = 1'000'000 * UNIT;
   abi_serializer kicker_ser, system_ser, epoch_ser;

   /// Bootstrap the real reserve-owning contracts, then start one token's accrual clock.
   kicker_tester() {
      BOOST_REQUIRE_EQUAL(success(), regliqpool(SOLANA, LIQSOL, POOL_SYM, seed, seed));
      BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, holder));
      create_accounts({KICKER, EPOCH, "sysio.andon"_n, "sysio.opreg"_n, "sysio.chalg"_n});
      deploy(KICKER, contracts::kicker_wasm(), contracts::kicker_abi(), kicker_ser, true);
      deploy(SYSIO_ACCOUNT, contracts::system_wasm(), contracts::system_abi(), system_ser, true);
      deploy(EPOCH, contracts::epoch_wasm(), contracts::epoch_abi(), epoch_ser, true);
      BOOST_REQUIRE_EQUAL(success(), push(EPOCH, epoch_ser, EPOCH, "setconfig"_n, mvo()
         ("epoch_duration_sec", 60)("operators_per_epoch", 7)("batch_operator_minimum_active", 21)
         ("batch_op_groups", 3)("epoch_retention_envelope_log_count", 1000)));
      const auto emission = mvo()
         ("t1_allocation", 0)("t2_allocation", 0)("t3_allocation", 0)
         ("t1_duration", 1)("t2_duration", 1)("t3_duration", 1)("min_claimable", 0)
         ("t5_distributable", 375'000'000 * UNIT)("t5_floor", 125'000'000 * UNIT)
         ("target_annual_decay_bps", 100)("annual_initial_emission", 1'000'000 * UNIT)
         ("annual_max_emission", 1'000'000 * UNIT)("annual_min_emission", UNIT)
         ("compute_bps", 0)("capex_bps", 0)("governance_bps", 0)
         ("producer_bps", 7000)("batch_op_bps", 3000)("standby_end_rank", 22)
         ("standby_bps", 0)("epoch_log_retention_count", 1000)("pay_cadence_epochs", 1);
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, system_ser, SYSIO_ACCOUNT, "setemitcfg"_n,
                                          mvo()("cfg", emission)));
      BOOST_REQUIRE_EQUAL(success(), push(SYSIO_ACCOUNT, system_ser, SYSIO_ACCOUNT, "initt5"_n,
                                          mvo()("start_time", time_point_sec(control->pending_block_time()))));
      BOOST_REQUIRE_EQUAL(success(), configure(budget));
      BOOST_REQUIRE_EQUAL(success(), add());
   }
   /// Change the absolute earmark and minimum payment spacing.
   action_result configure(uint64_t remaining, uint32_t interval = 3600, name signer = SYSIO_ACCOUNT) {
      return push(KICKER, kicker_ser, signer, "setconfig"_n,
                  mvo()("cfg", mvo()("budget_remaining", remaining)("min_interval_sec", interval)));
   }
   /// Start a token at the protocol defaults unless the test supplies overrides.
   action_result add(symbol sym = LIQSOL_SYM, uint16_t rate = math::default_rate_bps,
                     uint64_t minimum = math::default_min_gift, uint64_t cap = 0, name signer = SYSIO_ACCOUNT) {
      return push(KICKER, kicker_ser, signer, "addpool"_n,
                  mvo()("sym", sym.to_symbol_code())("rate_bps", rate)("min_gift", minimum)("max_gift_per_day", cap));
   }
   /// Change a pool without closing its current accrual interval.
   action_result change(uint16_t rate, uint64_t minimum, uint64_t cap = 0, symbol sym = LIQSOL_SYM,
                        name signer = SYSIO_ACCOUNT) {
      return push(KICKER, kicker_ser, signer, "setpool"_n,
                  mvo()("sym", sym.to_symbol_code())("rate_bps", rate)("min_gift", minimum)("max_gift_per_day", cap));
   }
   /// Invoke a permissionless keeper action as an ordinary holder.
   action_result kick(symbol sym = LIQSOL_SYM) {
      return push(KICKER, kicker_ser, "bob"_n, "kick"_n, mvo()("sym", sym.to_symbol_code()));
   }
   /// Decode the per-symbol payment clock and diagnostics.
   fc::variant pool(symbol sym = LIQSOL_SYM) {
      return decode(kicker_ser, "kick_pool", get_row_by_id(KICKER, KICKER, "kickpools"_n, sym.to_symbol_code().value));
   }
   /// Raw row bytes used to verify that minimum/interval skips write nothing.
   std::vector<char> bytes() { return get_row_by_id(KICKER, KICKER, "kickpools"_n, LIQSOL_SYM.to_symbol_code().value); }
   /// Paid amount (fixture budgets bound this well below uint64).
   uint64_t paid(symbol sym = LIQSOL_SYM) { return static_cast<uint64_t>(pool(sym)["gifted_total"].as_uint128()); }
   /// Exact binary pool state; ABI JSON time formatting loses microsecond precision.
   struct pool_state {
      uint64_t sym = 0, minimum = 0, shortfall = 0, cap = 0, day = 0;
      uint16_t rate = 0;
      time_point clock, observed;
      fc::uint128_t total = 0, spent = 0;
      bool overflow = false;
   };
   /// Decode every pool field once for clock and differential-state consumers.
   pool_state decode_pool(symbol sym = LIQSOL_SYM) {
      const auto data = get_row_by_id(KICKER, KICKER, "kickpools"_n, sym.to_symbol_code().value);
      fc::datastream<const char*> stream(data.data(), data.size());
      pool_state row;
      fc::raw::unpack(stream, row.sym);
      fc::raw::unpack(stream, row.rate);
      fc::raw::unpack(stream, row.minimum);
      fc::raw::unpack(stream, row.clock);
      fc::raw::unpack(stream, row.total);
      fc::raw::unpack(stream, row.shortfall);
      fc::raw::unpack(stream, row.observed);
      fc::raw::unpack(stream, row.overflow);
      fc::raw::unpack(stream, row.cap);
      fc::raw::unpack(stream, row.day);
      fc::raw::unpack(stream, row.spent);
      return row;
   }
   /// Read the serialized clock without JSON precision loss.
   time_point kick_clock(symbol sym = LIQSOL_SYM) { return decode_pool(sym).clock; }
   /// Elapsed microseconds for the next action's block timestamp.
   uint64_t elapsed(symbol sym = LIQSOL_SYM) {
      return (control->pending_block_time() - kick_clock(sym)).count();
   }
   /// Expected next payment uses the current supply and live pool, never an accumulator.
   uint64_t expected(symbol sym = LIQSOL_SYM) {
      const auto row = stat_row(sym);
      const auto pair_code = symbol::from_string("9," + row["pair_symbol"].as_string());
      const auto pair = swap_pool_row(pair_code);
      return reference(row["supply"].as<asset>().get_amount(), pool(sym)["rate_bps"].as_uint64(), elapsed(sym),
                       pair["pool2"]["quantity"].as<asset>().get_amount(),
                       pair["pool1"]["quantity"].as<asset>().get_amount());
   }
   /// Advance chain time in one block, without waiting on the host clock.
   void wait(uint64_t seconds) { produce_block(fc::seconds(seconds)); }
   /// Read a reserve-owning singleton through the deployed system ABI.
   fc::variant treasury(name table, const char* type) {
      return decode(system_ser, type, get_row_by_id(SYSIO_ACCOUNT, SYSIO_ACCOUNT, table, table.value));
   }
   /// Install controlled reserve stress through actual serialized on-chain layouts.
   void write_global(name table, const char* type, const variant_object& value) {
      write_row(SYSIO_ACCOUNT, system_ser, table, table.value, type, value);
   }
   /// Inject serialized fixture stress state into a KV row without using contract arithmetic.
   void write_row(name code, abi_serializer& ser, name table, uint64_t id, const char* type,
                  const variant_object& value) {
      const auto encoded = ser.variant_to_binary(type, value,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      char key[chain::kv_pri_key_size];
      chain::kv_encode_be64(key, id);
      auto& db = const_cast<chainbase::database&>(control->db());
      const auto& index = db.get_index<chain::kv_index, chain::by_code_key>();
      const auto it = index.find(boost::make_tuple(code, chain::compute_table_id(table.value),
                                                   std::string_view(key, sizeof(key))));
      if (it != index.end()) db.modify(*it, [&](auto& row) { row.value.assign(encoded.data(), encoded.size()); });
      else db.create<chain::kv_object>([&](auto& row) {
         row.code = code;
         row.payer = SYSIO_ACCOUNT;
         row.table_id = chain::compute_table_id(table.value);
         row.key.assign(key, sizeof(key));
         row.value.assign(encoded.data(), encoded.size());
      });
   }
   /// Add nonzero pending emissions and claims for reserve and gate tests.
   void reserves(uint64_t pending, uint64_t claims) {
      auto state = mvo(treasury("t5state"_n, "t5_state").get_object());
      state("pending_emission_amount", pending);
      write_global("t5state"_n, "t5_state", state);
      write_global("payclaimtot"_n, "pay_claim_total", mvo()("outstanding", claims));
   }
};
} // namespace kicker_test_support
