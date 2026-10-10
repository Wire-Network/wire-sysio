#include "kicker_tester.hpp"
#include <cmath>

namespace {
namespace math = sysio::kicker_math;
namespace kicker = kicker_test_support::kicker;
/// Independent expectations pin the governance defaults and validation boundary.
constexpr uint32_t expected_default_twap_window_sec = 86'400;
constexpr uint32_t expected_min_twap_window_sec = 60;
constexpr uint32_t below_min_twap_window_sec = expected_min_twap_window_sec - 1;
constexpr uint32_t disabled_twap_window_sec = 0;
constexpr auto invalid_twap_window_message = "TWAP window must be at least 60 seconds";
constexpr auto invalid_spot_deviation_message = "spot deviation must be 1 to 10000 bps";
using kicker_test_reference::wide;
using kicker_test_reference::reference;
using kicker_test_support::kicker_tester;
/// The review's construction: 100,000 LIQ for exactly one day at price 1, and the LIQ minted just before the kick
/// that would size a current-supply gift at the 1,000 WIRE daily cap.
constexpr int64_t review_supply = 100'000 * sysio_liq_tester::UNIT;
constexpr uint64_t review_mint = 18'162'500 * sysio_liq_tester::UNIT;
/// floor(100,000 LIQ * 200 bps * one day / Julian year), in WIRE subunits: the honest gift of the review's supply.
constexpr uint64_t review_honest_gift = 5'475'701'574;
/// One LIQ subunit: a supply whose gift never reaches the default one-WIRE minimum.
constexpr int64_t tiny_supply = 1;
/// Burn every LIQ the fixture holds except `left` subunits, leaving the swap reserves (and so the price) as they are.
void burn_to(kicker_tester& t, int64_t left) {
   BOOST_REQUIRE_EQUAL(t.success(), t.transfer_shadow("alice"_n, t.SYND_ACCOUNT, t.holder));
   BOOST_REQUIRE_EQUAL(t.success(), t.transfer_shadow(t.SWAP_ACCOUNT, t.SYND_ACCOUNT, t.seed));
   const auto token_code = sysio_system::test_support::codename_mvo(t.LIQSOL);
   BOOST_REQUIRE_EQUAL(t.success(), t.push_liq(t.SYND_ACCOUNT, "burn"_n,
      mvo()("token_code", token_code)("amount", uint64_t(t.seed + t.holder - left))));
   BOOST_REQUIRE_EQUAL(left, t.supply());
}
/// Move the next action to exactly `span_us` after the pool's accrual clock, which must sit on a block boundary (a
/// checkpoint or addpool). It reads the clock through the deployed ABI, so it also runs against an older row layout.
void advance_elapsed(kicker_tester& t, uint64_t span_us) {
   const auto target = t.pool()["last_kick"].as<time_point>() + fc::microseconds(span_us);
   BOOST_REQUIRE(t.control->pending_block_time() < target);
   t.produce_block(target - t.control->pending_block_time());
   BOOST_REQUIRE(t.control->pending_block_time() == target);
}
/// One day of accrual, in microseconds.
constexpr uint64_t day_us = math::day_sec * math::micros_per_sec;
/// The fixture's swap reserves are equal, so every observation prices at exactly 1 WIRE per LIQ.
constexpr uint64_t unit_price = 1;
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

/// Regular daily payments preserve simple accrual; a missed week can recover only 72 hours.
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
   uint64_t expected_frequent = 0;
   for (unsigned i = 0; i < 7; ++i) {
      frequent.wait(math::day_sec);
      expected_frequent += reference(supply, 200, frequent.elapsed(), 1, 1);
      BOOST_REQUIRE_EQUAL(frequent.success(), frequent.kick());
   }
   const auto span = frequent.kick_clock() - start_f;
   const auto wait_span = span - (single.control->pending_block_time() - start_s);
   single.produce_block(wait_span);
   BOOST_REQUIRE_EQUAL(single.success(), single.kick());
   // Daily payments include their block-slot offsets; the missed week is bounded to 72 hours.
   const auto expected_single = reference(supply, 200, 3 * day, 1, 1);
   BOOST_CHECK_EQUAL(expected_single, single.paid());
   BOOST_CHECK_EQUAL(expected_frequent, frequent.paid());
   BOOST_CHECK_GT(frequent.paid(), 2 * single.paid());
}

/// A real trade just before kick has only its elapsed-time weight in the gift, and LIQ minted just before it is not
/// sized at all (the checkpointed supply is the smaller); with spot now far above that TWAP, the spot guard refuses
/// it and holds the clock.
BOOST_FIXTURE_TEST_CASE(twap_after_trade_and_current_supply, kicker_tester) {
   wait(math::day_sec);
   const auto old_gift = expected();
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{WIRE_SYM, TOKEN_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{LIQSOL_SYM, LIQ_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "bob"_n, seed));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("bob"_n, SWAP_ACCOUNT, seed));
   BOOST_REQUIRE_EQUAL(success(), exchange("bob"_n, POOL_SYM, extended_asset{asset(seed, WIRE_SYM), TOKEN_ACCOUNT},
                                           asset(1, LIQSOL_SYM)));
   BOOST_REQUIRE_EQUAL(success(), mint("bob"_n, holder));
   BOOST_REQUIRE_EQUAL(supply() - int64_t(holder), pool()["supply_snapshot"].as_int64());
   const auto twap_gift = expected();
   const auto pair = swap_pool_row(POOL_SYM);
   const auto spot_gift = reference(supply(), 200, math::max_catchup_us,
      pair["pool2"]["quantity"].as<asset>().get_amount(), pair["pool1"]["quantity"].as<asset>().get_amount());
   BOOST_REQUIRE_GT(spot_gift, 3 * old_gift);
   BOOST_CHECK_LT(twap_gift, 2 * old_gift); // Spot nearly quadrupled; the new 25% of supply is not sized yet.
   const auto clock = kick_clock();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK(decode_pool().deviation);
   BOOST_CHECK_EQUAL(twap_gift, decode_pool().shortfall);
   BOOST_CHECK(clock == kick_clock());
}

/// The review's ten-million-WIRE pump contributes zero weight to the TWAP when kick follows in the same block, and
/// the spot guard refuses the kick outright.
BOOST_FIXTURE_TEST_CASE(large_same_block_trade_cannot_amplify_gift, kicker_tester) {
   constexpr uint64_t attack = 10'000'000 * UNIT;
   wait(3 * math::day_sec);
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{WIRE_SYM, TOKEN_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{LIQSOL_SYM, LIQ_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "bob"_n, attack));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("bob"_n, SWAP_ACCOUNT, attack));
   const auto baseline = expected();
   base_tester::push_action(SWAP_ACCOUNT, "exchange"_n, "bob"_n, mvo()
      ("user", "bob"_n)("pair_token", POOL_SYM.to_symbol_code())
      ("ext_asset_in", extended_asset{asset(attack, WIRE_SYM), TOKEN_ACCOUNT})
      ("min_expected", asset(1, LIQSOL_SYM)));
   BOOST_REQUIRE_EQUAL(baseline, expected());
   base_tester::push_action(KICKER, "kick"_n, "bob"_n, mvo()("sym", LIQSOL_SYM.to_symbol_code()));
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK(decode_pool().deviation);
   BOOST_CHECK_EQUAL(baseline, decode_pool().shortfall);
   BOOST_CHECK_EQUAL(reference(supply(), 200, math::max_catchup_us, 1, 1), baseline);
   produce_block();
}

