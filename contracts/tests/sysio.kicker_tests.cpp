#include "sysio_liq_tester.hpp"
#include <sysio.kicker/kicker_math.hpp>
#include "kicker_test_reference.hpp"

namespace {
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
   /// Read the serialized clock directly: JSON time_point formatting loses sub-millisecond precision.
   time_point kick_clock(symbol sym = LIQSOL_SYM) {
      const auto data = get_row_by_id(KICKER, KICKER, "kickpools"_n, sym.to_symbol_code().value);
      fc::datastream<const char*> stream(data.data(), data.size());
      uint64_t symbol_code, minimum;
      uint16_t rate;
      time_point clock;
      fc::raw::unpack(stream, symbol_code);
      fc::raw::unpack(stream, rate);
      fc::raw::unpack(stream, minimum);
      fc::raw::unpack(stream, clock);
      return clock;
   }
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
      const auto encoded = system_ser.variant_to_binary(type, value,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      char key[chain::kv_pri_key_size];
      chain::kv_encode_be64(key, table.value);
      auto& db = const_cast<chainbase::database&>(control->db());
      const auto& index = db.get_index<chain::kv_index, chain::by_code_key>();
      const auto it = index.find(boost::make_tuple(SYSIO_ACCOUNT, chain::compute_table_id(table.value),
                                                   std::string_view(key, sizeof(key))));
      if (it != index.end()) db.modify(*it, [&](auto& row) { row.value.assign(encoded.data(), encoded.size()); });
      else db.create<chain::kv_object>([&](auto& row) {
         row.code = SYSIO_ACCOUNT;
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
} // namespace

BOOST_AUTO_TEST_SUITE(sysio_kicker_tests)

/// Below-minimum calls leave row bytes untouched; the eventual payment covers the full open span.
BOOST_FIXTURE_TEST_CASE(skip_then_pay_and_pro_rata_claim, kicker_tester) {
   const auto initial = bytes();
   const auto treasury_before = wire_balance(SYSIO_ACCOUNT);
   wait(3600);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   BOOST_CHECK_EQUAL(treasury_before, wire_balance(SYSIO_ACCOUNT));
   wait(math::day_sec);
   const auto gift = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
   BOOST_CHECK_EQUAL(treasury_before - gift, wire_balance(SYSIO_ACCOUNT));
   BOOST_CHECK_EQUAL(0, wire_balance(KICKER));
   BOOST_CHECK_EQUAL(gift, pot());
   BOOST_CHECK_LE(owed("alice"_n), gift / 4);
   const auto distribution = yield_reference::distribute(gift, supply(), 0);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(distribution.carry), index_row()["carry"].as_uint64());
   BOOST_CHECK_EQUAL(owed("alice"_n), static_cast<uint64_t>(
      yield_reference::wide(holder) * distribution.index_delta / yield_reference::Scale));
   BOOST_CHECK_LE(gift / 4 - owed("alice"_n), holder / yield_reference::Scale + 1);
   const auto before = wire_balance("alice"_n);
   const auto claimable = owed("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n));
   BOOST_CHECK_EQUAL(before + claimable, wire_balance("alice"_n));
}

/// Integer floors remain the only frequency dependence at a fixed principal and spot price.
BOOST_AUTO_TEST_CASE(call_pattern_independence) {
   constexpr uint64_t supply = 100'000'000'000'000;
   constexpr uint64_t day = math::day_sec * math::micros_per_sec;
   const auto once = math::gift(supply, 200, 7 * day, 1, 1).value();
   const auto daily = 7 * math::gift(supply, 200, day, 1, 1).value();
   BOOST_CHECK_GE(once, daily);
   BOOST_CHECK_LE(once - daily, 7);
   kicker_tester frequent, single;
   BOOST_REQUIRE_EQUAL(frequent.success(), frequent.change(200, 1));
   BOOST_REQUIRE_EQUAL(single.success(), single.change(200, 1));
   const auto start_f = frequent.kick_clock();
   const auto start_s = single.kick_clock();
   for (unsigned i = 0; i < 7; ++i) {
      frequent.wait(math::day_sec);
      BOOST_REQUIRE_EQUAL(frequent.success(), frequent.kick());
   }
   const auto span = frequent.kick_clock() - start_f;
   const auto wait_span = span - (single.control->pending_block_time() - start_s);
   single.produce_block(wait_span);
   BOOST_REQUIRE_EQUAL(single.success(), single.kick());
   // One block-slot timestamp offset is explicitly priced in the independent reference.
   const auto expected_single = reference(100'000'000'000'000, 200,
      (single.kick_clock() - start_s).count(), 1, 1);
   BOOST_CHECK_EQUAL(expected_single, single.paid());
   const auto expected_frequent = reference(100'000'000'000'000, 200, span.count(), 1, 1);
   BOOST_CHECK_LE(expected_frequent - frequent.paid(), 7);
}

/// A swap just before kick reprices the entire unpaid span at spot, as governance ruled.
BOOST_FIXTURE_TEST_CASE(spot_after_trade_and_current_supply, kicker_tester) {
   wait(math::day_sec);
   const auto old_gift = expected();
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{WIRE_SYM, TOKEN_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{LIQSOL_SYM, LIQ_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "bob"_n, seed));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("bob"_n, SWAP_ACCOUNT, seed));
   BOOST_REQUIRE_EQUAL(success(), exchange("bob"_n, POOL_SYM, extended_asset{asset(seed, WIRE_SYM), TOKEN_ACCOUNT},
                                           asset(1, LIQSOL_SYM)));
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, holder));
   const auto spot_gift = expected();
   BOOST_REQUIRE_GT(spot_gift, 3 * old_gift);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(spot_gift, paid());
}

