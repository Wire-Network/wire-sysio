/** @file sysio.kicker_fuzz_tests.cpp
 * @brief Seeded differential sequences: the model is independent in ARITHMETIC;
 * its control flow follows the rulings in kick()'s order.
 * Supply/spot/reserve rows are deliberately injected boundary inputs, not a model of swap execution.
 * Two real LIQ pools share real treasury transfers and addyield; no kicker math helper is the oracle.
 */
#include "kicker_tester.hpp"
#include <array>
#include <random>
#include <magic_enum/magic_enum.hpp>

namespace {
using kicker_test_support::kicker_tester;
using kicker_test_reference::wide;
/// Literal protocol units, independently specified by the owner's rulings.
constexpr uint64_t unit = 1'000'000'000, micros = 1'000'000, day_seconds = 86'400;
constexpr uint64_t basis_points = 10'000;
/// Random campaign sizes and logarithmic input scales; no step selects a forced outcome.
constexpr uint32_t default_spacing = 3600;
/// Longer randomly selected spacing exposes funded short intervals in the fixed campaign.
constexpr uint32_t random_spacing = day_seconds;
constexpr auto funding_account = "carol"_n;
constexpr auto remove_pool_action = "rmpool"_n;
constexpr uint64_t funding_range = 100, reserve_range = 50, treasury_base = 300;
static_assert(treasury_base > 2 * reserve_range + funding_range);
constexpr uint64_t minimum_range = 5, distributed_range = 1000;
constexpr uint64_t boundary_supply = 100'000 * unit, boundary_minimum = unit;
constexpr uint64_t boundary_funding = 100 * unit, boundary_distributed = 101 * unit;
constexpr uint64_t midday_seconds = day_seconds / 2;
constexpr uint64_t asset_limit = (uint64_t{1} << 62) - 1;
constexpr unsigned sequences = 150, steps = 16, batch_size = 10;
constexpr unsigned hold_toggle_denominator = 8, exhaustion_denominator = 5;
constexpr std::array<uint64_t, 5> supply_scales{unit, 100 * unit, 100'000 * unit, 100'000'000 * unit, asset_limit};
constexpr std::array<uint64_t, 5> price_scales{unit, 100 * unit, 10'000 * unit, 1'000'000 * unit, asset_limit};
constexpr auto precision_prefix = "9,";
/// Contract identifiers shared by deterministic injection and random differential checks.
namespace field {
constexpr auto sym = "sym", distributable = "t5_distributable", supply = "supply";
constexpr auto distributed = "total_distributed", floor = "t5_floor";
constexpr auto pool1 = "pool1", pool2 = "pool2", pot = "pot", carry = "carry", index = "index";
constexpr auto budget = "budget_remaining", spacing = "min_interval_sec";
}
/// Historical campaign identity combines kicker PR 675 with the original source SHA prefix 651c01.
constexpr uint64_t seed_base = 0x675'651c01;
/// Golden-ratio Weyl stride spreads adjacent sequence seeds across the 64-bit seed space.
constexpr uint64_t seed_stride = 0x9e3779b97f4a7c15ULL;
constexpr auto config_table = "kickcfg"_n;
constexpr auto stat_table = "stat"_n;
constexpr auto eth_pair = "ETHPOOL";
constexpr auto stat_type = "currency_stats";
constexpr auto config_type = "kick_config";
constexpr auto emission_table = "emitcfg"_n, state_table = "t5state"_n;
constexpr auto emission_type = "emission_config", state_type = "t5_state";
constexpr auto swap_table = "stat"_n;
constexpr auto swap_type = "currency_stats";
constexpr std::array<uint64_t, 8> intervals{1, 30, 3599, 3600, 15'768, 86'400, 2'592'000, 7'776'000};

/// Outcomes counted only among independently sampled random kicks.
struct coverage {
   uint64_t random_price = 0, full_random_price = 0, partial = 0, subminimum = 0, unfunded = 0;
   uint64_t reserve_exhausted = 0, cap_exhausted = 0, exclusive_cap = 0, held_would_pay = 0, spaced_would_pay = 0;
};
/// Half-observed random-only floors, independent of fixture precondition assertions.
constexpr coverage coverage_floor{
   .random_price = 165, .full_random_price = 21, .partial = 144, .subminimum = 158, .unfunded = 68,
   .reserve_exhausted = 54, .cap_exhausted = 6, .exclusive_cap = 3, .held_would_pay = 196, .spaced_would_pay = 11};
/// Draw magnitude and scale in a specified order, reproducible across compiler evaluation orders.
uint64_t sample_amount(std::mt19937_64& random, const std::array<uint64_t, 5>& scales) {
   const auto amount_draw = random();
   const auto scale_draw = random();
   return 1 + amount_draw % scales[scale_draw % scales.size()];
}
/// Complete per-pool state, including exact microsecond clocks (JSON timestamps truncate them).
struct state {
   uint64_t sym, minimum, total, shortfall, cap, day, spent;
   uint16_t rate;
   int64_t clock, observed;
   bool overflow;
};
/// Adapt the shared fixture decoder to the arithmetic model's bounded integer fields.
state read(kicker_tester& t, symbol sym) {
   const auto row = t.decode_pool(sym);
   state r;
   r.sym = row.sym; r.rate = row.rate; r.minimum = row.minimum;
   r.clock = row.clock.time_since_epoch().count();
   r.observed = row.observed.time_since_epoch().count();
   r.total = static_cast<uint64_t>(row.total); r.spent = static_cast<uint64_t>(row.spent);
   r.shortfall = row.shortfall; r.overflow = row.overflow;
   r.cap = row.cap; r.day = row.day;
   return r;
}
/// Assert every field rather than accepting a correct payment with an incorrect clock/diagnostic.
void compare(const state& a, const state& b) {
   BOOST_REQUIRE_EQUAL(a.sym, b.sym);
   BOOST_REQUIRE_EQUAL(a.rate, b.rate);
   BOOST_REQUIRE_EQUAL(a.minimum, b.minimum);
   BOOST_REQUIRE_EQUAL(a.clock, b.clock);
   BOOST_REQUIRE_EQUAL(a.total, b.total);
   BOOST_REQUIRE_EQUAL(a.shortfall, b.shortfall);
   BOOST_REQUIRE_EQUAL(a.observed, b.observed);
   BOOST_REQUIRE_EQUAL(a.overflow, b.overflow);
   BOOST_REQUIRE_EQUAL(a.cap, b.cap);
   BOOST_REQUIRE_EQUAL(a.day, b.day);
   BOOST_REQUIRE_EQUAL(a.spent, b.spent);
}
/// Apply kick rulings in order using shared wide exact interest and floored Q64 valuation.
uint64_t advance(state& s, int64_t now, uint64_t supply, uint64_t wire, uint64_t shadow,
                 uint64_t budget, uint64_t treasury, uint64_t pending, uint64_t claims,
                 uint64_t remaining, uint32_t spacing, bool held) {
   BOOST_REQUIRE_GT(now, s.clock);
   if (uint64_t(now - s.clock) < spacing * micros || held) return 0;
   if (!supply) {
      s.clock = now; s.shortfall = 0; s.observed = 0; s.overflow = false;
      return 0;
   }
   const auto elapsed = uint64_t(now - s.clock);
   const wide gift = kicker_test_reference::wide_reference(supply, s.rate, elapsed, wire, shadow);
   if (gift < s.minimum) return 0;
   const wide reserve = wide(pending) + claims + remaining;
   uint64_t available = reserve >= treasury ? 0 : uint64_t(wide(treasury) - reserve);
   available = std::min(available, budget);
   const auto today = uint64_t(now) / (day_seconds * micros);
   const auto spent = today == s.day ? s.spent : 0;
   if (s.cap) available = std::min(available, spent >= s.cap ? 0 : s.cap - spent);
   s.overflow = gift > asset_limit;
   const uint64_t paid = !s.overflow && available >= s.minimum ? std::min(uint64_t(gift), available) : 0;
   s.shortfall = s.overflow ? 0 : uint64_t(gift) - paid;
   s.observed = s.shortfall || s.overflow ? now : 0;
   if (paid) {
      s.clock += int64_t(wide(elapsed) * paid / gift);
      s.total += paid; s.day = today; s.spent = spent + paid;
   }
   return paid;
}
/// Inject current supply and spot through the real contract layouts.
void inject_price(kicker_tester& t, symbol sym, symbol pair_sym, uint64_t supply, uint64_t wire, uint64_t shadow) {
   auto stat = mvo(t.stat_row(sym).get_object());
   stat(field::supply, asset(supply, sym));
   t.write_row(t.LIQ_ACCOUNT, t.liq_abi_ser, stat_table, sym.to_symbol_code().value, stat_type, stat);
   auto pair = mvo(t.swap_pool_row(pair_sym).get_object());
   pair(field::pool1, extended_asset(asset(shadow, sym), t.LIQ_ACCOUNT))
       (field::pool2, extended_asset(asset(wire, t.WIRE_SYM), t.TOKEN_ACCOUNT));
   t.write_row(t.SWAP_ACCOUNT, t.swap_abi_ser, swap_table, pair_sym.to_symbol_code().value, swap_type, pair);
}
/// Set reserve components independently, including a ceiling below already distributed emissions.
void inject_reserves(kicker_tester& t, uint64_t pending, uint64_t claims, uint64_t remaining,
                     uint64_t distributed, bool saturated = false) {
   auto treasury_state = mvo(t.treasury(state_table, state_type).get_object());
   treasury_state(field::distributed, distributed);
   t.write_global(state_table, state_type, treasury_state);
   auto cfg = mvo(t.treasury(emission_table, emission_type).get_object());
   cfg(field::distributable, cfg[field::floor].as_uint64() + (saturated ? 0 : distributed + remaining));
   t.write_global(emission_table, emission_type, cfg);
   t.reserves(pending, claims);
}
/// Adjust custody with real transfers so available funding is an independently controlled input.
void fund(kicker_tester& t, uint64_t target) {
   const auto current = uint64_t(t.wire_balance(t.SYSIO_ACCOUNT));
   if (current > target)
      BOOST_REQUIRE_EQUAL(t.success(), t.transfer_wire(t.SYSIO_ACCOUNT, funding_account, current - target));
   else if (current < target)
      BOOST_REQUIRE_EQUAL(t.success(), t.transfer_wire(funding_account, t.SYSIO_ACCOUNT, target - current));
}
/// Execute one kick and check the complete model, custody, budget, yield distribution and unchanged treasury.
uint64_t check_kick(kicker_tester& t, state& expected, symbol sym, uint64_t supply, uint64_t wire,
                    uint64_t shadow, uint64_t budget, uint64_t pending, uint64_t claims,
                    uint64_t remaining, uint32_t spacing, bool held) {
   const auto before = t.wire_balance(t.SYSIO_ACCOUNT);
   const auto liq_before = t.wire_balance(t.LIQ_ACCOUNT);
   const auto system_before = t.get_row_by_id(t.SYSIO_ACCOUNT, t.SYSIO_ACCOUNT, state_table, state_table.value);
   const auto pot_before = t.index_row(sym);
   const auto pot = pot_before.is_null() ? 0 : pot_before[field::pot].as_uint64();
   const auto carry = pot_before.is_null() ? 0 : pot_before[field::carry].as_uint64();
   const auto index = pot_before.is_null() ? fc::uint128_t{0} : pot_before[field::index].as_uint128();
   const auto now = t.control->pending_block_time().time_since_epoch().count();
   const auto paid = advance(expected, now, supply, wire, shadow, budget, before,
                             pending, claims, remaining, spacing, held);
   BOOST_REQUIRE_EQUAL(t.success(), t.kick(sym));
   compare(expected, read(t, sym));
   BOOST_REQUIRE_EQUAL(before - paid, t.wire_balance(t.SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(liq_before + paid, t.wire_balance(t.LIQ_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, t.wire_balance(t.KICKER));
   const auto actual_cfg = t.decode(t.kicker_ser, config_type,
      t.get_row_by_id(t.KICKER, t.KICKER, config_table, config_table.value));
   BOOST_REQUIRE_EQUAL(budget - paid, actual_cfg[field::budget].as_uint64());
   BOOST_REQUIRE_EQUAL(spacing, actual_cfg[field::spacing].as_uint64());
   BOOST_REQUIRE(system_before == t.get_row_by_id(t.SYSIO_ACCOUNT, t.SYSIO_ACCOUNT,
      state_table, state_table.value));
   const auto idx = t.index_row(sym);
   BOOST_REQUIRE_EQUAL(pot + paid, idx.is_null() ? 0 : idx[field::pot].as_uint64());
   if (paid) {
      const auto distribution = yield_reference::distribute(paid, supply, carry);
      BOOST_REQUIRE_EQUAL(distribution.carry, idx[field::carry].as_uint64());
      BOOST_REQUIRE(idx[field::index].as_uint128() == index + fc::uint128_t(distribution.index_delta));
   } else {
      BOOST_REQUIRE_EQUAL(carry, idx.is_null() ? 0 : idx[field::carry].as_uint64());
      BOOST_REQUIRE(index == (idx.is_null() ? fc::uint128_t{0} : idx[field::index].as_uint128()));
   }
   return paid;
}
} // namespace

BOOST_AUTO_TEST_SUITE(sysio_kicker_fuzz_tests)
/// Every named boundary owns a fresh fixture and asserts its outcome directly.
BOOST_AUTO_TEST_CASE(kicker_fuzz_boundaries) {
   /// Isolated fixtures for the protocol gates and arithmetic boundaries.
   enum class boundary { treasury_min_minus_one, budget_min_minus_one, available_exact, gift_exact,
                         cap_exact, saturation, reserve_exhausted, overflow, zero_supply, spacing, hold };
   constexpr std::array<boundary, 11> boundaries{boundary::treasury_min_minus_one, boundary::budget_min_minus_one,
      boundary::available_exact, boundary::gift_exact, boundary::cap_exact, boundary::saturation,
      boundary::reserve_exhausted, boundary::overflow, boundary::zero_supply, boundary::spacing, boundary::hold};
   for (unsigned index = 0; index < boundaries.size(); ++index) {
      BOOST_TEST_CONTEXT("boundary=" << magic_enum::enum_name(boundaries[index])) {
         const auto kind = boundaries[index];
         kicker_tester t;
         // Move explicitly to the next UTC noon before any same-day cap test setup.
         const auto seconds = uint64_t(t.control->pending_block_time().time_since_epoch().count()) / micros;
         t.wait(day_seconds - seconds % day_seconds + midday_seconds);
         BOOST_REQUIRE_EQUAL(t.success(), t.push(t.KICKER, t.kicker_ser, t.SYSIO_ACCOUNT, remove_pool_action,
                                                mvo()(field::sym, t.LIQSOL_SYM.to_symbol_code())));
         BOOST_REQUIRE_EQUAL(t.success(), t.add());
         uint64_t minimum = boundary_minimum, budget = boundary_funding, available = boundary_funding;
         uint64_t supply = boundary_supply, wire = unit, shadow = unit;
         uint64_t pending = unit, claims = unit, remaining = unit, cap = 0;
         uint32_t spacing = 1;
         if (kind == boundary::treasury_min_minus_one) available = minimum - 1;
         if (kind == boundary::budget_min_minus_one) budget = minimum - 1;
         if (kind == boundary::available_exact) available = minimum;
         if (kind == boundary::cap_exact) cap = minimum;
         if (kind == boundary::saturation) remaining = 0;
         if (kind == boundary::overflow) { supply = asset_limit; wire = asset_limit; shadow = 1; }
         if (kind == boundary::spacing) spacing = default_spacing;
         BOOST_REQUIRE_EQUAL(t.success(), t.change(basis_points, minimum, cap));
         BOOST_REQUIRE_EQUAL(t.success(), t.configure(budget, spacing));
         fund(t, available + pending + claims + remaining);
         if (kind == boundary::reserve_exhausted)
            remaining = uint64_t(t.wire_balance(t.SYSIO_ACCOUNT)) + unit;
         if (kind != boundary::spacing) t.wait(day_seconds / 4);
         if (kind == boundary::gift_exact) {
            minimum = kicker_test_reference::reference(supply, basis_points,
               t.elapsed() + chain::config::block_interval_us, wire, shadow);
            BOOST_REQUIRE_GT(minimum, 0);
            BOOST_REQUIRE_EQUAL(t.success(), t.change(basis_points, minimum));
         }
         abi_serializer andon_ser;
         namespace cord = sysio_system::test_support::andon;
         if (kind == boundary::hold) {
            cord::deploy(t, andon_ser, contracts::andon_wasm(), contracts::andon_abi());
            BOOST_REQUIRE_EQUAL(t.success(), cord::pull(t, andon_ser));
         }
         if (kind == boundary::spacing) {
            // The next kick lands exactly one block below the inclusive payment threshold.
            const auto target = spacing * micros - chain::config::block_interval_us;
            BOOST_REQUIRE_LT(t.elapsed(), target);
            t.produce_block(fc::microseconds(target - t.elapsed()));
            BOOST_REQUIRE_EQUAL(t.elapsed(), target);
         }
         // Install stress rows after all time-producing setup actions.
         inject_reserves(t, pending, claims, remaining, boundary_distributed, kind == boundary::saturation);
         inject_price(t, t.LIQSOL_SYM, t.POOL_SYM, supply, wire, shadow);
         auto expected = read(t, t.LIQSOL_SYM);
         if (kind == boundary::zero_supply) {
            // Establish real nonzero diagnostics before exercising their zero-supply reset.
            BOOST_REQUIRE_EQUAL(t.success(), t.configure(0, spacing));
            BOOST_REQUIRE_EQUAL(check_kick(t, expected, t.LIQSOL_SYM, supply, wire, shadow, 0,
                                          pending, claims, remaining, spacing, false), 0);
            BOOST_REQUIRE_GT(expected.shortfall, 0);
            BOOST_REQUIRE_GT(expected.observed, 0);
            BOOST_REQUIRE_EQUAL(t.success(), t.configure(budget, spacing));
            supply = 0;
            inject_price(t, t.LIQSOL_SYM, t.POOL_SYM, supply, wire, shadow);
         }
         const auto old = expected;
         const auto gift = kicker_test_reference::wide_reference(supply, expected.rate, t.elapsed(), wire, shadow);
         if (kind == boundary::spacing) BOOST_REQUIRE_GT(gift, minimum);
         const auto paid = check_kick(t, expected, t.LIQSOL_SYM, supply, wire, shadow, budget,
                                     pending, claims, remaining, spacing, kind == boundary::hold);
         if (kind == boundary::treasury_min_minus_one || kind == boundary::budget_min_minus_one ||
             kind == boundary::reserve_exhausted) {
            BOOST_REQUIRE_EQUAL(paid, 0);
            BOOST_REQUIRE_GT(gift, minimum);
            BOOST_REQUIRE_EQUAL(expected.shortfall, uint64_t(gift));
            BOOST_REQUIRE_EQUAL(expected.clock, old.clock);
         } else if (kind == boundary::overflow) {
            BOOST_REQUIRE_EQUAL(paid, 0);
            BOOST_REQUIRE(expected.overflow);
            BOOST_REQUIRE_EQUAL(expected.clock, old.clock);
         } else if (kind == boundary::zero_supply) {
            BOOST_REQUIRE_EQUAL(paid, 0);
            BOOST_REQUIRE_GT(expected.clock, old.clock);
            BOOST_REQUIRE_EQUAL(expected.shortfall, 0);
            BOOST_REQUIRE_EQUAL(expected.observed, 0);
         } else if (kind == boundary::spacing || kind == boundary::hold) {
            BOOST_REQUIRE_EQUAL(paid, 0);
            compare(old, expected);
            BOOST_REQUIRE_GT(gift, minimum);
            if (kind == boundary::spacing) {
               BOOST_REQUIRE_EQUAL(t.elapsed(), spacing * micros);
               BOOST_REQUIRE_GT(check_kick(t, expected, t.LIQSOL_SYM, supply, wire, shadow, budget,
                                          pending, claims, remaining, spacing, false), 0);
            }
            if (kind == boundary::hold) {
               BOOST_REQUIRE_EQUAL(t.success(), cord::clear(t, andon_ser));
               BOOST_REQUIRE_GT(check_kick(t, expected, t.LIQSOL_SYM, supply, wire, shadow, budget,
                                          pending, claims, remaining, spacing, false), 0);
            }
         } else {
            BOOST_REQUIRE_EQUAL(paid, kind == boundary::saturation ? uint64_t(gift) : minimum);
            if (kind == boundary::gift_exact) {
               BOOST_REQUIRE_EQUAL(gift, wide(minimum));
               BOOST_REQUIRE_EQUAL(expected.shortfall, 0);
            }
            if (kind == boundary::saturation) {
               const auto cfg = t.treasury(emission_table, emission_type);
               BOOST_REQUIRE_LT(cfg[field::distributable].as_uint64(),
                                cfg[field::floor].as_uint64() + boundary_distributed);
            }
            if (kind == boundary::cap_exact) {
               BOOST_REQUIRE_EQUAL(expected.spent, cap);
               const auto now = t.control->pending_block_time().time_since_epoch().count();
               BOOST_REQUIRE_EQUAL(expected.day, uint64_t(now) / (day_seconds * micros));
               BOOST_REQUIRE_EQUAL(check_kick(t, expected, t.LIQSOL_SYM, supply, wire, shadow, budget - paid,
                                             pending, claims, remaining, spacing, false), 0);
               BOOST_REQUIRE_GT(expected.shortfall, 0);
            }
         }
      }
   }
}

/// Replay uses the batch's first sequence through the failing sequence: pools share fixture history.
/// Seeds are fixed; random two-pool sequences have no step anchors or fixed hold windows.
BOOST_AUTO_TEST_CASE(kicker_fuzz_sequences) {
   coverage covered;
   for (unsigned batch = 0; batch < sequences; batch += batch_size) {
      BOOST_TEST_CONTEXT("batch=" << batch / batch_size << " first_sequence=" << batch) {
         kicker_tester t;
         const auto eth_pool = symbol::from_string(std::string(precision_prefix) + eth_pair);
         BOOST_REQUIRE_EQUAL(t.success(), t.create(t.LIQETH_SYM, t.ETH, t.LIQETH));
         BOOST_REQUIRE_EQUAL(t.success(), t.regliqpool(t.ETH, t.LIQETH, eth_pool, t.seed, t.seed));
         namespace cord = sysio_system::test_support::andon;
         abi_serializer andon_ser;
         cord::deploy(t, andon_ser, contracts::andon_wasm(), contracts::andon_abi());
         const std::array<symbol, 2> symbols{t.LIQSOL_SYM, t.LIQETH_SYM};
         const std::array<symbol, 2> pairs{t.POOL_SYM, eth_pool};
         bool held = false;
         for (unsigned sequence = batch; sequence < batch + batch_size; ++sequence) {
            const auto seed = seed_base + seed_stride * sequence;
            std::mt19937_64 random(seed);
            BOOST_TEST_CONTEXT("seed=" << seed << " sequence=" << sequence) {
               for (unsigned i = 0; i < symbols.size(); ++i) {
                  if (i == 0 || sequence != batch)
                     BOOST_REQUIRE_EQUAL(t.success(), t.push(t.KICKER, t.kicker_ser, t.SYSIO_ACCOUNT, remove_pool_action,
                                                            mvo()(field::sym, symbols[i].to_symbol_code())));
                  BOOST_REQUIRE_EQUAL(t.success(), t.add(symbols[i]));
               }
               std::array<state, 2> model{read(t, symbols[0]), read(t, symbols[1])};
               for (unsigned step = 0; step < steps; ++step) {
                  BOOST_TEST_CONTEXT("step=" << step) {
                     const auto i = unsigned(random() % symbols.size());
                     auto& expected = model[i];
                     t.wait(intervals[random() % intervals.size()]);
                     if (random() % hold_toggle_denominator == 0) {
                        held = !held;
                        BOOST_REQUIRE_EQUAL(t.success(), held ? cord::pull(t, andon_ser) : cord::clear(t, andon_ser));
                     }
                     expected.rate = random() % (basis_points + 1);
                     expected.minimum = 1 + random() % (minimum_range * unit);
                     expected.cap = random() % exhaustion_denominator == 0 ? 0
                        : expected.minimum + random() % (funding_range * unit);
                     BOOST_REQUIRE_EQUAL(t.success(), t.change(expected.rate, expected.minimum, expected.cap, symbols[i]));
                     const uint32_t spacing = random() % 2 ? random_spacing : 1;
                     const uint64_t budget = random() % exhaustion_denominator == 0
                        ? 0 : random() % (funding_range * unit);
                     BOOST_REQUIRE_EQUAL(t.success(), t.configure(budget, spacing));
                     fund(t, treasury_base * unit + random() % (funding_range * unit));
                     const auto treasury = uint64_t(t.wire_balance(t.SYSIO_ACCOUNT));
                     const uint64_t pending = random() % (reserve_range * unit);
                     const uint64_t claims = random() % (reserve_range * unit);
                     const uint64_t free = random() % (funding_range * unit);
                     const uint64_t remaining = random() % exhaustion_denominator == 0 ? treasury + unit
                        : treasury - pending - claims - free;
                     const uint64_t distributed = 1 + random() % (distributed_range * unit);
                     inject_reserves(t, pending, claims, remaining, distributed);
                     const uint64_t supply = sample_amount(random, supply_scales);
                     const uint64_t shadow = sample_amount(random, price_scales);
                     const uint64_t wire = sample_amount(random, price_scales);
                     inject_price(t, symbols[i], pairs[i], supply, wire, shadow);
                     const auto now = t.control->pending_block_time().time_since_epoch().count();
                     const auto old = expected;
                     auto without_hold = old;
                     const auto payable = advance(without_hold, now, supply, wire, shadow, budget, treasury,
                                                   pending, claims, remaining, spacing, false);
                     auto without_spacing = old;
                     const auto unspaced_payable = advance(without_spacing, now, supply, wire, shadow, budget, treasury,
                                                           pending, claims, remaining, 0, held);
                     const auto gift = kicker_test_reference::wide_reference(supply, old.rate,
                                                                             uint64_t(now - old.clock), wire, shadow);
                     const auto paid = check_kick(t, expected, symbols[i], supply, wire, shadow, budget,
                                                 pending, claims, remaining, spacing, held);
                     compare(model[1 - i], read(t, symbols[1 - i]));
                     const bool priced = !held && uint64_t(now - old.clock) >= spacing * micros;
                     const bool payable_gift = priced && gift >= old.minimum && gift <= asset_limit;
                     covered.random_price += paid > 0 && wire != shadow;
                     covered.full_random_price += paid > 0 && wire != shadow && wide(paid) == gift;
                     covered.partial += paid > 0 && expected.shortfall > 0;
                     covered.subminimum += priced && gift > 0 && gift < old.minimum && paid == 0;
                     covered.unfunded += payable_gift && paid == 0 && budget < old.minimum;
                     covered.reserve_exhausted += payable_gift && paid == 0 &&
                        wide(pending) + claims + remaining >= treasury;
                     covered.cap_exhausted += payable_gift && paid == 0 && old.cap > 0 && old.spent >= old.cap &&
                        old.day == uint64_t(now) / (day_seconds * micros);
                     covered.exclusive_cap += payable_gift && paid == 0 && old.cap > 0 && old.spent >= old.cap &&
                        old.day == uint64_t(now) / (day_seconds * micros) && budget >= old.minimum &&
                        wide(pending) + claims + remaining < treasury &&
                        wide(treasury) - pending - claims - remaining >= old.minimum;
                     covered.held_would_pay += held && payable > 0;
                     covered.spaced_would_pay += uint64_t(now - old.clock) < spacing * micros && unspaced_payable > 0;
                  }
               }
            }
         }
      }
   }
   BOOST_TEST_MESSAGE("coverage random_price=" << covered.random_price
      << " full_random_price=" << covered.full_random_price << " partial=" << covered.partial
      << " subminimum=" << covered.subminimum << " unfunded=" << covered.unfunded
      << " reserve_exhausted=" << covered.reserve_exhausted << " cap_exhausted=" << covered.cap_exhausted
      << " exclusive_cap=" << covered.exclusive_cap
      << " held_would_pay=" << covered.held_would_pay << " spaced_would_pay=" << covered.spaced_would_pay);
   // Floors are about half the measured fixed-seed counts and guard against coverage drift.
   BOOST_REQUIRE_GE(covered.random_price, coverage_floor.random_price);
   BOOST_REQUIRE_GE(covered.full_random_price, coverage_floor.full_random_price);
   BOOST_REQUIRE_GE(covered.exclusive_cap, coverage_floor.exclusive_cap);
   BOOST_REQUIRE_GE(covered.spaced_would_pay, coverage_floor.spaced_would_pay);
   BOOST_REQUIRE_GE(covered.partial, coverage_floor.partial);
   BOOST_REQUIRE_GE(covered.subminimum, coverage_floor.subminimum);
   BOOST_REQUIRE_GE(covered.unfunded, coverage_floor.unfunded);
   BOOST_REQUIRE_GE(covered.reserve_exhausted, coverage_floor.reserve_exhausted);
   BOOST_REQUIRE_GE(covered.cap_exhausted, coverage_floor.cap_exhausted);
   BOOST_REQUIRE_GE(covered.held_would_pay, coverage_floor.held_would_pay);
}
BOOST_AUTO_TEST_SUITE_END()