/// Even a tiny minimum and abundant funds cannot bypass the first full day of observation.
BOOST_FIXTURE_TEST_CASE(twap_warmup_is_enforced, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1));
   const auto initial = bytes();
   const auto until_boundary = uint64_t(math::default_twap_window_sec) * math::micros_per_sec -
                               elapsed() - config::block_interval_us;
   produce_block(fc::microseconds(until_boundary));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_GT(paid(), 0);
}

/// Governance defaults to one day, accepts one minute, and rejects every shorter window atomically.
BOOST_FIXTURE_TEST_CASE(twap_window_config_validation, kicker_tester) {
   BOOST_CHECK_EQUAL(expected_default_twap_window_sec, configuration()[kicker::field::twap_window_sec].as_uint64());
   BOOST_CHECK_EQUAL(expected_min_twap_window_sec, math::min_twap_window_sec);
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1, SYSIO_ACCOUNT, math::min_twap_window_sec));
   BOOST_CHECK_EQUAL(expected_min_twap_window_sec, configuration()[kicker::field::twap_window_sec].as_uint64());
   for (const uint32_t window : {below_min_twap_window_sec, disabled_twap_window_sec}) {
      BOOST_CHECK(mentions(configure(budget, 1, SYSIO_ACCOUNT, window), invalid_twap_window_message));
      BOOST_CHECK_EQUAL(expected_min_twap_window_sec, configuration()[kicker::field::twap_window_sec].as_uint64());
   }
}

/// The minimum configured window pays at its inclusive boundary and starts a fresh observation on payment.
BOOST_FIXTURE_TEST_CASE(short_twap_window_pays, kicker_tester) {
   constexpr uint32_t window = math::min_twap_window_sec;
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1, SYSIO_ACCOUNT, window));
   const auto initial = bytes();
   produce_block(fc::microseconds(uint64_t(window) * math::micros_per_sec - elapsed() - config::block_interval_us));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   const auto gift = expected();
   BOOST_REQUIRE_GT(gift, 0);
   const auto now = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
   BOOST_CHECK(pool()["price_time"].as<time_point>() == now);
   const auto after = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == after);
}

/// Lowering the window preserves all existing history and can make an in-flight observation immediately payable.
BOOST_FIXTURE_TEST_CASE(lower_twap_window_keeps_open_observation, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1));
   wait(math::min_twap_window_sec);
   const auto initial = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1, SYSIO_ACCOUNT, math::min_twap_window_sec));
   BOOST_CHECK(bytes() == initial);
   const auto gift = expected();
   BOOST_REQUIRE_GT(gift, 0);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
}

/// Raising the window delays the same in-flight observation, retaining its timestamp, snapshot and accrual clock.
BOOST_FIXTURE_TEST_CASE(raise_twap_window_keeps_open_observation, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1, SYSIO_ACCOUNT, math::min_twap_window_sec));
   wait(math::min_twap_window_sec);
   BOOST_REQUIRE_GT(expected(), 0);
   const auto initial = bytes();
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1));
   BOOST_CHECK(bytes() == initial);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == initial);
   produce_block(fc::microseconds(uint64_t(math::default_twap_window_sec) * math::micros_per_sec - elapsed()));
   const auto gift = expected();
   BOOST_REQUIRE_GT(gift, 0);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
}

/// The default ceiling limits a payable high-supply pool even with ample treasury and budget.
BOOST_FIXTURE_TEST_CASE(default_daily_cap_is_enforced, kicker_tester) {
   constexpr uint64_t extra_supply = 25'000'000 * UNIT;
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, extra_supply));
   reopen();   // checkpoint the minted supply; minted after the checkpoint, it would size nothing until the next one
   wait(math::day_sec);
   BOOST_REQUIRE_GT(expected(), math::max_daily_gift);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(math::max_daily_gift, paid());
   BOOST_CHECK_EQUAL(math::max_daily_gift, pool()["spent_today"].as_uint128());
   const auto before = paid();
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(before, paid());
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
   cfg("t5_distributable", 125'000'000 * UNIT + pending + ceiling);
   write_global("emitcfg"_n, "emission_config", cfg);
   reserves(pending, claims);
   // The gate sizes the share on net headroom, and pending sits inside the gross headroom, so it is held twice.
   const auto balance = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "carol"_n,
                                                balance - 2 * pending - claims - ceiling - available));
   const auto gift = expected();
   // The old reserve allows this entire available gift, leaving less than the next emission.
   BOOST_REQUIRE_LT(gift, ceiling + available);
   BOOST_REQUIRE_GT(gift, available);
   // Independent control applies the old permitted transfer and observes the fatal readiness block.
   kicker_tester old;
   old.wait(86'400);
   auto old_cfg = mvo(old.treasury("emitcfg"_n, "emission_config").get_object());
   old_cfg("t5_distributable", 125'000'000 * UNIT + pending + ceiling);
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
   const auto span = std::min(elapsed(), math::max_catchup_us);
   const auto base = control->pending_block_time() - fc::microseconds(span);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(available, paid());
   BOOST_CHECK_EQUAL(2 * pending + claims + ceiling, wire_balance(SYSIO_ACCOUNT));
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(wide(span) * available / gift),
      (kick_clock() - base).count());
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

/// Andon holds every clock; clearing pays only the bounded stopped span.
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
   BOOST_CHECK(mentions(change(200, UNIT, 0), "invalid daily cap"));
   BOOST_CHECK(mentions(add(LIQETH_SYM, 200, UNIT, 0), "invalid daily cap"));
   BOOST_CHECK(mentions(change(200, UNIT, math::max_daily_gift + 1), "invalid daily cap"));
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
   wait(math::day_sec);
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
   const auto future_elapsed = std::min(elapsed() + config::block_interval_us, math::max_catchup_us);
   const auto boundary = reference(supply(), 400, future_elapsed, 1, 1);
   BOOST_REQUIRE_EQUAL(success(), change(400, boundary));
   BOOST_REQUIRE_EQUAL(boundary, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(boundary, paid());
}