/// Funding shortages record the latest deficit without losing the open interval; funding later pays all.
BOOST_FIXTURE_TEST_CASE(budget_shortfall_then_refund, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), configure(1));
   wait(math::day_sec);
   const auto clock = kick_clock();
   const auto gift = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK(clock == kick_clock());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   const auto catchup = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(catchup, paid());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
}

/// Reserve the next pay epoch as well as pending and claims; the old guard would strand advance.
BOOST_FIXTURE_TEST_CASE(reserve_guard_and_emissions_gate, kicker_tester) {
   wait(86'400);
   constexpr uint64_t pending = 100 * UNIT, claims = 50 * UNIT, ceiling = 5 * UNIT, available = UNIT;
   auto cfg = mvo(treasury("emitcfg"_n, "emission_config").get_object());
   cfg("t5_distributable", 125'000'000 * UNIT + ceiling);
   write_global("emitcfg"_n, "emission_config", cfg);
   reserves(pending, claims);
   const auto balance = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "carol"_n,
                                                balance - pending - claims - ceiling - available));
   const auto gift = expected();
   // The old reserve allows this entire available gift, leaving less than the next emission.
   BOOST_REQUIRE_LT(gift, ceiling + available);
   BOOST_REQUIRE_GT(gift, available);
   // Independent control applies the old permitted transfer and observes the fatal readiness block.
   kicker_tester old;
   old.wait(86'400);
   auto old_cfg = mvo(old.treasury("emitcfg"_n, "emission_config").get_object());
   old_cfg("t5_distributable", 125'000'000 * UNIT + ceiling);
   old.write_global("emitcfg"_n, "emission_config", old_cfg);
   old.reserves(pending, claims);
   const auto old_gift = old.expected();
   const auto old_balance = old.wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(old.success(), old.transfer_wire(SYSIO_ACCOUNT, "carol"_n,
      old_balance - pending - claims - ceiling - available + old_gift));
   BOOST_REQUIRE_EQUAL(old.success(), old.push(EPOCH, old.epoch_ser, EPOCH, "advance"_n, mvo()));
   const auto blocked = old.decode(old.epoch_ser, "epoch_state",
      old.get_row_by_id(EPOCH, EPOCH, "epochstate"_n, "epochstate"_n.value));
   BOOST_CHECK(blocked.is_null());
   const auto log = old.decode(old.epoch_ser, "blocklog_entry",
      old.get_row_by_id(EPOCH, EPOCH, "blocklog"_n, 1));
   EmissionsBlockReason reason;
   BOOST_REQUIRE(EmissionsBlockReason_Parse(log["reason"].as_string(), &reason));
   BOOST_CHECK_EQUAL(EMISSIONS_BLOCK_REASON_BALANCE_INSUFFICIENT, reason);
   const auto clock = kick_clock();
   const auto span = elapsed();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(available, paid());
   BOOST_CHECK_EQUAL(pending + claims + ceiling, wire_balance(SYSIO_ACCOUNT));
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(wide(span) * available / gift),
      (kick_clock() - clock).count());
   BOOST_REQUIRE_EQUAL(success(), push(EPOCH, epoch_ser, EPOCH, "advance"_n, mvo()));
   const auto epoch = decode(epoch_ser, "epoch_state",
      get_row_by_id(EPOCH, EPOCH, "epochstate"_n, "epochstate"_n.value));
   BOOST_CHECK_EQUAL(1, epoch["current_epoch_index"].as_uint64());
   BOOST_CHECK_EQUAL(0, treasury("t5state"_n, "t5_state")["pending_emission_amount"].as_int64());
}