/// A three-day outage pays the daily cap within bounded 72-hour accrual, retaining its floored pro-rata remainder
/// for later observations.
BOOST_FIXTURE_TEST_CASE(daily_cap_partial_catchup, kicker_tester) {
   constexpr auto cap = 2 * UNIT;
   BOOST_REQUIRE_EQUAL(success(), change(200, 1, cap));
   // Align the first payment to UTC noon so the one-hour retry cannot cross a day boundary.
   const auto utc_seconds = control->pending_block_time().time_since_epoch().count() / math::micros_per_sec;
   wait(3 * math::day_sec + (math::day_sec + math::day_sec / 2 - utc_seconds % math::day_sec) % math::day_sec);
   const auto span = std::min(elapsed(), math::max_catchup_us);
   const auto base = control->pending_block_time() - fc::microseconds(span);
   const auto gift = expected();
   const auto now = control->pending_block_time();
   BOOST_REQUIRE_GT(gift, cap);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(cap, paid());
   BOOST_CHECK_EQUAL(gift - cap, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK(pool()["shortfall_time"].as<time_point>() == now);
   BOOST_CHECK_EQUAL(cap, pool()["spent_today"].as_uint128());
   const auto payment_day = now.time_since_epoch().count() / (math::day_sec * math::micros_per_sec);
   BOOST_CHECK_EQUAL(payment_day, pool()["day"].as_uint64());
   const auto advanced = (kick_clock() - base).count();
   const wide numerator = wide(span) * cap;
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(numerator / gift), advanced);
   BOOST_CHECK(numerator % gift != 0);
   BOOST_CHECK(pool()["price_time"].as<time_point>() == now);
   // The partial payment restarted the observation: an hourly retry is a TWAP warm-up skip that writes nothing.
   wait(3600);
   const auto held = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(held == bytes());
   BOOST_CHECK_EQUAL(cap, paid());
   // Each later UTC noon has a fresh day's ceiling and a mature observation; the remainder stays open.
   for (unsigned day = 0; day < 4; ++day) {
      const auto before = paid();
      wait(day == 0 ? math::day_sec - 3600 : math::day_sec);
      BOOST_REQUIRE_EQUAL(success(), kick());
      BOOST_CHECK_GT(paid(), before);
      BOOST_CHECK_LE(paid() - before, cap);
   }
   BOOST_CHECK_GT(pool()["shortfall_amount"].as_uint64(), 0);
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
   const auto span = std::min(elapsed(), math::max_catchup_us);
   const auto base = control->pending_block_time() - fc::microseconds(span);
   const auto gift = expected();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(2 * UNIT, paid());
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(wide(span) * (2 * UNIT) / gift),
      (kick_clock() - base).count());
   BOOST_CHECK_EQUAL(gift - 2 * UNIT, pool()["shortfall_amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   // The payment restarted the observation, so refunding does not reprice the remainder at once.
   wait(3600);
   const auto held = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(held == bytes());
   wait(math::day_sec - 3600);
   const auto remaining = expected();
   BOOST_CHECK_EQUAL(reference(supply(), 200, std::min(elapsed(), math::max_catchup_us), 1, 1), remaining);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(2 * UNIT + remaining, paid());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
}

/// Review of #675: a spike that funds a capped partial payment must not price the remainder again after the price
/// unwinds. 100,000 LIQ at 2% for 72 hours, observed at 61 WIRE/LIQ, is a gift above the 1,000 WIRE cap; the cap is
/// paid. A day later, back at 1 WIRE/LIQ, the next payment must be the remainder's and the new day's accrual at the
/// unwound price (about 5.5 WIRE). Reusing the spike's observation paid about 250 WIRE: one excursion funded two
/// capped payouts.
BOOST_FIXTURE_TEST_CASE(spike_capped_partial_then_unwind, kicker_tester) {
   constexpr uint64_t spike = 61;
   // The injected spike leaves spot at 1 against a TWAP of 61 (98.4% away): this case isolates the observation
   // checkpoint, so it runs the spot guard at its widest setting.
   BOOST_REQUIRE_EQUAL(success(), configure(budget, math::default_min_interval_sec, SYSIO_ACCOUNT,
                                            math::default_twap_window_sec, uint16_t{10'000}));
   wait(3 * math::day_sec);
   BOOST_REQUIRE_EQUAL(100'000 * UNIT, supply());
   BOOST_REQUIRE_GE(elapsed(), math::max_catchup_us);
   price_history(LIQSOL_SYM, POOL_SYM, spike * UNIT, UNIT, 3 * math::day_sec);
   const auto spiked = reference(supply(), 200, math::max_catchup_us, spike, 1);
   BOOST_REQUIRE_EQUAL(spiked, expected());
   BOOST_REQUIRE_GT(spiked, math::max_daily_gift);
   const auto first_at = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_REQUIRE_EQUAL(math::max_daily_gift, paid());
   BOOST_CHECK_EQUAL(spiked - math::max_daily_gift, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK(pool()["price_time"].as<time_point>() == first_at);

   // The spot price never moved (the spike is injected history), so the next day observes 1 WIRE/LIQ throughout.
   wait(math::day_sec);
   const auto unwound = reference(supply(), 200, std::min(elapsed(), math::max_catchup_us), 1, 1);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(unwound, paid() - math::max_daily_gift);
   BOOST_CHECK_LT(paid() - math::max_daily_gift, 10 * UNIT);
}

/// A year of downtime pays 72 hours' interest and closes the old backlog; a retry cannot recover it.
BOOST_FIXTURE_TEST_CASE(year_outage_is_bounded, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), change(200, 1));
   const auto start = kick_clock();
   produce_block(fc::microseconds(31'557'600'000'000LL - elapsed()));
   BOOST_REQUIRE_EQUAL(31'557'600'000'000LL, elapsed());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(reference(supply(), 200, math::max_catchup_us, 1, 1), paid());
   BOOST_CHECK_EQUAL(31'557'600'000'000LL, (kick_clock() - start).count());
   const auto first = paid();
   wait(3600);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(first, paid());
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
   reopen();
   BOOST_REQUIRE_EQUAL(success(), change(10000, 1));
   wait(2 * math::year_sec);
   price_history(LIQSOL_SYM, POOL_SYM, math::max_amount, 1);
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
   price_history(LIQSOL_SYM, POOL_SYM, 1, 1);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_GT(paid(), 0);
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK(!pool()["gift_overflow"].as_bool());
   // Sized on the burned-down supply, the payment was full and checkpoints it.
   BOOST_CHECK_EQUAL(supply(), pool()["supply_snapshot"].as_int64());
}

/// Zero supply clears an actual overflow observation as well as its timestamp.
BOOST_FIXTURE_TEST_CASE(zero_supply_clears_overflow, kicker_tester) {
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, math::max_amount - supply()));
   reopen();
   BOOST_REQUIRE_EQUAL(success(), change(10000, 1));
   wait(2 * 31'557'600);
   price_history(LIQSOL_SYM, POOL_SYM, math::max_amount, 1);
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
   BOOST_CHECK_EQUAL(0, pool()["supply_snapshot"].as_int64());
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
   BOOST_CHECK_EQUAL(0, pool()["supply_snapshot"].as_int64());
   BOOST_CHECK_EQUAL(0, paid());
   for (const auto& action : trace->action_traces)
      BOOST_CHECK(action.act.name != "addyield"_n);
   produce_block();
}

/// Budget equality pays exactly; funding bookkeeping and all transfers roll back on inline failure.
BOOST_FIXTURE_TEST_CASE(exact_budget_and_atomic_inline_failure, kicker_tester) {
   wait(math::day_sec);
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   // Configure consumes a half-second block; equality is measured at the ensuing kick time.
   const auto exact = reference(supply(), 200,
      std::min(elapsed() + config::block_interval_us, math::max_catchup_us), 1, 1);
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
   BOOST_CHECK_GT(paid(), exact);
}

/// A quarter holder claims the exact floored quarter gift when the existing yield-index scale divides supply.
BOOST_FIXTURE_TEST_CASE(quarter_holder_claims_floored_gift, kicker_tester) {
   constexpr uint64_t small_seed = 750 * UNIT, small_holder = 250 * UNIT;
   const auto pair = symbol::from_string("9,ETHPOOL");
   BOOST_REQUIRE_EQUAL(success(), create(LIQETH_SYM, ETH, LIQETH));
   BOOST_REQUIRE_EQUAL(success(), regliqpool(ETH, LIQETH, pair, small_seed, small_seed));
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, small_holder, LIQETH));
   BOOST_REQUIRE_EQUAL(success(), add(LIQETH_SYM, math::default_rate_bps, 1));
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

/// The accepted residual: a one-block spike buys TWAP area, and a partial unwind to the inflated TWAP, the kick and
/// the rest of the unwind in one block pass the guard even at its tightest 1 bps setting, because same-block trades
/// carry no TWAP weight. The kick pays the spike-inflated gift in full under the daily cap, and the cap above it.
BOOST_AUTO_TEST_CASE(one_block_spike_cashed_in_one_block_pays_the_inflated_gift) {
   constexpr int64_t pump = 10'000'000 * sysio_liq_tester::UNIT;
   constexpr uint16_t tightest = 1;
   constexpr unsigned max_rounds = 6;
   /// Default ceiling, then one below the inflated gift (about 0.58 WIRE).
   for (const uint64_t cap : {math::max_daily_gift, uint64_t{sysio_liq_tester::UNIT / 10}}) {
      BOOST_TEST_CONTEXT("max_gift_per_day=" << cap) {
         kicker_tester t;
         BOOST_REQUIRE_EQUAL(t.success(), t.change(200, 1, cap));
         BOOST_REQUIRE_EQUAL(t.success(), t.configure(t.budget, 1, t.SYSIO_ACCOUNT, math::min_twap_window_sec,
                                                      tightest));
         BOOST_REQUIRE_EQUAL(t.success(), t.openext("bob"_n, "bob"_n, extended_symbol{t.WIRE_SYM, t.TOKEN_ACCOUNT}));
         BOOST_REQUIRE_EQUAL(t.success(), t.openext("bob"_n, "bob"_n, extended_symbol{t.LIQSOL_SYM, t.LIQ_ACCOUNT}));
         BOOST_REQUIRE_EQUAL(t.success(), t.transfer_wire(t.SYSIO_ACCOUNT, "bob"_n, pump));
         BOOST_REQUIRE_EQUAL(t.success(), t.transfer_wire("bob"_n, t.SWAP_ACCOUNT, pump));
         t.wait(math::min_twap_window_sec);
         const auto honest = t.expected();
         BOOST_REQUIRE_GT(honest, 0);
         const auto reserves = [&] {
            const auto pair = t.swap_pool_row(t.POOL_SYM);
            return std::pair{pair["pool2"]["quantity"].as<asset>().get_amount(),
                             pair["pool1"]["quantity"].as<asset>().get_amount()};
         };
         // One block at the pumped price.
         const auto liq_before = reserves().second;
         BOOST_REQUIRE_EQUAL(t.success(), t.exchange("bob"_n, t.POOL_SYM,
            extended_asset{asset(pump, t.WIRE_SYM), t.TOKEN_ACCOUNT}, asset(1, t.LIQSOL_SYM)));
         const auto liq_out = liq_before - reserves().second;
         // Next block, no block produced in between: trade spot onto the inflated TWAP within 1 bps (the swap fee
         // makes one constant-product step land slightly off, so step until it is in), kick, then sell the rest.
         const auto trade = [&](int64_t amount, bool sell_liq) {
            const auto in = sell_liq ? extended_asset{asset(amount, t.LIQSOL_SYM), t.LIQ_ACCOUNT}
                                     : extended_asset{asset(amount, t.WIRE_SYM), t.TOKEN_ACCOUNT};
            t.push_action(t.SWAP_ACCOUNT, "exchange"_n, "bob"_n, mvo()
               ("user", "bob"_n)("pair_token", t.POOL_SYM.to_symbol_code())("ext_asset_in", in)
               ("min_expected", sell_liq ? asset(1, t.WIRE_SYM) : asset(1, t.LIQSOL_SYM)));
         };
         const auto twap = static_cast<math::u128>(t.twap());
         const double target = double(t.twap()) / 18446744073709551616.0;   // Q64.64 to WIRE per LIQ
         BOOST_REQUIRE_GT(target, 100.0);
         int64_t sold = 0;
         unsigned rounds = 0;
         for (; rounds < max_rounds; ++rounds) {
            const auto [wire, liq] = reserves();
            if (!math::spot_deviates(opp::twap::price_fp(wire, liq), twap, tightest)) break;
            const double product = double(wire) * double(liq);
            if (double(wire) / double(liq) > target) {
               const auto amount = std::max<int64_t>(1, int64_t(std::sqrt(product / target)) - liq);
               trade(amount, true);
               sold += amount;
            } else {
               trade(std::max<int64_t>(1, int64_t(std::sqrt(product * target)) - wire), false);
            }
         }
         BOOST_REQUIRE_LT(rounds, max_rounds);
         BOOST_REQUIRE(sold > 0 && sold < liq_out);
         const auto inflated = t.expected();   // same-block trades leave the TWAP as it was
         BOOST_REQUIRE_GT(inflated, 100 * honest);
         if (cap != math::max_daily_gift) BOOST_REQUIRE_GT(inflated, cap);
         t.push_action(t.KICKER, "kick"_n, "bob"_n, mvo()("sym", t.LIQSOL_SYM.to_symbol_code()));
         trade(liq_out - sold, true);
         t.produce_block();
         BOOST_CHECK(!t.decode_pool().deviation);
         if (inflated <= cap) {
            BOOST_CHECK_EQUAL(inflated, t.paid());
         } else {
            BOOST_CHECK_EQUAL(cap, t.paid());
            BOOST_CHECK_EQUAL(inflated - cap, t.decode_pool().shortfall);
         }
      }
   }
}

/// The guard compares spot with the window TWAP at the configured share of the TWAP, inclusively. Spot stays at the
/// fixture's 1 WIRE/LIQ while the injected history prices the window at 2: |1 - 2| / 2 is exactly 5,000 bps.
BOOST_AUTO_TEST_CASE(spot_deviation_threshold_is_inclusive) {
   for (const uint16_t deviation : {uint16_t{5'000}, uint16_t{4'999}}) {
      BOOST_TEST_CONTEXT("max_spot_deviation_bps=" << deviation) {
         kicker_tester t;
         BOOST_REQUIRE_EQUAL(t.success(), t.configure(t.budget, 1, t.SYSIO_ACCOUNT, math::default_twap_window_sec, deviation));
         t.wait(math::day_sec);
         t.price_history(t.LIQSOL_SYM, t.POOL_SYM, 2 * t.UNIT, t.UNIT);
         const auto gift = t.expected();
         BOOST_REQUIRE_GT(gift, 0);
         const auto observation = t.decode_pool();
         BOOST_REQUIRE_EQUAL(t.success(), t.kick());
         const auto after = t.decode_pool();
         if (deviation == 5'000) {
            BOOST_CHECK_EQUAL(gift, t.paid());
            BOOST_CHECK(!after.deviation);
         } else {
            BOOST_CHECK_EQUAL(0, t.paid());
            BOOST_CHECK(after.deviation);
            BOOST_CHECK_EQUAL(gift, after.shortfall);
            BOOST_CHECK(after.observed == t.control->head_block_time());
            // The clock and the observation are held.
            BOOST_CHECK(after.clock == observation.clock);
            BOOST_CHECK(after.price_time == observation.price_time);
            BOOST_CHECK(after.snapshot_lo == observation.snapshot_lo && after.snapshot_hi == observation.snapshot_hi);
         }
      }
   }
}

/// Governance must keep the guard on: 1 to 10,000 bps; a rejected change leaves the configuration as it was.
BOOST_FIXTURE_TEST_CASE(spot_deviation_config_validation, kicker_tester) {
   BOOST_CHECK_EQUAL(math::default_max_spot_deviation_bps,
                     configuration()[kicker::field::max_spot_deviation_bps].as_uint64());
   BOOST_CHECK_EQUAL(5'000, math::default_max_spot_deviation_bps);
   for (const uint16_t accepted : {uint16_t{1}, uint16_t{10'000}}) {
      BOOST_REQUIRE_EQUAL(success(), configure(budget, 1, SYSIO_ACCOUNT, math::default_twap_window_sec, accepted));
      BOOST_CHECK_EQUAL(accepted, configuration()[kicker::field::max_spot_deviation_bps].as_uint64());
   }
   for (const uint16_t refused : {uint16_t{0}, uint16_t{10'001}}) {
      BOOST_CHECK(mentions(configure(budget, 1, SYSIO_ACCOUNT, math::default_twap_window_sec, refused),
                           invalid_spot_deviation_message));
      BOOST_CHECK_EQUAL(10'000, configuration()[kicker::field::max_spot_deviation_bps].as_uint64());
   }
}

/// Review of #675: a pump held across a payment checkpoint. With a 60 s window, 100,000 LIQ at 200 bps, the
/// attacker pumps in the block before `kick`, holds for one more 500 ms block, then unwinds: each half-second
/// lands in a different observation, one each side of a UTC midnight. Run against the contract before the guard,
/// this test saw both kicks pay the capped 50 WIRE (100 WIRE in all). The pool's daily cap is 50 WIRE here: the fixture's treasury cannot move this pool's spot to the
/// review's 31.5 M WIRE/LIQ, and the construction scales with the cap. The spot-vs-TWAP guard refuses both: spot is
/// far from the window TWAP at the first kick, and the unrestarted observation still carries the pump at the
/// second.
BOOST_FIXTURE_TEST_CASE(pump_straddling_a_checkpoint_funds_no_payout, kicker_tester) {
   constexpr uint32_t window = math::min_twap_window_sec;
   constexpr int64_t pump = 500'000'000 * UNIT;
   constexpr uint64_t cap = 50 * UNIT;
   const auto block = fc::microseconds(config::block_interval_us);
   const auto advance_to = [&](time_point target) {
      BOOST_REQUIRE(control->pending_block_time() <= target);
      if (control->pending_block_time() < target) produce_block(target - control->pending_block_time());
      BOOST_REQUIRE(control->pending_block_time() == target);
   };
   BOOST_REQUIRE_EQUAL(success(), configure(budget, 1, SYSIO_ACCOUNT, window));
   BOOST_REQUIRE_EQUAL(100'000 * UNIT, supply());
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{WIRE_SYM, TOKEN_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), openext("bob"_n, "bob"_n, extended_symbol{LIQSOL_SYM, LIQ_ACCOUNT}));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, "bob"_n, pump));
   BOOST_REQUIRE_EQUAL(success(), transfer_wire("bob"_n, SWAP_ACCOUNT, pump));

   // The first kick lands ten seconds before a UTC midnight, the observation starting one window earlier.
   constexpr int64_t day_us = int64_t(math::day_sec) * math::micros_per_sec;
   const int64_t pending_us = control->pending_block_time().time_since_epoch().count();
   int64_t midnight_us = (pending_us / day_us + 1) * day_us;
   if (midnight_us - pending_us < 120 * int64_t(math::micros_per_sec)) midnight_us += day_us;
   const time_point first(fc::microseconds(midnight_us - 10 * int64_t(math::micros_per_sec)));
   const time_point start = first - fc::seconds(window);
   // Each push produces its block, so the next action lands one block later.
   advance_to(start - block);
   BOOST_REQUIRE_EQUAL(success(), push(KICKER, kicker_ser, SYSIO_ACCOUNT, "rmpool"_n,
                                       mvo()("sym", LIQSOL_SYM.to_symbol_code())));
   BOOST_REQUIRE_EQUAL(success(), add(LIQSOL_SYM, math::default_rate_bps, math::default_min_gift, cap));
   BOOST_REQUIRE(decode_pool().price_time == start);

   // Pump in the block before the kick.
   advance_to(first - block);
   const auto liq_before = swap_pool_row(POOL_SYM)["pool1"]["quantity"].as<asset>().get_amount();
   BOOST_REQUIRE_EQUAL(success(), exchange("bob"_n, POOL_SYM, extended_asset{asset(pump, WIRE_SYM), TOKEN_ACCOUNT},
                                           asset(1, LIQSOL_SYM)));
   const auto liq_out = liq_before - swap_pool_row(POOL_SYM)["pool1"]["quantity"].as<asset>().get_amount();
   advance_to(first);
   BOOST_REQUIRE_EQUAL(success(), kick());
   const auto first_paid = paid();
   // Hold one more block, then unwind.
   advance_to(first + block);
   BOOST_REQUIRE_EQUAL(success(), exchange("bob"_n, POOL_SYM, extended_asset{asset(liq_out, LIQSOL_SYM), LIQ_ACCOUNT},
                                           asset(1, WIRE_SYM)));
   advance_to(first + fc::seconds(window));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_TEST_MESSAGE("straddle: first kick paid " << first_paid << ", both kicks paid " << paid());
   BOOST_CHECK_EQUAL(0, first_paid);
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK(pool()["spot_deviation"].as_bool());
   BOOST_CHECK(decode_pool().clock == start);
   BOOST_CHECK(decode_pool().price_time == start);
}

/** Review of #675 (PRRT_kwDOMnX85s6vxcVQ): LIQ minted immediately before a mature kick must not be paid for the
 * interval it did not hold. 100,000 LIQ accrue for exactly one day at price 1; 18,162,500 LIQ are minted in the
 * kick's block. A gift sized on the current supply would be 182.625 times the honest one and pay the 1,000 WIRE
 * daily cap; sized on the checkpointed supply it pays the honest 5.475701574 WIRE, and the full payment then
 * checkpoints the grown supply.
 */
BOOST_FIXTURE_TEST_CASE(mint_immediately_before_mature_kick, kicker_tester) {
   BOOST_REQUIRE_EQUAL(review_supply, supply());
   BOOST_REQUIRE_EQUAL(review_honest_gift,
                       reference(review_supply, math::default_rate_bps, day_us, unit_price, unit_price));
   // Each pushed action seals its own block, so the mint lands in the block just before the kick's.
   advance_elapsed(*this, day_us - config::block_interval_us);
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, review_mint));
   // Read through the deployed ABI, so the payment assertion below also runs against an older row layout.
   BOOST_REQUIRE(control->pending_block_time() == pool()["last_kick"].as<time_point>() + fc::microseconds(day_us));
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(review_honest_gift, paid());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(supply(), pool()["supply_snapshot"].as_int64());
   // The correctly sized gift is still shared across the current supply, the new LIQ included.
   const auto distribution = yield_reference::distribute(paid(), supply(), 0);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(distribution.carry), index_row()["carry"].as_uint64());
}

/// A partial payment keeps the supply it priced: LIQ minted before it is not sized into the open remainder, and only
/// the full payment that settles the remainder checkpoints the grown supply.
BOOST_FIXTURE_TEST_CASE(partial_payment_keeps_priced_supply, kicker_tester) {
   constexpr uint64_t partial_budget = 2 * UNIT;
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, review_mint));
   BOOST_REQUIRE_EQUAL(success(), configure(partial_budget));
   advance_elapsed(*this, day_us);
   BOOST_REQUIRE_EQUAL(review_honest_gift, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(partial_budget, paid());
   BOOST_CHECK_EQUAL(review_honest_gift - partial_budget, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(review_supply, pool()["supply_snapshot"].as_int64());
   BOOST_REQUIRE_EQUAL(success(), configure(budget));
   wait(math::day_sec);
   const auto remainder = reference(review_supply, math::default_rate_bps, std::min(elapsed(), math::max_catchup_us),
                                    unit_price, unit_price);
   BOOST_REQUIRE_EQUAL(remainder, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(partial_budget + remainder, paid());
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   BOOST_CHECK_EQUAL(supply(), pool()["supply_snapshot"].as_int64());
}

/// A zero snapshot with nonzero supply only checkpoints: LIQ minted after the supply emptied starts accruing at the
/// next kick, which pays nothing and keeps the price observation, and the kick after it pays that supply's interval.
BOOST_FIXTURE_TEST_CASE(zero_snapshot_checkpoints_then_pays, kicker_tester) {
   burn_to(*this, 0);
   wait(math::default_min_interval_sec);
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_REQUIRE_EQUAL(0, pool()["supply_snapshot"].as_int64());
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, holder));
   wait(math::day_sec);
   const auto observed = decode_pool().price_time;
   const auto now = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK(kick_clock() == now);
   BOOST_CHECK_EQUAL(int64_t(holder), pool()["supply_snapshot"].as_int64());
   BOOST_CHECK(decode_pool().price_time == observed);
   advance_elapsed(*this, day_us);
   const auto gift = reference(holder, math::default_rate_bps, day_us, unit_price, unit_price);
   BOOST_REQUIRE_EQUAL(gift, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
   BOOST_CHECK_EQUAL(supply(), pool()["supply_snapshot"].as_int64());
}

/// A snapshot too small to reach min_gift cannot hold a grown supply unpaid forever: once the 72-hour catch-up is
/// saturated, a below-minimum kick forfeits the interval and checkpoints the current supply, which the next mature
/// kick pays for.
BOOST_FIXTURE_TEST_CASE(saturated_tiny_snapshot_resets_to_grown_supply, kicker_tester) {
   burn_to(*this, tiny_supply);
   reopen();
   BOOST_REQUIRE_EQUAL(tiny_supply, pool()["supply_snapshot"].as_int64());
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, holder));
   // Below the catch-up bound the skip writes nothing.
   wait(math::day_sec);
   const auto held = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == held);
   // Saturated: forfeit and checkpoint at the current supply, keeping the observation.
   wait(2 * math::day_sec);
   BOOST_REQUIRE_GE(elapsed(), math::max_catchup_us);
   const auto observed = decode_pool().price_time;
   const auto now = control->pending_block_time();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK(kick_clock() == now);
   BOOST_CHECK_EQUAL(supply(), pool()["supply_snapshot"].as_int64());
   BOOST_CHECK(decode_pool().price_time == observed);
   BOOST_CHECK_EQUAL(0, pool()["shortfall_amount"].as_uint64());
   advance_elapsed(*this, day_us);
   const auto gift = reference(uint64_t(supply()), math::default_rate_bps, day_us, unit_price, unit_price);
   BOOST_REQUIRE_EQUAL(gift, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
}

/// Without growth there is nothing to reset to: a saturated below-minimum snapshot that equals the supply writes
/// nothing, however long the interval.
BOOST_FIXTURE_TEST_CASE(saturated_tiny_snapshot_without_growth_holds, kicker_tester) {
   burn_to(*this, tiny_supply);
   reopen();
   wait(3 * math::day_sec);
   BOOST_REQUIRE_GE(elapsed(), math::max_catchup_us);
   const auto held = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == held);
   BOOST_CHECK_EQUAL(0, paid());
}

/// A price dip is not a small snapshot: with the 72 hours saturated and the supply grown past the snapshot, a window
/// TWAP that sinks the gift of the current supply below min_gift too leaves the kick holding (writing nothing), and
/// the interval still pays at the recovered price.
BOOST_FIXTURE_TEST_CASE(price_dip_below_minimum_does_not_forfeit, kicker_tester) {
   burn_to(*this, holder);
   reopen();
   const auto snapshot = pool()["supply_snapshot"].as_int64();
   BOOST_REQUIRE_EQUAL(int64_t(holder), snapshot);
   BOOST_REQUIRE_EQUAL(success(), mint("alice"_n, holder));
   BOOST_REQUIRE_GT(supply(), snapshot);
   wait(3 * math::day_sec);
   BOOST_REQUIRE_GE(elapsed(), math::max_catchup_us);
   // A tenth of the price: the snapshot's gift AND the grown supply's gift fall under the one-WIRE minimum, while
   // the snapshot's gift at the recovered price clears it.
   price_history(LIQSOL_SYM, POOL_SYM, UNIT, 10 * UNIT, 3 * math::day_sec);
   const auto min_gift = pool()["min_gift"].as_uint64();
   BOOST_REQUIRE_LT(reference(uint64_t(snapshot), 200, math::max_catchup_us, 1, 10), min_gift);
   BOOST_REQUIRE_LT(reference(supply(), 200, math::max_catchup_us, 1, 10), min_gift);
   BOOST_REQUIRE_GE(reference(uint64_t(snapshot), 200, math::max_catchup_us, 1, 1), min_gift);
   const auto held = bytes();
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK(bytes() == held);
   BOOST_CHECK_EQUAL(0, paid());
   BOOST_CHECK_EQUAL(snapshot, pool()["supply_snapshot"].as_int64());
   // The price recovers: the same open interval pays on the snapshot's supply.
   price_history(LIQSOL_SYM, POOL_SYM, UNIT, UNIT, 3 * math::day_sec);
   const auto gift = reference(uint64_t(snapshot), 200, math::max_catchup_us, 1, 1);
   BOOST_REQUIRE_EQUAL(gift, expected());
   BOOST_REQUIRE_EQUAL(success(), kick());
   BOOST_CHECK_EQUAL(gift, paid());
   BOOST_CHECK_EQUAL(supply(), pool()["supply_snapshot"].as_int64());
}