/// Wide reserves fail closed; an exhausted ceiling clamps at zero without arithmetic wrap.
BOOST_FIXTURE_TEST_CASE(reserve_bounds_and_exhausted_ceiling, kicker_tester) {
   wait(86'400);
   reserves(math::max_amount, std::numeric_limits<uint64_t>::max());
   const auto clock = kick_clock();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK(kick_clock() == clock);
   auto state = mvo(treasury("t5state"_n, "t5_state").get_object());
   state("pending_emission_amount", 0)("total_distributed", 250'000'001 * UNIT);
   write_global("t5state"_n, "t5_state", state);
   write_global("payclaimtot"_n, "pay_claim_total", mvo()("outstanding", 0));
   const auto balance = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "carol"_n, balance - 2 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(2 * UNIT, paid());
   BOOST_CHECK_EQUAL(0, wire_balance(SYSIO_ACCOUNT));
}

/// Andon holds every clock and the first clear payment includes the stopped span.
BOOST_FIXTURE_TEST_CASE(andon_hold_then_pay, kicker_tester) {
   namespace cord = sysio_system::test_support::andon;
   abi_serializer ser;
   cord::deploy(*this, ser, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), cord::pull(*this, ser));
   const auto initial = bytes();
   wait(2 * math::day_sec);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   BOOST_REQUIRE_EQUAL(success(), cord::clear(*this, ser));
   const auto gift = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
}

/// Ordinary holders can kick, while all four governance actions require sysio.
BOOST_FIXTURE_TEST_CASE(auth_validation_and_rmpool, kicker_tester) {
   BOOST_CHECK(mentions(configure(budget, 3600, "alice"_n), "missing authority of sysio"));
   BOOST_CHECK(mentions(add(LIQSOL_SYM, 200, UNIT, 0, "alice"_n), "missing authority of sysio"));
   BOOST_CHECK(mentions(change(200, UNIT, 0, LIQSOL_SYM, "alice"_n), "missing authority of sysio"));
   BOOST_CHECK(mentions(push(KICKER, kicker_ser, "alice"_n, "rmpool"_n,
                            mvo()("sym", LIQSOL_SYM.to_symbol_code())), "missing authority of sysio"));
   BOOST_CHECK(mentions(add(), "already configured"));
   BOOST_CHECK(mentions(change(10001, UNIT), "rate exceeds"));
   BOOST_CHECK(mentions(change(200, 0), "invalid minimum"));
   BOOST_CHECK(mentions(change(200, UNIT, UNIT - 1), "invalid daily cap"));
   BOOST_CHECK(mentions(configure(budget, 0), "interval must be positive"));
   BOOST_CHECK(mentions(configure(math::max_amount + 1), "budget exceeds"));
   BOOST_REQUIRE_EQUAL(success(), push(KICKER, kicker_ser, SYSIO_ACCOUNT, "rmpool"_n,
                                       mvo()("sym", LIQSOL_SYM.to_symbol_code())));
   BOOST_CHECK(pool().is_null());
   BOOST_CHECK(mentions(kick(), "unknown kicker pool"));
   wait(math::day_sec);
   BOOST_REQUIRE_EQUAL(success(), add());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK_LT(elapsed(), 3600 * math::micros_per_sec);
}