namespace {
/// What a partial-unwind straddle measured: both kicks' prices and payments, and the honest value of the second.
struct straddle_outcome {
   wide twap1, twap2;
   uint64_t gift1 = 0, paid1 = 0, gift2 = 0, honest2 = 0, paid2 = 0;
   /// The contract's recorded shortfall after each kick: its own gift less what it paid.
   uint64_t shortfall1 = 0, shortfall2 = 0;
   int64_t advance1 = 0;
   uint64_t elapsed2 = 0;
   /// The second kick's guard verdict, and the pool row's clock and observation either side of it.
   bool deviation2 = false;
   kicker_tester::pool_state before2, after2;
};

/** A straddle whose first kick follows a PARTIAL unwind, with the swap history injected (the fixture's WIRE supply
 * cannot reach the peak; pricing, guard and payment are the contract's). Default 86,400 s window, 5,000 bps guard
 * and 1,000 WIRE cap; `supply` LIQ at 200 bps. The observation holds spot 1 for a day except its last block, where a
 * one-block peak of 2,985,984,001 lifts the TWAP to 17,281. In that block the attacker unwinds to spot 17,281 and
 * kicks: the guard sees no deviation and the payment stores the checkpoint at that spot. Spot stays 17,281 for
 * `hold_blocks`, then unwinds fully to 1, and the second kick, one window after the first and on the next UTC day,
 * prices at the seeded TWAP 1 + 17,280 * hold / window. A nonzero `mint_between` mints that much LIQ right after the
 * first kick; the honest value of the second stays that of `supply_target`, the supply that held the interval.
 */
straddle_outcome run_partial_unwind_straddle(int64_t supply_target, unsigned hold_blocks, uint64_t mint_between = 0) {
   constexpr uint64_t peak = 2'985'984'001;
   constexpr uint64_t partial = 17'281;
   kicker_tester t;
   const auto& unit = t.UNIT;
   const int64_t liq_reserve = 75'000 * unit;
   const int64_t day_us = int64_t(math::day_sec) * math::micros_per_sec;
   const auto block = fc::microseconds(config::block_interval_us);
   const auto advance_to = [&](time_point target) {
      BOOST_REQUIRE(t.control->pending_block_time() <= target);
      if (t.control->pending_block_time() < target) t.produce_block(target - t.control->pending_block_time());
      BOOST_REQUIRE(t.control->pending_block_time() == target);
   };
   const auto pair_code = t.POOL_SYM.to_symbol_code().value;
   const auto set_spot = [&](uint64_t wire_per_liq) {
      auto pair = mvo(t.swap_pool_row(t.POOL_SYM).get_object());
      pair("pool1", extended_asset(asset(liq_reserve, t.LIQSOL_SYM), t.LIQ_ACCOUNT))
          ("pool2", extended_asset(asset(liq_reserve * int64_t(wire_per_liq), t.WIRE_SYM), t.TOKEN_ACCOUNT));
      t.write_row(t.SWAP_ACCOUNT, t.swap_abi_ser, "stat"_n, pair_code, "currency_stats", pair);
   };
   const wide one_q = wide(1) << 64;
   const auto set_accumulator = [&](const wide& cumulative, time_point at) {
      const wide mask = (wide(1) << 128) - 1;
      const auto zero = mvo()("lo", fc::uint128_t{0})("hi", fc::uint128_t{0});
      t.write_row(t.SWAP_ACCOUNT, t.swap_abi_ser, "priceaccum"_n, pair_code, "price_accumulator", mvo()
         ("price1", mvo()("lo", static_cast<fc::uint128_t>(cumulative & mask))
                         ("hi", static_cast<fc::uint128_t>(cumulative >> 128)))
         ("price2", zero)("last_update", at));
   };

   BOOST_REQUIRE_EQUAL(t.success(), t.mint("alice"_n, uint64_t(supply_target - t.supply())));
   BOOST_REQUIRE_EQUAL(supply_target, t.supply());
   // The first kick lands ten seconds before a UTC midnight, the pool's clock and observation one day earlier.
   const int64_t pending_us = t.control->pending_block_time().time_since_epoch().count();
   const int64_t midnight_us = (pending_us / day_us + 2) * day_us;
   const time_point first(fc::microseconds(midnight_us - 10 * int64_t(math::micros_per_sec)));
   const time_point start = first - fc::microseconds(day_us);
   advance_to(start - block);
   BOOST_REQUIRE_EQUAL(t.success(), t.push(t.KICKER, t.kicker_ser, t.SYSIO_ACCOUNT, "rmpool"_n,
                                           mvo()("sym", t.LIQSOL_SYM.to_symbol_code())));
   BOOST_REQUIRE_EQUAL(t.success(), t.add());
   BOOST_REQUIRE(t.decode_pool().clock == start && t.decode_pool().price_time == start);

   // Kick 1, in the block that ends the peak: the day at spot 1 bar the last block at the peak, observed from zero.
   advance_to(first);
   const wide area1 = one_q * (day_us - block.count()) + one_q * peak * block.count();
   set_accumulator(area1, first);
   auto row = t.get_row_by_id(t.KICKER, t.KICKER, "kickpools"_n, t.LIQSOL_SYM.to_symbol_code().value);
   const auto observation = fc::raw::pack(std::make_tuple(fc::uint128_t{0}, fc::uint128_t{0}, start));
   std::copy(observation.begin(), observation.end(), row.end() - observation.size());
   t.write_raw_row(t.KICKER, "kickpools"_n, t.LIQSOL_SYM.to_symbol_code().value, row);
   set_spot(partial);   // the partial unwind, in the same block
   straddle_outcome out;
   out.twap1 = t.twap();
   out.gift1 = t.expected();
   BOOST_REQUIRE_EQUAL(t.success(), t.kick());
   out.paid1 = t.paid();
   out.shortfall1 = t.decode_pool().shortfall;
   out.advance1 = (t.decode_pool().clock - start).count();
   if (mint_between) BOOST_REQUIRE_EQUAL(t.success(), t.mint("alice"_n, mint_between));

   // Hold spot 17,281 for `hold_blocks`, then unwind fully to 1.
   const time_point unwind = first + fc::microseconds(block.count() * hold_blocks);
   advance_to(unwind);
   set_accumulator(area1 + one_q * partial * (block.count() * hold_blocks), unwind);
   set_spot(1);
   t.produce_block();

   // Kick 2, one window after the checkpoint and on the next UTC day.
   advance_to(first + fc::microseconds(day_us));
   out.twap2 = t.twap();
   out.elapsed2 = std::min(t.elapsed(), math::max_catchup_us);
   out.gift2 = t.expected();
   out.before2 = t.decode_pool();
   out.honest2 = reference(uint64_t(supply_target), 200, out.elapsed2, 1, 1);
   BOOST_REQUIRE_EQUAL(t.success(), t.kick());
   out.paid2 = t.paid() - out.paid1;
   out.after2 = t.decode_pool();
   out.shortfall2 = out.after2.shortfall;
   out.deviation2 = out.after2.deviation;
   const auto ratio = [&](const wide& q) { return double(q) / double(one_q); };
   BOOST_TEST_MESSAGE("partial-unwind straddle supply=" << supply_target << " hold_blocks=" << hold_blocks
      << ": twap1=" << ratio(out.twap1) << " gift1=" << out.gift1 << " paid1=" << out.paid1
      << " clock advance (us)=" << out.advance1 << " twap2=" << ratio(out.twap2) << " elapsed2 (us)="
      << out.elapsed2 << " gift2=" << out.gift2 << " honest2=" << out.honest2 << " paid2=" << out.paid2
      << " excess2=" << int64_t(out.paid2) - int64_t(out.honest2));
   return out;
}
} // namespace