/// A second transaction in the same block and calls before min_interval make no second gift.
BOOST_FIXTURE_TEST_CASE(min_interval_and_same_block, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   const auto initial = bytes();
   wait(60);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   wait(3600);
   base_tester::push_action(KICKER, "kick"_n, "alice"_n, mvo()("sym", LIQSOL_SYM.to_symbol_code()));
   const auto first = bytes();
   base_tester::push_action(KICKER, "kick"_n, "bob"_n, mvo()("sym", LIQSOL_SYM.to_symbol_code()));
   BOOST_CHECK(bytes() == first);
   produce_block();
}

/// Equality with the minimum pays; a new rate applies retrospectively to the still-open interval.
BOOST_FIXTURE_TEST_CASE(minimum_boundary_and_rate_change, kicker_tester) {
   wait(math::day_sec);
   const auto old_clock = kick_clock();
   BOOST_REQUIRE_EQUAL(success(), change(400, 1));
   BOOST_CHECK(old_clock == kick_clock());
   // setpool itself produces one block; compute the boundary for the next kick's timestamp.
   const auto future_elapsed = elapsed() + config::block_interval_us;
   const auto boundary = reference(supply(), 400, future_elapsed, 1, 1);
   BOOST_REQUIRE_EQUAL(success(), change(400, boundary));
   BOOST_REQUIRE_EQUAL(boundary, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(boundary, paid());
}

/// A three-day outage pays the unchanged daily cap, retaining a floored pro-rata remainder.
BOOST_FIXTURE_TEST_CASE(daily_cap_partial_catchup, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), change(200, 1, 10 * UNIT));
   // Align the first payment to UTC noon so the one-hour retry cannot cross a day boundary.
   const auto utc_seconds = control->pending_block_time().time_since_epoch().count() / math::micros_per_sec;
   wait(3 * math::day_sec + (math::day_sec + math::day_sec / 2 - utc_seconds % math::day_sec) % math::day_sec);
   const auto clock = kick_clock();
   const auto span = elapsed();
   const auto gift = expected();
   const auto now = control->pending_block_time();
   BOOST_REQUIRE_GT(gift, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(10 * UNIT, paid());
   BOOST_CHECK_EQUAL(gift - 10 * UNIT, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK(pool()["shortfall_time"].as<time_point>() == now);
   BOOST_CHECK_EQUAL(10 * UNIT, pool()["spent_today"].as_uint128());
   const auto payment_day = now.time_since_epoch().count() / (math::day_sec * math::micros_per_sec);
   BOOST_CHECK_EQUAL(payment_day, pool()["day"].as_uint64());
   const auto advanced = (kick_clock() - clock).count();
   const wide numerator = wide(span) * (10 * UNIT);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(numerator / gift), advanced);
   BOOST_CHECK(numerator % gift != 0);
   wait(3600);
   const auto open_gift = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(10 * UNIT, paid());
   BOOST_CHECK_EQUAL(open_gift, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(10 * UNIT, pool()["spent_today"].as_uint128());
   BOOST_CHECK_EQUAL(payment_day, pool()["day"].as_uint64());
   for (unsigned day = 0; day < 4; ++day) {
      const auto before = paid();
      const auto seconds = control->pending_block_time().time_since_epoch().count() / math::micros_per_sec;
      wait(day == 0 ? math::day_sec - seconds % math::day_sec : math::day_sec);
      BOOST_REQUIRE_EQUAL(success(), kick());
      BOOST_CHECK_GT(paid(), before);
      BOOST_CHECK_LE(paid() - before, 10 * UNIT);
   }
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_time"].as<time_point>().time_since_epoch().count());
   BOOST_CHECK(!pool()["gift_overflow"].as_bool());
}

/// Missing emission configuration fails closed even when T5 state and a payable gift exist.
BOOST_FIXTURE_TEST_CASE(missing_emitcfg_fails_closed, kicker_tester) {
   // Advance before removing the row: a skipped block aborts pending state, including direct fixture edits.
   wait(math::day_sec);
   constexpr auto table = "emitcfg"_n;
   char key[chain::kv_pri_key_size];
   chain::kv_encode_be64(key, table.value);
   auto& db = const_cast<chainbase::database&>(control->db());
   const auto& index = db.get_index<chain::kv_index, chain::by_code_key>();
   const auto it = index.find(boost::make_tuple(SYSIO_ACCOUNT, chain::compute_table_id(table.value),
                                               std::string_view(key, sizeof(key))));
   BOOST_REQUIRE(it != index.end());
   db.remove(*it);
   BOOST_REQUIRE(treasury(table, "emission_config").is_null());
   BOOST_REQUIRE(!treasury("t5state"_n, "t5_state").is_null());
   const auto gift = expected();
   BOOST_REQUIRE_GE(gift, pool()["min_gift"].as_uint64());
   const auto clock = kick_clock();
   const auto balance = wire_balance(SYSIO_ACCOUNT);
   const auto yield_before = pot();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK_EQUAL(balance, wire_balance(SYSIO_ACCOUNT));
   BOOST_CHECK_EQUAL(0, wire_balance(KICKER));
   BOOST_CHECK_EQUAL(yield_before, pot());
   BOOST_CHECK(clock == kick_clock());
   BOOST_CHECK_EQUAL(gift, pool()["shortfall_amount"].as_uint64());
}

/// A budget shortage pays what it can and advances exactly the floored paid fraction.
BOOST_FIXTURE_TEST_CASE(budget_partial_pro_rata, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), configure(2 * UNIT));
   wait(86'400);
   const auto clock = kick_clock();
   const auto span = elapsed();
   const auto gift = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(2 * UNIT, paid());
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(wide(span) * (2 * UNIT) / gift),
      (kick_clock() - clock).count());
   BOOST_CHECK_EQUAL(gift - 2 * UNIT, pool()["shortfall_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   wait(3600);
   const auto remaining = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(2 * UNIT + remaining, paid());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
}

/// Literal Julian-year oracle is independent of the accrual header constants.
BOOST_FIXTURE_TEST_CASE(literal_year_payment, kicker_tester) {
   const auto start = kick_clock();
   produce_block(fc::microseconds(31'557'600'000'000LL - elapsed()));
   BOOST_REQUIRE_EQUAL(31'557'600'000'000LL, elapsed());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(2'000'000'000'000LL, paid());
   BOOST_CHECK_EQUAL(31'557'600'000'000LL, (kick_clock() - start).count());
}

/// Each new LIQ token is configured through existing registries and addpool; clocks and rates are independent.
BOOST_FIXTURE_TEST_CASE(per_token_rates_and_third_token, kicker_tester) {
   const auto third = symbol::from_string("9,LIQALT");
   const auto eth_pool = symbol::from_string("9,ETHPOOL");
   const auto third_pool = symbol::from_string("9,ALTPOOL");
   BOOST_REQUIRE_EQUAL(success(), create(LIQETH_SYM, ETH, LIQETH));
   BOOST_CHECK(mentions(add(LIQETH_SYM), "no yield pool"));
   BOOST_REQUIRE_EQUAL(success(), regliqpool(ETH, LIQETH, eth_pool, seed, seed));
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, holder, LIQETH));
   BOOST_REQUIRE_EQUAL(success(), add(LIQETH_SYM, 300));
   BOOST_REQUIRE_EQUAL(success(), regtoken(TokenKind::TOKEN_KIND_LIQ, "LIQALT", ChainKind::CHAIN_KIND_EVM, ETH));
   BOOST_REQUIRE_EQUAL(success(), create(third, ETH, "LIQALT"));
   BOOST_REQUIRE_EQUAL(success(), regliqpool(ETH, "LIQALT", third_pool, seed, seed));
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, holder, "LIQALT"));
   BOOST_REQUIRE_EQUAL(success(), add(third, 400));
   const auto sol_clock = kick_clock();
   const auto eth_clock = kick_clock(LIQETH_SYM);
   wait(math::day_sec);
   const auto third_gift = expected(third);
   BOOST_REQUIRE_EQUAL(success(), kick(third));
   BOOST_CHECK_EQUAL(third_gift, paid(third));
   BOOST_CHECK(sol_clock == kick_clock());
   BOOST_CHECK(eth_clock == kick_clock(LIQETH_SYM));
   for (const auto sym : {LIQSOL_SYM, LIQETH_SYM}) {
      const auto gift = expected(sym);
      BOOST_REQUIRE_EQUAL(success(), kick(sym));
      BOOST_CHECK_EQUAL(gift, paid(sym));
   }
   BOOST_CHECK_GT(paid(third), paid(LIQETH_SYM));
   BOOST_CHECK_GT(paid(LIQETH_SYM), paid());
}

/// The real action never wraps an asset-sized gift; it records overflow and leaves the debt window open.
BOOST_FIXTURE_TEST_CASE(action_overflow_holds_clock, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, math::max_amount - supply()));
   BOOST_REQUIRE_EQUAL(success(), change(10000, 1));
   wait(2 * math::year_sec);
   const auto clock = kick_clock();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(pool()["gift_overflow"].as_bool());
   BOOST_CHECK(clock == kick_clock());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_REQUIRE_EQUAL(success(), change(0, 1));
   BOOST_REQUIRE_EQUAL(success(), kick());
   // Below-minimum kick writes nothing: rate 0 intentionally does not clear the prior overflow flag.
   BOOST_CHECK(pool()["gift_overflow"].as_bool());
   BOOST_REQUIRE_EQUAL(success(), change(1, 1));
   // Lower supply enough to make a later full payment representable and funded.
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SYND_ACCOUNT, math::max_amount - seed));
   BOOST_REQUIRE_EQUAL(success(), push_liq(SYND_ACCOUNT, "burn"_n,
      mvo()("token_code", codename_mvo(LIQSOL))("amount", math::max_amount - seed)));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_GT(paid(), 0);
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK(!pool()["gift_overflow"].as_bool());
}

/// Zero supply clears an actual overflow observation as well as its timestamp.
BOOST_FIXTURE_TEST_CASE(zero_supply_clears_overflow, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, math::max_amount - supply()));
   BOOST_REQUIRE_EQUAL(success(), change(10000, 1));
   wait(2 * 31'557'600);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_REQUIRE(pool()["gift_overflow"].as_bool());
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SYND_ACCOUNT, math::max_amount - seed));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow(SWAP_ACCOUNT, SYND_ACCOUNT, seed));
   BOOST_REQUIRE_EQUAL(success(), push_liq(SYND_ACCOUNT, "burn"_n,
      mvo()("token_code", codename_mvo(LIQSOL))("amount", math::max_amount)));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_time"].as<time_point>().time_since_epoch().count());
   BOOST_CHECK(!pool()["gift_overflow"].as_bool());
}