/** Review of #675 (third pass, PRRT_kwDOMnX85s6vo-1F): one price excursion can fund one capped payout plus, on the
 * next payout, at most `max_spot_deviation_bps * cap / 10000` of excess over its honest value (500 WIRE at the
 * default). The guard bounds the next price at spot / (1 - d), so the excess is min(cap, honest / (1 - d)) - honest,
 * largest when the honest gift is cap * (1 - d).
 *
 * Measured, 8,675,000 LIQ with a one-block hold (the review's construction): TWAP 17,281 then 1.1; the first kick pays
 * the 1,000 WIRE cap and advances the clock 10.525327 s; the second's gift is 1,044.973991726 WIRE against an honest
 * 949.976356115 WIRE, and it pays the cap: 50.023643885 WIRE of excess.
 *
 * Measured at the bound's worst point, 4,566,000 LIQ held ten blocks (5 s): TWAP 17,281 then exactly 2, the guard's
 * inclusive edge at spot 1. The first kick pays the cap and advances the clock 19.9972 s; the second gift is
 * 999.966401480 WIRE, twice an honest 499.983200740 WIRE, just under the cap, so it pays in full: 499.983200740 WIRE
 * of excess against the 500 WIRE bound.
 *
 * Past the edge, 2,283,000 LIQ held thirty blocks (15 s): TWAP 17,281 then 4, 7,500 bps from spot 1. The guard refuses
 * kick 2, so the case fails if the guard is removed or loosened past 7,500 bps.
 */