/// Empty supply advances only its own clock; no inline addyield can be emitted without holders.
BOOST_FIXTURE_TEST_CASE(zero_supply_resets_clock_without_payment, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), configure(1));
   wait(86'400);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_REQUIRE_GT(pool()["shortfall_amount"].as_uint64(), 0);
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow("alice"_n, SYND_ACCOUNT, holder));
   BOOST_REQUIRE_EQUAL(success(), transfer_shadow(SWAP_ACCOUNT, SYND_ACCOUNT, seed));
   BOOST_REQUIRE_EQUAL(success(), push_liq(SYND_ACCOUNT, "burn"_n,
      mvo()("token_code", codename_mvo(LIQSOL))("amount", seed + holder)));
   BOOST_REQUIRE_EQUAL(0, supply());
   wait(math::day_sec);
   const auto before = wire_balance(SYSIO_ACCOUNT);
   const auto clock = kick_clock();
   namespace cord = sysio_system::test_support::andon;
   abi_serializer ser;
   cord::deploy(*this, ser, contracts::andon_wasm(), contracts::andon_abi());
   BOOST_REQUIRE_EQUAL(success(), cord::pull(*this, ser));
   const auto held = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == held);
   BOOST_REQUIRE_EQUAL(success(), cord::clear(*this, ser));
   const auto trace = base_tester::push_action(KICKER, "kick"_n, "bob"_n,
                                              mvo()("sym", LIQSOL_SYM.to_symbol_code()));
   BOOST_REQUIRE(trace && !trace->except);
   BOOST_CHECK(clock < kick_clock());
   BOOST_CHECK_EQUAL(before, wire_balance(SYSIO_ACCOUNT));
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_time"].as<time_point>().time_since_epoch().count());
   BOOST_CHECK(!pool()["gift_overflow"].as_bool());
   BOOST_CHECK_EQUAL(0, paid());
   for (const auto& action : trace->action_traces)
      BOOST_CHECK(action.act.name != "addyield"_n);
   produce_block();
}