BOOST_AUTO_TEST_CASE(partial_unwind_straddle_overpays_at_most_deviation_times_cap) {
   const wide one_q = wide(1) << 64;
   const uint64_t cap = math::max_daily_gift;
   const uint64_t bound = uint64_t(math::default_max_spot_deviation_bps) * cap / math::bps_scale;
   BOOST_REQUIRE_EQUAL(500 * sysio_liq_tester::UNIT, bound);
   struct scenario { int64_t supply; unsigned hold_blocks; };
   for (const auto [supply, hold_blocks] : {scenario{8'675'000 * sysio_liq_tester::UNIT, 1},
                                            scenario{4'566'000 * sysio_liq_tester::UNIT, 10},
                                            scenario{2'283'000 * sysio_liq_tester::UNIT, 30}}) {
      BOOST_TEST_CONTEXT("supply=" << supply << " hold_blocks=" << hold_blocks) {
         const auto out = run_partial_unwind_straddle(supply, hold_blocks);
         // The contract priced kick 1 at TWAP 17,281: its recorded shortfall is the oracle's gift less the cap.
         BOOST_CHECK(out.twap1 == one_q * 17'281);
         BOOST_CHECK_EQUAL(cap, out.paid1);
         BOOST_CHECK_EQUAL(out.gift1 - cap, out.shortfall1);
         if (hold_blocks == 30) {
            // Past the guard's inclusive edge: TWAP 4 against spot 1 is 7,500 bps. Kick 2 pays nothing, records the
            // trip with its gift as the shortfall, and holds the clock and the observation.
            BOOST_CHECK(out.twap2 == one_q * 4);
            BOOST_CHECK(out.deviation2);
            BOOST_CHECK_EQUAL(0, out.paid2);
            BOOST_CHECK_EQUAL(out.gift2, out.shortfall2);
            BOOST_CHECK(out.after2.clock == out.before2.clock);
            BOOST_CHECK(out.after2.price_time == out.before2.price_time);
            BOOST_CHECK(out.after2.snapshot_lo == out.before2.snapshot_lo &&
                        out.after2.snapshot_hi == out.before2.snapshot_hi);
            continue;
         }
         BOOST_CHECK(!out.deviation2);
         BOOST_CHECK_EQUAL(std::min(out.gift2, cap), out.paid2);
         BOOST_CHECK_EQUAL(out.gift2 - out.paid2, out.shortfall2);   // the contract's own kick-2 pricing
         BOOST_REQUIRE_LE(out.honest2, out.paid2);
         BOOST_CHECK_LE(out.paid2 - out.honest2, bound);
         if (hold_blocks == 1) {
            BOOST_CHECK(out.twap2 == one_q * 11 / 10);
            BOOST_CHECK_EQUAL(cap, out.paid2);   // the second payout reaches the cap
         } else {
            BOOST_CHECK(out.twap2 == one_q * 2);
            BOOST_CHECK_GT(out.paid2 - out.honest2, bound * 99 / 100);   // the bound is nearly reached here
         }
      }
   }
}

/** The bound holds with LIQ minted inside the straddle. At the bound's worst point (4,566,000 LIQ held ten blocks,
 * kick 2 at TWAP exactly 2), minting ten times the supply after kick 1 would, sized on the current supply, lift kick 2
 * to the 1,000 WIRE cap: about 500.017 WIRE of excess, past the 500 WIRE bound. Sized on the supply kick 1 priced,
 * kick 2 pays its 999.966401480 WIRE gift and the excess stays inside the bound.
 */
BOOST_AUTO_TEST_CASE(partial_unwind_straddle_with_mint_stays_within_bound) {
   constexpr int64_t straddle_supply = 4'566'000 * sysio_liq_tester::UNIT;
   constexpr unsigned straddle_hold_blocks = 10;
   constexpr uint64_t straddle_mint = 10 * uint64_t(straddle_supply);
   const uint64_t cap = math::max_daily_gift;
   const uint64_t bound = uint64_t(math::default_max_spot_deviation_bps) * cap / math::bps_scale;
   const auto out = run_partial_unwind_straddle(straddle_supply, straddle_hold_blocks, straddle_mint);
   BOOST_CHECK_EQUAL(cap, out.paid1);
   BOOST_CHECK(!out.deviation2);
   BOOST_CHECK(out.twap2 == (wide(2) << 64));
   BOOST_CHECK_EQUAL(out.gift2, out.paid2);
   BOOST_CHECK_LT(out.paid2, cap);
   BOOST_REQUIRE_LE(out.honest2, out.paid2);
   BOOST_CHECK_LE(out.paid2 - out.honest2, bound);
   BOOST_CHECK_EQUAL(straddle_supply + int64_t(straddle_mint), out.after2.supply_snapshot);
}

BOOST_AUTO_TEST_SUITE_END()