/// Budget equality pays exactly; funding bookkeeping and all transfers roll back on inline failure.
BOOST_FIXTURE_TEST_CASE(exact_budget_and_atomic_inline_failure, kicker_tester) {
   wait(math::day_sec);
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   const auto gift = expected();
   // Configure consumes a half-second block; equality is measured at the ensuing kick time.
   const auto exact = reference(supply(), 200, elapsed() + config::block_interval_us, 1, 1);
   BOOST_REQUIRE_EQUAL(success(), configure(exact));
   BOOST_REQUIRE_EQUAL(exact, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(exact, paid());
   const auto cfg = decode(kicker_ser, "kick_config", get_row_by_id(KICKER, KICKER, "kickcfg"_n, "kickcfg"_n.value));
   BOOST_CHECK_EQUAL(0, cfg["budget_remaining"].as_uint64());
   wait(math::day_sec);
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   const auto clock = bytes();
   const auto balance = wire_balance(SYSIO_ACCOUNT);
   base_tester::push_action(SYSIO_ACCOUNT, "setpriv"_n, SYSIO_ACCOUNT,
                           mvo()("account", KICKER)("is_priv", 0));
   const auto failure = kick();
   BOOST_CHECK(failure != success());
   BOOST_CHECK(bytes() == clock);
   BOOST_CHECK_EQUAL(balance, wire_balance(SYSIO_ACCOUNT));
   set_privileged(KICKER);
   // Restore sufficient funding because the failed push produced another block.
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_GT(paid(), gift);
}

/// A quarter holder claims the exact floored quarter gift when the existing yield-index scale divides supply.
BOOST_FIXTURE_TEST_CASE(quarter_holder_claims_floored_gift, kicker_tester) {
   constexpr uint64_t small_seed = 750 * UNIT, small_holder = 250 * UNIT;
   const auto pair = symbol::from_string("9,ETHPOOL");
   BOOST_REQUIRE_EQUAL(success(), create(LIQETH_SYM, ETH, LIQETH));
   BOOST_REQUIRE_EQUAL(success(), regliqpool(ETH, LIQETH, pair, small_seed, small_seed));
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, small_holder, LIQETH));
   BOOST_REQUIRE_EQUAL(success(), add(LIQETH_SYM));
   wait(math::year_sec);
   const auto gift = expected(LIQETH_SYM);
   BOOST_REQUIRE_EQUAL(success(), kick(LIQETH_SYM));
   const auto before = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), claim("alice"_n, LIQETH_SYM));
   BOOST_CHECK_EQUAL(before + gift / 4, wire_balance("alice"_n));
}

/// A deployed but unconfigured kicker gives a stable actionable diagnostic.
BOOST_AUTO_TEST_CASE(missing_config_diagnostic) {
   sysio_liq_tester t;
   t.create_accounts({kicker_tester::KICKER});
   abi_serializer ser;
   t.deploy(kicker_tester::KICKER, contracts::kicker_wasm(), contracts::kicker_abi(), ser, true);
   BOOST_CHECK(t.mentions(t.push(kicker_tester::KICKER, ser, "bob"_n, "kick"_n,
      mvo()("sym", t.LIQSOL_SYM.to_symbol_code())), "kicker not configured"));
}

BOOST_AUTO_TEST_SUITE_END()
