#include "../src/underwriter.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <boost/endian/conversion.hpp>
#include <fc/variant_object.hpp>

using namespace sysio::batch_operator_detail::underwriter;
using mvo = fc::mutable_variant_object;

namespace {

const fc::slug_name eth{"ETH"};
const fc::slug_name sol{"SOL"};
const fc::slug_name liqeth{"LIQETH"};
const fc::slug_name liqsol{"LIQSOL"};
/// Cent precision makes each base unit a valid 0.01 bond increment. Production-precision cases below use nine decimals.
const sysio::chain::symbol liqeth_symbol{2, "LIQETH"};
const sysio::chain::symbol liqsol_symbol{2, "LIQSOL"};
const token_symbols        symbols{{liqeth, liqeth_symbol}, {liqsol, liqsol_symbol}};

/// `units` base units of `token`.
sysio::chain::asset units_of(fc::slug_name token, int64_t units) {
   return sysio::chain::asset(units, symbols.at(token));
}
/// `units` base units of LIQETH.
sysio::chain::asset liqeth_units(int64_t units) { return units_of(liqeth, units); }
/// `units` base units of WIRE.
sysio::chain::asset wire_units(int64_t units) { return sysio::chain::asset(units, wire::asset_symbol); }

fc::sha256 digest_of(const std::string& seed) { return fc::sha256::hash(seed); }

/// Statement bytes as `sysio.synd` packs them: chain code, epoch, digest, token code, little-endian.
std::vector<char> statement_bytes(fc::slug_name chain_code, uint32_t epoch, const fc::sha256& digest,
                                  fc::slug_name token) {
   std::vector<char> out(STATEMENT_BYTES);
   auto* p = reinterpret_cast<unsigned char*>(out.data());
   boost::endian::store_little_u64(p, chain_code.value);
   boost::endian::store_little_u32(p + 8, epoch);
   std::memcpy(p + 12, digest.data(), 32);
   boost::endian::store_little_u64(p + 44, token.value);
   return out;
}

/// An OPEN `sysio.synd` envelope request covering `covered` base units of `token`.
request synd_request(uint64_t id, fc::slug_name chain_code, uint32_t epoch, fc::slug_name token, int64_t covered) {
   return request{
      .id         = id,
      .issuer     = sysio::chain::name(synd::account),
      .schema     = sysio::chain::name(envelope_schema),
      .statement  = statement_bytes(chain_code, epoch, digest_of(std::to_string(id)), token),
      .token_code = token,
      .covered    = units_of(token, covered),
      .bonded     = units_of(token, 0),
      .window     = fc::hours(3),
   };
}

/// Inputs where LIQETH has room for 100 with 100 held, and no outpost record has been read.
plan_inputs eth_inputs() {
   plan_inputs in;
   in.caps     = {{liqeth, liqeth_units(100)}};
   in.balances = {{liqeth, liqeth_units(100)}};
   in.now      = fc::time_point::now();
   return in;
}

/// Record, as each outpost would, the digest every request in `in` states: every statement checks out.
void outpost_emitted_all(plan_inputs& in) {
   for (const auto& r : in.requests) {
      if (const auto s = decode_statement(r.statement)) in.emitted[{s->chain_code, s->epoch_index}] = s->digest;
   }
}

const sysio::chain::name underwriter_account{"uw.acct"};

} // namespace

BOOST_AUTO_TEST_SUITE(underwriter_tests)

BOOST_AUTO_TEST_CASE(decode_statement_reads_the_packed_layout) {
   const auto digest = digest_of("envelope");
   const auto s      = decode_statement(statement_bytes(eth, 7, digest, liqeth));
   BOOST_REQUIRE(s.has_value());
   BOOST_CHECK_EQUAL(eth, s->chain_code);
   BOOST_CHECK_EQUAL(7u, s->epoch_index);
   BOOST_CHECK(digest == s->digest);
   BOOST_CHECK_EQUAL(liqeth, s->token_code);

   auto bytes = statement_bytes(eth, 7, digest, liqeth);
   bytes.pop_back();
   BOOST_CHECK(!decode_statement(bytes).has_value());
   bytes.resize(STATEMENT_BYTES + 1);
   BOOST_CHECK(!decode_statement(bytes).has_value());
}

BOOST_AUTO_TEST_CASE(decode_request_reads_a_rendered_row) {
   const auto statement = statement_bytes(eth, 3, digest_of("e3"), liqeth);
   const auto row = mvo()
      ("id", 42)("issuer", "sysio.synd")("schema", "oppenvelope")
      ("statement", fc::to_hex(statement.data(), statement.size()))
      ("statement_digest", digest_of("sd"))("token_code", "LIQETH")("covered", 300)("bonded", 100)
      ("bounty", 0)("window_sec", 10800)("state", "BONDED")("created_at", "2026-10-07T00:00:00.000")
      ("bonded_at", "2026-10-07T00:00:05.000")("outcome_acknowledged", true);
   const auto r = decode_request(row, symbols);
   BOOST_REQUIRE(r.has_value());
   BOOST_CHECK_EQUAL(42u, r->id);
   BOOST_CHECK_EQUAL(std::string("sysio.synd"), r->issuer.to_string());
   BOOST_CHECK_EQUAL(std::string("oppenvelope"), r->schema.to_string());
   BOOST_CHECK(statement == r->statement);
   BOOST_CHECK_EQUAL(liqeth, r->token_code);
   BOOST_CHECK_EQUAL(liqeth_units(300), r->covered);
   BOOST_CHECK_EQUAL(liqeth_units(100), r->bonded);
   BOOST_CHECK(fc::seconds(10800) == r->window);
   BOOST_CHECK(request_state::BONDED == r->state);
   BOOST_CHECK(fc::time_point::from_iso_string("2026-10-07T00:00:05.000") == r->bonded_at);
   BOOST_CHECK(r->outcome_acknowledged);

   auto unknown_state = row;
   unknown_state.set("state", 9);   // a value the enum does not name renders as its integer
   BOOST_CHECK(!decode_request(unknown_state, symbols).has_value());
   auto missing = row;
   missing.erase("covered");
   BOOST_CHECK(!decode_request(missing, symbols).has_value());
   // A token with no known symbol, and an amount no asset can carry.
   BOOST_CHECK(!decode_request(row, token_symbols{{liqsol, liqsol_symbol}}).has_value());
   auto huge = row;
   huge.set("covered", std::numeric_limits<uint64_t>::max());
   BOOST_CHECK(!decode_request(huge, symbols).has_value());
}

BOOST_AUTO_TEST_CASE(decode_bond_keeps_only_the_underwriters_rows) {
   const std::vector<request> requests{synd_request(5, eth, 5, liqeth, 100)};
   const auto row = mvo()("request_id", 5)("underwriter", "uw.acct")("amount", 70)
                       ("yield", mvo()("index_checkpoint", "0")("owed_wire", 7))("paid", false);
   const auto ours = decode_bond(row, underwriter_account, requests);
   BOOST_REQUIRE(ours.has_value());
   BOOST_CHECK_EQUAL(5u, ours->request_id);
   BOOST_CHECK_EQUAL(liqeth_units(70), ours->amount);   // in its request's token
   BOOST_CHECK(!ours->paid);
   BOOST_CHECK_EQUAL(wire_units(7), ours->owed_wire);
   BOOST_CHECK(!decode_bond(row, sysio::chain::name{"someone"}, requests).has_value());
}

BOOST_AUTO_TEST_CASE(a_bond_row_of_ours_that_does_not_decode_stops_the_pass) {
   const std::vector<request> requests{synd_request(5, eth, 5, liqeth, 100)};
   // Ours without its yield position, and a row naming no underwriter: neither may drop out of the exposure.
   BOOST_CHECK_THROW(decode_bond(mvo()("request_id", 5)("underwriter", "uw.acct")("amount", 70)("paid", false),
                                 underwriter_account, requests),
                     fc::exception);
   BOOST_CHECK_THROW(decode_bond(mvo()("request_id", 5)("amount", 70)("paid", false), underwriter_account, requests),
                     fc::exception);
   // Someone else's malformed row is not ours to read.
   BOOST_CHECK(
      !decode_bond(mvo()("request_id", 5)("underwriter", "someone"), underwriter_account, requests).has_value());
}

BOOST_AUTO_TEST_CASE(a_bond_of_ours_without_its_request_stops_the_pass) {
   // Its request's row did not decode, so its amount belongs to no token's cap: paid or not, the pass stops.
   const std::vector<request> requests{synd_request(1, eth, 1, liqeth, 10)};
   const auto bond_on = [](uint64_t id, bool paid) {
      return mvo()("request_id", id)("underwriter", "uw.acct")("amount", 10)
                  ("yield", mvo()("index_checkpoint", "0")("owed_wire", 0))("paid", paid);
   };
   BOOST_CHECK(decode_bond(bond_on(1, false), underwriter_account, requests).has_value());
   BOOST_CHECK_THROW(decode_bond(bond_on(3, false), underwriter_account, requests), fc::exception);
   BOOST_CHECK_THROW(decode_bond(bond_on(3, true), underwriter_account, requests), fc::exception);
}

BOOST_AUTO_TEST_CASE(synd_has_work_while_a_request_is_in_play_or_unacknowledged) {
   auto settled = synd_request(1, eth, 1, liqeth, 10);
   settled.state                = request_state::APPROVED;
   settled.outcome_acknowledged = true;
   BOOST_CHECK(!synd_has_work({settled}));
   BOOST_CHECK(synd_has_work({settled, synd_request(2, eth, 2, liqeth, 10)}));   // OPEN
   auto ruled = synd_request(3, eth, 3, liqeth, 10);
   ruled.state = request_state::INVALID;   // not yet acknowledged
   BOOST_CHECK(synd_has_work({settled, ruled}));
   auto other_issuer = synd_request(4, eth, 4, liqeth, 10);
   other_issuer.issuer = sysio::chain::name{"someone"};
   BOOST_CHECK(!synd_has_work({settled, other_issuer}));
}

BOOST_AUTO_TEST_CASE(the_scan_window_keeps_every_request_still_needed) {
   const auto settled = [](uint64_t id) {
      auto r                 = synd_request(id, eth, static_cast<uint32_t>(id), liqeth, 10);
      r.state                = request_state::APPROVED;
      r.outcome_acknowledged = true;
      return r;
   };
   auto in_play  = synd_request(3, eth, 3, liqeth, 10);
   in_play.state = request_state::BONDED;
   const std::vector<request> requests{settled(1), settled(2), in_play, settled(4)};
   BOOST_CHECK_EQUAL(3u, next_scan_start(0, requests, {}, {}));
   // Nothing needs a later look: one past the newest read.
   BOOST_CHECK_EQUAL(5u, next_scan_start(0, {settled(1), settled(4)}, {}, {}));
   // A bond row of ours, or a request remembered as bonded, keeps its request in the window.
   BOOST_CHECK_EQUAL(2u, next_scan_start(0, {settled(1), settled(2), settled(4)},
                                         {{2, {.request_id = 2, .amount = liqeth_units(10), .paid = true,
                                               .owed_wire = wire_units(1)}}},
                                         {}));
   BOOST_CHECK_EQUAL(1u, next_scan_start(0, {settled(1), settled(4)}, {}, {1}));
   // Another issuer's open request does not pin the window, and an empty read keeps it where it was.
   auto other_issuer   = synd_request(0, eth, 0, liqeth, 10);
   other_issuer.issuer = sysio::chain::name{"someone"};
   BOOST_CHECK_EQUAL(2u, next_scan_start(0, {other_issuer, settled(1)}, {}, {}));
   BOOST_CHECK_EQUAL(7u, next_scan_start(7, {}, {}, {}));
}

BOOST_AUTO_TEST_CASE(a_prune_sweep_moves_past_what_each_prune_examines) {
   constexpr uint32_t limit = 3;
   // More rows than one prune examines: the next one starts at the first id it leaves.
   prune_sweep sweep{.from = 2, .last = 9};
   sweep.advance({.ids = {2, 4, 7, 9}, .more = true}, limit);
   BOOST_CHECK_EQUAL(9u, sweep.from);
   BOOST_CHECK_EQUAL(9u, sweep.last.value());
   // A read cut short before the id after the prune's: the next one starts where this one did.
   sweep.advance({.ids = {9, 11}, .more = true}, limit);
   BOOST_CHECK_EQUAL(9u, sweep.from);
   // The first id left is newer than the sweep: a new sweep starts at 0 and reads its own newest id.
   sweep.advance({.ids = {9, 11, 12, 14}}, limit);
   BOOST_CHECK_EQUAL(0u, sweep.from);
   BOOST_CHECK(!sweep.last.has_value());
   // The prune reaches the end of the table: a new sweep, too.
   sweep = {.from = 2, .last = 7};
   sweep.advance({.ids = {2, 4, 7}}, limit);
   BOOST_CHECK_EQUAL(0u, sweep.from);
   BOOST_CHECK(!sweep.last.has_value());
   sweep = {.from = 0, .last = 0};   // an empty table
   sweep.advance({}, limit);
   BOOST_CHECK_EQUAL(0u, sweep.from);
   BOOST_CHECK(!sweep.last.has_value());
}

// Requests arrive faster than one prune per interval examines, and each becomes prunable a retention period after it
// arrives. Every sweep still returns to the oldest requests, so they are pruned once eligible.
BOOST_AUTO_TEST_CASE(a_prune_sweep_returns_to_old_requests_under_sustained_intake) {
   constexpr uint32_t limit             = 64;
   constexpr uint32_t arrivals          = limit + 1;   // per interval
   constexpr uint32_t retention         = 7 * 24 * 6;  // intervals of 10 minutes: seven days
   constexpr uint32_t intervals         = 3 * retention;
   std::map<request_id_t, uint32_t> table;              // request id -> interval it arrived in
   request_id_t                     next_id = 1;
   prune_sweep                      sweep;
   uint32_t                         sweeps = 0, removed = 0;
   for (uint32_t now = 0; now < intervals; ++now) {
      for (uint32_t i = 0; i < arrivals; ++i) table.emplace(next_id++, now);
      if (!sweep.last) sweep.last = table.empty() ? 0 : table.rbegin()->first;
      request_id_page page;
      auto            it = table.lower_bound(sweep.from);
      for (; it != table.end() && page.ids.size() <= limit; ++it) page.ids.push_back(it->first);
      page.more = it != table.end();
      // The prune: the first `limit` requests from `from` go once their retention period has passed.
      for (size_t i = 0; i < std::min<size_t>(limit, page.ids.size()); ++i) {
         if (now - table.at(page.ids[i]) >= retention) {
            table.erase(page.ids[i]);
            ++removed;
         }
      }
      sweep.advance(page, limit);
      if (sweep.from == 0) ++sweeps;
   }
   BOOST_CHECK(!table.contains(1));
   BOOST_CHECK_GT(removed, 0u);
   BOOST_CHECK_GT(sweeps, 1u);
}

BOOST_AUTO_TEST_CASE(bonded_requests_are_remembered_until_their_row_is_pruned) {
   const auto r1 = synd_request(1, eth, 1, liqeth, 10);
   const auto r2 = synd_request(2, eth, 2, liqeth, 10);
   std::set<uint64_t> bonded;
   remember_bonded(bonded, {{1, {.request_id = 1}}, {2, {.request_id = 2}}}, {r1, r2});
   BOOST_CHECK((bonded == std::set<uint64_t>{1, 2}));
   remember_bonded(bonded, {{1, {.request_id = 1}}}, {r1, r2});   // bond row 2 pruned, its request still there
   BOOST_CHECK((bonded == std::set<uint64_t>{1, 2}));
   remember_bonded(bonded, {{1, {.request_id = 1}}}, {r1});       // request 2 pruned too
   BOOST_CHECK((bonded == std::set<uint64_t>{1}));
}

BOOST_AUTO_TEST_CASE(only_a_forfeit_after_the_first_pass_halts_bonding) {
   forfeit_watch watch;
   BOOST_CHECK(!watch.note({4}));   // on chain at startup: the restart resumed bonding
   BOOST_CHECK(!watch.note({4}));
   BOOST_CHECK(!watch.halted_by.has_value());
   BOOST_CHECK(watch.note({4, 7}));
   BOOST_CHECK_EQUAL(7u, watch.halted_by.value());
   BOOST_CHECK(!watch.note({4, 7, 9}));   // already halted
   BOOST_CHECK_EQUAL(7u, watch.halted_by.value());
}

BOOST_AUTO_TEST_CASE(an_unreadable_chain_holds_back_only_its_own_reads) {
   auto bonded_already = synd_request(4, eth, 9, liqeth, 10);
   bonded_already.state = request_state::BONDED;
   auto full = synd_request(5, eth, 10, liqeth, 10);
   full.bonded = full.covered;
   const auto wanted = wanted_envelopes({synd_request(1, eth, 7, liqeth, 10), synd_request(2, eth, 8, liqeth, 10),
                                         synd_request(3, sol, 3, liqsol, 10), bonded_already, full});
   BOOST_CHECK((wanted == std::set<envelope_key>{{eth, 7}, {eth, 8}, {sol, 3}}));

   std::vector<envelope_key>  reads;
   std::vector<fc::slug_name> failures;
   const auto emitted = collect_emitted(
      wanted,
      [&](fc::slug_name chain, uint32_t epoch) -> std::optional<fc::sha256> {
         reads.emplace_back(chain, epoch);
         if (chain == eth) FC_THROW_EXCEPTION(fc::exception, "rpc down");
         return digest_of("sol3");
      },
      [&](fc::slug_name chain, const std::string&) { failures.push_back(chain); });
   BOOST_CHECK((reads == std::vector<envelope_key>{{eth, 7}, {sol, 3}}));   // ETH is not read again this pass
   BOOST_CHECK((failures == std::vector<fc::slug_name>{eth}));
   BOOST_REQUIRE_EQUAL(1u, emitted.size());
   BOOST_CHECK(emitted.at({sol, 3}) == digest_of("sol3"));
}

BOOST_AUTO_TEST_CASE(a_statement_the_outpost_recorded_is_bonded_for_its_remainder) {
   auto in = eth_inputs();
   auto r  = synd_request(1, eth, 10, liqeth, 60);
   r.bonded = liqeth_units(20);   // someone else bonded part of it
   in.requests = {r};
   outpost_emitted_all(in);
   const auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(1u, out.accepts[0].request_id);
   BOOST_CHECK_EQUAL(liqeth_units(40), out.accepts[0].amount);
   BOOST_CHECK(out.waiting.empty());
}

BOOST_AUTO_TEST_CASE(five_underwriters_with_two_liqsol_each_can_cover_six) {
   const sysio::chain::symbol sym{9, "LIQSOL"};
   constexpr int64_t unit = 1'000'000'000;
   auto r = synd_request(1, sol, 1, liqsol, 0);
   r.covered = sysio::chain::asset(6 * unit, sym);
   r.bonded  = sysio::chain::asset(0, sym);
   std::vector<plan_inputs> providers(5);
   for (auto& in : providers) {
      in.requests = {r};
      in.caps     = {{liqsol, sysio::chain::asset(2 * unit, sym)}};
      in.balances = {{liqsol, sysio::chain::asset(10 * unit, sym)}};
      outpost_emitted_all(in);
   }

   for (size_t i = 0; i < providers.size(); ++i) {
      auto& in = providers[i];
      in.requests = {r};
      const auto out = plan_actions(in);
      if (i >= 3) {
         BOOST_CHECK(out.accepts.empty());
         continue;
      }
      BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
      const auto& contribution = out.accepts.front();
      BOOST_CHECK_EQUAL(r.id, contribution.request_id);
      BOOST_CHECK_EQUAL(sysio::chain::asset(2 * unit, sym), contribution.amount);
      r.bonded += contribution.amount;
      if (r.bonded == r.covered) r.state = request_state::BONDED;

      // A later pass must count this provider's partial bond against its cap.
      in.bonds = {{r.id, {.request_id = r.id, .amount = contribution.amount}}};
      in.balances.at(liqsol) -= contribution.amount;
      in.requests = {r};
      const auto next = plan_actions(in);
      BOOST_CHECK(next.accepts.empty());
      if (r.state == request_state::OPEN) {
         BOOST_REQUIRE_EQUAL(1u, next.waiting.size());
         BOOST_CHECK(wait_reason::OVER_CAP == next.waiting.front().reason);
      }
   }
   BOOST_CHECK_EQUAL(r.covered, r.bonded);
   BOOST_CHECK(r.state == request_state::BONDED);
}

BOOST_AUTO_TEST_CASE(partial_contributions_round_down_to_the_contract_increment) {
   const sysio::chain::symbol sym{9, "LIQSOL"};
   constexpr int64_t unit = 1'000'000'000;
   constexpr int64_t cent = unit / 100;
   /// Available funds and the contribution, or reason to wait, expected from one pass.
   struct funding_case {
      int64_t cap;
      int64_t balance;
      int64_t expected;
      wait_reason reason = wait_reason::OVER_CAP;
   };
   const std::vector<funding_case> cases{
      {2 * unit + cent - 1, 6 * unit, 2 * unit},
      {6 * unit, 2 * unit + cent - 1, 2 * unit},
      {6 * unit, cent, cent},
      {cent - 1, 6 * unit, 0, wait_reason::OVER_CAP},
      {6 * unit, cent - 1, 0, wait_reason::LOW_BALANCE},
      {0, 6 * unit, 0, wait_reason::OVER_CAP},
      {6 * unit, 0, 0, wait_reason::LOW_BALANCE},
   };
   for (const auto& c : cases) {
      BOOST_TEST_CONTEXT("cap=" << c.cap << " balance=" << c.balance) {
         auto r = synd_request(1, sol, 1, liqsol, 0);
         r.covered = sysio::chain::asset(6 * unit, sym);
         r.bonded  = sysio::chain::asset(0, sym);
         plan_inputs in;
         in.requests = {r};
         in.caps     = {{liqsol, sysio::chain::asset(c.cap, sym)}};
         in.balances = {{liqsol, sysio::chain::asset(c.balance, sym)}};
         outpost_emitted_all(in);
         const auto out = plan_actions(in);
         if (c.expected == 0) {
            BOOST_CHECK(out.accepts.empty());
            BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
            BOOST_CHECK(c.reason == out.waiting.front().reason);
         } else {
            BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
            BOOST_CHECK_EQUAL(sysio::chain::asset(c.expected, sym), out.accepts.front().amount);
            BOOST_CHECK(out.waiting.empty());
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(a_cap_lowered_below_existing_exposure_waits) {
   auto in = eth_inputs();
   auto existing = synd_request(1, eth, 1, liqeth, 100);
   existing.state = request_state::APPROVED;
   in.requests = {existing, synd_request(2, eth, 2, liqeth, 10)};
   in.bonds = {{1, {.request_id = 1, .amount = liqeth_units(100)}}};
   in.caps.at(liqeth) = liqeth_units(90);
   outpost_emitted_all(in);
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::OVER_CAP == out.waiting.front().reason);
}

BOOST_AUTO_TEST_CASE(the_contract_and_planner_share_increment_boundaries) {
   static_assert(!sysio::bond_increment::for_decimals(1));
   static_assert(sysio::bond_increment::for_decimals(2).value() == 1);
   static_assert(sysio::bond_increment::for_decimals(9).value() == 10'000'000);
   BOOST_CHECK(!sysio::bond_increment::for_decimals(0));
   BOOST_CHECK(!sysio::bond_increment::for_decimals(19));
   BOOST_CHECK_EQUAL(10'000'000'000'000'000ULL, sysio::bond_increment::for_decimals(18).value());
}

BOOST_AUTO_TEST_CASE(a_partial_bond_can_be_topped_up_when_capacity_frees_up) {
   auto in = eth_inputs();
   auto open = synd_request(1, eth, 1, liqeth, 600);
   open.bonded = liqeth_units(200);
   auto approved = synd_request(2, eth, 2, liqeth, 200);
   approved.state = request_state::APPROVED;
   in.requests = {open, approved};
   in.bonds = {{1, {.request_id = 1, .amount = liqeth_units(200)}},
               {2, {.request_id = 2, .amount = liqeth_units(200)}}};
   in.caps.at(liqeth) = liqeth_units(400);
   in.balances.at(liqeth) = liqeth_units(1000);
   outpost_emitted_all(in);
   auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::OVER_CAP == out.waiting.front().reason);

   in.bonds.at(2).paid = true;
   out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(liqeth_units(200), out.accepts.front().amount);
   BOOST_CHECK_EQUAL(liqeth_units(200), out.accepts.front().bonded_before);
   in.caps.at(liqeth) = liqeth_units(200);
   out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::OVER_CAP == out.waiting.front().reason);
}

BOOST_AUTO_TEST_CASE(a_partial_accept_is_not_repeated_while_lib_lags) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 1, liqeth, 600)};
   in.caps.at(liqeth) = liqeth_units(200);
   in.balances.at(liqeth) = liqeth_units(1000);
   outpost_emitted_all(in);
   const auto first = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, first.accepts.size());
   BOOST_CHECK_EQUAL(liqeth_units(200), first.accepts.front().amount);
   const fc::time_point_sec expiration(in.now + fc::seconds(30));
   remember_submitted_accept(in.pending_accepts, first.accepts.front(), expiration);

   // Both an unchanged LIB and a newer LIB short of the inclusion block still show the old row and wallet.
   for (const auto advance : {fc::seconds(0), fc::seconds(1), fc::seconds(15)}) {
      in.now += advance;
      const auto next = plan_actions(in);
      BOOST_CHECK(next.accepts.empty());
      BOOST_REQUIRE_EQUAL(1u, next.pending_accepts.size());
      BOOST_REQUIRE_EQUAL(1u, next.waiting.size());
      BOOST_CHECK(wait_reason::PENDING_ACCEPT == next.waiting.front().reason);
   }
}

BOOST_AUTO_TEST_CASE(submitted_accepts_reserve_both_exposure_and_wallet_for_other_requests) {
   for (const bool cap_limited : {true, false}) {
      BOOST_TEST_CONTEXT("cap_limited=" << cap_limited) {
         auto in = eth_inputs();
         in.requests = {synd_request(1, eth, 1, liqeth, 100), synd_request(2, eth, 2, liqeth, 600),
                        synd_request(3, eth, 3, liqeth, 100)};
         in.caps.at(liqeth) = liqeth_units(cap_limited ? 300 : 1000);
         in.balances.at(liqeth) = liqeth_units(cap_limited ? 1000 : 300);
         outpost_emitted_all(in);
         const auto first = plan_actions(in);
         BOOST_REQUIRE_EQUAL(2u, first.accepts.size());
         BOOST_CHECK_EQUAL(liqeth_units(100), first.accepts[0].amount);   // fully covers the first request
         BOOST_CHECK_EQUAL(liqeth_units(200), first.accepts[1].amount);   // partially covers the second
         for (const auto& accept : first.accepts) {
            remember_submitted_accept(in.pending_accepts, accept, fc::time_point_sec(in.now + fc::seconds(30)));
         }
         in.now += fc::seconds(1);
         const auto next = plan_actions(in);
         BOOST_CHECK(next.accepts.empty());
         BOOST_REQUIRE_EQUAL(3u, next.waiting.size());
         BOOST_CHECK(wait_reason::PENDING_ACCEPT == next.waiting[0].reason);
         BOOST_CHECK(wait_reason::PENDING_ACCEPT == next.waiting[1].reason);
         BOOST_CHECK((cap_limited ? wait_reason::OVER_CAP : wait_reason::LOW_BALANCE) == next.waiting[2].reason);
      }
   }
}

BOOST_AUTO_TEST_CASE(a_visible_partial_bond_replaces_its_submission_reservation) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 1, liqeth, 600)};
   in.caps.at(liqeth) = liqeth_units(400);
   in.balances.at(liqeth) = liqeth_units(1000);
   // Our row held 2 and another provider bonded 1 before this 2-token top-up was submitted.
   in.requests.front().bonded = liqeth_units(300);
   in.bonds = {{1, {.request_id = 1, .amount = liqeth_units(200)}}};
   outpost_emitted_all(in);
   const auto first = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, first.accepts.size());
   remember_submitted_accept(in.pending_accepts, first.accepts.front(), fc::time_point_sec(in.now + fc::seconds(30)));
   BOOST_CHECK(plan_actions(in).accepts.empty());

   in.requests.front().bonded = liqeth_units(500);
   in.bonds.at(1).amount = liqeth_units(400);
   in.balances.at(liqeth) = liqeth_units(800);
   auto next = plan_actions(in);
   BOOST_CHECK(next.pending_accepts.empty());
   BOOST_CHECK(next.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, next.waiting.size());
   BOOST_CHECK(wait_reason::OVER_CAP == next.waiting.front().reason);
   in.caps.at(liqeth) = liqeth_units(600);
   next = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, next.accepts.size());
   BOOST_CHECK_EQUAL(liqeth_units(100), next.accepts.front().amount);
   BOOST_CHECK_EQUAL(liqeth_units(400), next.accepts.front().bonded_before);
}

BOOST_AUTO_TEST_CASE(unobserved_submissions_expire_only_after_lib_passes_the_signed_expiration) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 1, liqeth, 600)};
   in.caps.at(liqeth) = liqeth_units(200);
   in.balances.at(liqeth) = liqeth_units(1000);
   outpost_emitted_all(in);
   const auto first = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, first.accepts.size());
   // No submission (for example, signing failed) reserves nothing. A submitted but unconfirmed push does.
   remember_submitted_accept(in.pending_accepts, first.accepts.front(), std::nullopt);
   BOOST_CHECK(in.pending_accepts.empty());
   const fc::time_point_sec expiration(in.now + fc::seconds(30));
   remember_submitted_accept(in.pending_accepts, first.accepts.front(), expiration);
   in.now = expiration.to_time_point();
   auto next = plan_actions(in);
   BOOST_CHECK(next.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, next.pending_accepts.size());
   in.now += fc::milliseconds(1);
   next = plan_actions(in);
   BOOST_CHECK(next.pending_accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, next.accepts.size());
   BOOST_CHECK_EQUAL(liqeth_units(200), next.accepts.front().amount);
}

BOOST_AUTO_TEST_CASE(a_request_leaving_open_releases_a_clamped_submission_reservation) {
   for (const auto state : {request_state::BONDED, request_state::HELD, request_state::APPROVED,
                            request_state::VALID, request_state::INVALID}) {
      auto in = eth_inputs();
      auto r = synd_request(1, eth, 1, liqeth, 600);
      r.state = state;
      in.requests = {r};
      in.bonds = {{1, {.request_id = 1, .amount = liqeth_units(100)}}};
      // Other providers filled most of the remainder, so the contract accepted less than the submitted 2.
      in.pending_accepts = {{1, {liqeth, liqeth_units(200), liqeth_units(0),
                                 fc::time_point_sec(in.now + fc::seconds(30))}}};
      BOOST_CHECK(plan_actions(in).pending_accepts.empty());
   }
}

BOOST_AUTO_TEST_CASE(a_missing_request_does_not_drop_a_live_reservation_or_advance_the_scan_past_it) {
   auto in = eth_inputs();
   in.requests = {synd_request(2, eth, 2, liqeth, 600)};
   in.caps.at(liqeth) = liqeth_units(200);
   in.balances.at(liqeth) = liqeth_units(1000);
   in.pending_accepts = {{1, {liqeth, liqeth_units(200), liqeth_units(0),
                              fc::time_point_sec(in.now + fc::seconds(30))}}};
   outpost_emitted_all(in);
   const auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.pending_accepts.size());
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::OVER_CAP == out.waiting.front().reason);
   BOOST_CHECK_EQUAL(1u, next_scan_start(2, in.requests, in.bonds, {}, out.pending_accepts));
}

BOOST_AUTO_TEST_CASE(startup_wait_covers_failover_to_an_older_lib) {
   auto in = eth_inputs();
   in.now = fc::time_point_sec(in.now).to_time_point();
   in.requests = {synd_request(1, eth, 1, liqeth, 10)};
   outpost_emitted_all(in);
   const auto transaction_lifetime = fc::seconds(30);
   const auto startup_lib = in.now;
   const auto old_node_lib = startup_lib + fc::seconds(10);
   const auto startup_wall_time = startup_lib + fc::seconds(20);
   const fc::time_point_sec old_submission_expiration(old_node_lib + transaction_lifetime);
   in.accepts_after = startup_accept_deadline(startup_lib, startup_wall_time, transaction_lifetime);
   BOOST_CHECK(fc::time_point_sec(startup_wall_time + transaction_lifetime) == *in.accepts_after);
   // When LIB is ahead of wall time, its transaction expiration remains the conservative bound.
   BOOST_CHECK(*in.accepts_after == startup_accept_deadline(startup_wall_time, startup_lib, transaction_lifetime));

   // The old LIB-only startup wait has elapsed, but the previous node's accept can still execute.
   in.now = startup_lib + transaction_lifetime + fc::seconds(1);
   BOOST_REQUIRE(in.now < old_submission_expiration.to_time_point());
   auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::STARTUP_FINALITY == out.waiting.front().reason);
   in.now = in.accepts_after->to_time_point();
   BOOST_CHECK(plan_actions(in).accepts.empty());
   in.now += fc::milliseconds(1);
   BOOST_REQUIRE_EQUAL(1u, plan_actions(in).accepts.size());
}

BOOST_AUTO_TEST_CASE(startup_and_unsupported_precision_wait_without_blocking_approvals_or_claims) {
   auto in = eth_inputs();
   auto bonded = synd_request(2, eth, 2, liqeth, 10);
   bonded.state = request_state::BONDED;
   bonded.bonded_at = in.now - bonded.window;
   auto approved = synd_request(3, eth, 3, liqeth, 10);
   approved.state = request_state::APPROVED;
   in.requests = {synd_request(1, eth, 1, liqeth, 10), bonded, approved};
   in.bonds = {{2, {.request_id = 2, .amount = liqeth_units(10)}},
               {3, {.request_id = 3, .amount = liqeth_units(10)}}};
   outpost_emitted_all(in);
   in.accepts_after = fc::time_point_sec(in.now + fc::seconds(30));
   auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::STARTUP_FINALITY == out.waiting.front().reason);
   BOOST_CHECK(out.approves == std::vector<uint64_t>{2});
   BOOST_CHECK(out.claims == std::vector<uint64_t>{3});
   in.now = in.accepts_after->to_time_point();
   BOOST_CHECK(plan_actions(in).accepts.empty());
   in.now += fc::milliseconds(1);
   BOOST_REQUIRE_EQUAL(1u, plan_actions(in).accepts.size());

   const sysio::chain::symbol unsupported{1, "LIQETH"};
   in.requests.front().covered = sysio::chain::asset(10, unsupported);
   in.requests.front().bonded = sysio::chain::asset(0, unsupported);
   out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::INVALID_PRECISION == out.waiting.front().reason);
   BOOST_CHECK(out.approves == std::vector<uint64_t>{2});
   BOOST_CHECK(out.claims == std::vector<uint64_t>{3});
}

BOOST_AUTO_TEST_CASE(a_statement_without_an_outpost_record_waits) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 11, liqeth, 10), synd_request(2, sol, 1, liqeth, 10)};
   // A record of another epoch, and one of another chain, prove neither statement.
   in.emitted[{eth, 10}] = digest_of("1");
   in.emitted[{sol, 2}]  = digest_of("2");
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(2u, out.waiting.size());
   BOOST_CHECK(wait_reason::UNVERIFIED == out.waiting[0].reason);
   BOOST_CHECK(wait_reason::UNVERIFIED == out.waiting[1].reason);
}

BOOST_AUTO_TEST_CASE(a_digest_the_outpost_did_not_record_is_never_bonded) {
   auto in = eth_inputs();
   const auto r = synd_request(1, eth, 7, liqeth, 10);   // states digest_of("1") for ETH epoch 7
   in.requests = {r};
   // Right chain and epoch, another digest: the depot accepted an envelope the outpost never emitted.
   in.emitted[{eth, 7}] = digest_of("forged");
   auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::CONTRADICTED == out.waiting[0].reason);

   // Right chain and epoch, no record at all: it waits.
   in.emitted.clear();
   out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::UNVERIFIED == out.waiting[0].reason);

   // The outpost's record carries the statement's digest: it is bonded.
   in.emitted[{eth, 7}] = decode_statement(r.statement)->digest;
   out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK(out.waiting.empty());
}

BOOST_AUTO_TEST_CASE(the_cap_counts_unsettled_bonds_and_this_pass_oldest_first) {
   auto in = eth_inputs();
   // 30 already bonded on an approved request not yet claimed counts; a claimed bond does not.
   auto approved = synd_request(1, eth, 1, liqeth, 30);
   approved.state = request_state::APPROVED;
   auto settled = synd_request(2, eth, 2, liqeth, 50);
   settled.state = request_state::APPROVED;
   in.bonds = {{1, {.request_id = 1, .amount = liqeth_units(30)}},
               {2, {.request_id = 2, .amount = liqeth_units(50), .paid = true}}};
   // Listed newest first: the plan still takes request 3 before request 4.
   in.requests = {approved, settled, synd_request(5, eth, 5, liqeth, 10), synd_request(4, eth, 4, liqeth, 40),
                  synd_request(3, eth, 3, liqeth, 60)};
   outpost_emitted_all(in);
   const auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(2u, out.accepts.size());
   BOOST_CHECK_EQUAL(3u, out.accepts[0].request_id);   // 30 + 60 = 90 fits under 100
   BOOST_CHECK_EQUAL(liqeth_units(60), out.accepts[0].amount);
   BOOST_CHECK_EQUAL(4u, out.accepts[1].request_id);   // only the remaining 10 of capacity is contributed
   BOOST_CHECK_EQUAL(liqeth_units(10), out.accepts[1].amount);
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK_EQUAL(5u, out.waiting[0].request_id);   // the cap is now exhausted
   BOOST_CHECK(wait_reason::OVER_CAP == out.waiting[0].reason);
   BOOST_CHECK(out.claims == std::vector<uint64_t>{1});
}

BOOST_AUTO_TEST_CASE(no_cap_or_too_little_balance_waits) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 1, liqsol, 10)};
   outpost_emitted_all(in);
   auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::NO_CAP == out.waiting[0].reason);

   in = eth_inputs();
   in.balances = {{liqeth, liqeth_units(50)}};
   in.requests = {synd_request(1, eth, 1, liqeth, 40), synd_request(2, eth, 2, liqeth, 40),
                  synd_request(3, eth, 3, liqeth, 10)};
   outpost_emitted_all(in);
   out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(2u, out.accepts.size());
   BOOST_CHECK_EQUAL(liqeth_units(40), out.accepts[0].amount);
   BOOST_CHECK_EQUAL(liqeth_units(10), out.accepts[1].amount);
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK_EQUAL(3u, out.waiting[0].request_id);
   BOOST_CHECK(wait_reason::LOW_BALANCE == out.waiting[0].reason);

   in.balances.clear();   // no balance of the token at all
   out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(3u, out.waiting.size());
   BOOST_CHECK(wait_reason::LOW_BALANCE == out.waiting[0].reason);
}

BOOST_AUTO_TEST_CASE(a_malformed_or_mismatched_statement_waits) {
   auto in = eth_inputs();
   auto bad = synd_request(1, eth, 1, liqeth, 10);
   bad.statement.pop_back();
   auto mismatch = synd_request(2, eth, 2, liqeth, 10);
   mismatch.token_code = liqsol;
   in.requests = {bad, mismatch};
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(2u, out.waiting.size());
   BOOST_CHECK(wait_reason::BAD_STATEMENT == out.waiting[0].reason);
   BOOST_CHECK(wait_reason::TOKEN_MISMATCH == out.waiting[1].reason);
}

BOOST_AUTO_TEST_CASE(other_issuers_and_full_requests_are_left_alone) {
   auto in = eth_inputs();
   auto other_issuer = synd_request(1, eth, 1, liqeth, 10);
   other_issuer.issuer = sysio::chain::name{"someone"};
   auto other_schema = synd_request(2, eth, 2, liqeth, 10);
   other_schema.schema = sysio::chain::name{"other"};
   auto full = synd_request(3, eth, 3, liqeth, 10);
   full.bonded = full.covered;
   in.requests = {other_issuer, other_schema, full};
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_CHECK(out.waiting.empty());
}

BOOST_AUTO_TEST_CASE(a_bonded_request_is_approved_once_its_window_passes) {
   auto in = eth_inputs();
   auto r = synd_request(1, eth, 1, liqeth, 10);
   r.state     = request_state::BONDED;
   r.bonded    = r.covered;
   r.bonded_at = in.now - r.window + fc::seconds(1);
   in.requests = {r};
   in.bonds    = {{1, {.request_id = 1, .amount = liqeth_units(10)}}};
   BOOST_CHECK(plan_actions(in).approves.empty());   // one second to go
   in.requests[0].bonded_at = in.now - r.window;
   BOOST_CHECK(plan_actions(in).approves == std::vector<uint64_t>{1});
   in.bonds.clear();   // not ours: someone else approves it
   BOOST_CHECK(plan_actions(in).approves.empty());
}

BOOST_AUTO_TEST_CASE(rulings_are_claimed_and_held_or_forfeited_bonds_reported) {
   auto in = eth_inputs();
   auto valid = synd_request(1, eth, 1, liqeth, 10);
   valid.state = request_state::VALID;
   auto held = synd_request(2, eth, 2, liqeth, 10);
   held.state = request_state::HELD;
   auto invalid = synd_request(3, eth, 3, liqeth, 90);
   invalid.state = request_state::INVALID;
   in.requests = {valid, held, invalid, synd_request(4, eth, 4, liqeth, 80)};
   in.bonds = {{1, {.request_id = 1, .amount = liqeth_units(10)}},
               {2, {.request_id = 2, .amount = liqeth_units(10)}},
               {3, {.request_id = 3, .amount = liqeth_units(90)}}};
   in.bonded = {1, 2, 3};
   outpost_emitted_all(in);
   const auto out = plan_actions(in);
   BOOST_CHECK(out.claims == std::vector<uint64_t>{1});   // never the forfeited one
   BOOST_CHECK(out.held == std::vector<uint64_t>{2});
   BOOST_CHECK(out.blocked.empty());
   BOOST_CHECK(out.forfeited == std::vector<uint64_t>{3});
   BOOST_CHECK(out.forfeit_claims == std::vector<uint64_t>{3});   // what it earned is settled with the housekeeping
   // The forfeited 90 no longer counts against the cap; the unclaimed valid and held 10s do: 20 + 80 fits in 100.
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(4u, out.accepts[0].request_id);
   BOOST_CHECK_EQUAL(liqeth_units(80), out.accepts[0].amount);
   BOOST_CHECK(out.waiting.empty());
}

BOOST_AUTO_TEST_CASE(a_request_challenged_before_anyone_bonded_it_is_reported_blocked) {
   auto in = eth_inputs();
   auto challenged = synd_request(1, eth, 1, liqeth, 10);
   challenged.state = request_state::HELD;
   auto other_issuer = synd_request(2, eth, 2, liqeth, 10);
   other_issuer.state  = request_state::HELD;
   other_issuer.issuer = sysio::chain::name{"someone"};
   in.requests = {challenged, other_issuer};
   const auto out = plan_actions(in);
   BOOST_CHECK(out.blocked == std::vector<uint64_t>{1});
   BOOST_CHECK(out.held.empty());
}

BOOST_AUTO_TEST_CASE(nothing_is_bonded_approved_or_claimed_while_the_cord_is_pulled) {
   auto in = eth_inputs();
   in.frozen = true;
   auto bonded = synd_request(1, eth, 1, liqeth, 10);
   bonded.state     = request_state::BONDED;
   bonded.bonded    = bonded.covered;
   bonded.bonded_at = in.now - bonded.window;
   auto approved = synd_request(2, eth, 2, liqeth, 10);
   approved.state = request_state::APPROVED;
   auto held = synd_request(3, eth, 3, liqeth, 10);
   held.state = request_state::HELD;
   auto invalid = synd_request(4, eth, 4, liqeth, 10);
   invalid.state = request_state::INVALID;
   in.requests = {bonded, approved, held, invalid, synd_request(5, eth, 5, liqeth, 10)};
   in.bonds    = {{1, {.request_id = 1, .amount = liqeth_units(10)}},
                  {2, {.request_id = 2, .amount = liqeth_units(10)}},
                  {3, {.request_id = 3, .amount = liqeth_units(10)}},
                  {4, {.request_id = 4, .amount = liqeth_units(10)}}};
   in.bonded = {1, 2, 3, 4};
   outpost_emitted_all(in);
   auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_CHECK(out.approves.empty());
   BOOST_CHECK(out.claims.empty());
   BOOST_CHECK(out.forfeit_claims.empty());
   BOOST_CHECK(out.waiting.empty());   // a frozen pass does not report waits
   // What needs people is still reported.
   BOOST_CHECK(out.held == std::vector<uint64_t>{3});
   BOOST_CHECK(out.forfeited == std::vector<uint64_t>{4});

   in.frozen = false;
   out = plan_actions(in);
   BOOST_CHECK(out.approves == std::vector<uint64_t>{1});
   BOOST_CHECK(out.claims == std::vector<uint64_t>{2});
   BOOST_CHECK(out.forfeit_claims == std::vector<uint64_t>{4});
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(5u, out.accepts[0].request_id);
}

BOOST_AUTO_TEST_CASE(a_halted_underwriter_bonds_nothing_but_settles_what_it_bonded) {
   auto in = eth_inputs();
   in.halted = true;
   auto bonded = synd_request(1, eth, 1, liqeth, 10);
   bonded.state     = request_state::BONDED;
   bonded.bonded    = bonded.covered;
   bonded.bonded_at = in.now - bonded.window;
   auto approved = synd_request(2, eth, 2, liqeth, 10);
   approved.state = request_state::APPROVED;
   in.requests = {bonded, approved, synd_request(3, eth, 3, liqeth, 10)};
   in.bonds    = {{1, {.request_id = 1, .amount = liqeth_units(10)}},
                  {2, {.request_id = 2, .amount = liqeth_units(10)}}};
   outpost_emitted_all(in);
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_CHECK(out.waiting.empty());
   BOOST_CHECK(out.approves == std::vector<uint64_t>{1});
   BOOST_CHECK(out.claims == std::vector<uint64_t>{2});
}

BOOST_AUTO_TEST_CASE(a_forfeit_is_any_bonded_envelope_request_ruled_invalid) {
   auto mine = synd_request(5, eth, 5, liqeth, 10);
   mine.state = request_state::INVALID;
   auto not_mine = synd_request(4, eth, 4, liqeth, 10);
   not_mine.state = request_state::INVALID;
   auto other_issuer = synd_request(2, eth, 2, liqeth, 10);
   other_issuer.state  = request_state::INVALID;
   other_issuer.issuer = sysio::chain::name{"someone"};
   auto valid = synd_request(1, eth, 1, liqeth, 10);
   valid.state = request_state::VALID;
   auto earlier = synd_request(0, eth, 0, liqeth, 10);
   earlier.state = request_state::INVALID;
   const auto forfeits = forfeited_requests({mine, not_mine, other_issuer, valid, earlier}, {0, 1, 2, 5});
   BOOST_CHECK((forfeits == std::vector<uint64_t>{0, 5}));   // in id order
}

BOOST_AUTO_TEST_CASE(a_forfeit_is_reported_after_anyone_claims_or_prunes_the_bond) {
   auto in = eth_inputs();
   auto claimed = synd_request(1, eth, 1, liqeth, 10);   // its yield claimed by anyone: the bond is paid
   claimed.state = request_state::INVALID;
   auto pruned = synd_request(2, eth, 2, liqeth, 10);    // then pruned: the bond row is gone
   pruned.state = request_state::INVALID;
   in.requests = {claimed, pruned};
   in.bonds    = {{1, {.request_id = 1, .amount = liqeth_units(10), .paid = true}}};
   in.bonded   = {1, 2};
   const auto out = plan_actions(in);
   BOOST_CHECK((out.forfeited == std::vector<uint64_t>{1, 2}));
   BOOST_CHECK(out.claims.empty());
   BOOST_CHECK(out.forfeit_claims.empty());   // nothing of ours is left to settle on either
}

BOOST_AUTO_TEST_CASE(what_a_forfeited_bond_earned_is_claimed_with_the_housekeeping) {
   auto in = eth_inputs();
   const auto invalid = [](uint64_t id) {
      auto r  = synd_request(id, eth, id, liqeth, 10);
      r.state = request_state::INVALID;
      return r;
   };
   in.requests = {invalid(1), invalid(2), invalid(3), invalid(4)};
   // Unclaimed, claimed with WIRE the pool could not cover, and settled; 4 is someone else's.
   in.bonds    = {{1, {.request_id = 1, .amount = liqeth_units(10)}},
                  {2, {.request_id = 2, .amount = liqeth_units(10), .paid = true, .owed_wire = wire_units(5)}},
                  {3, {.request_id = 3, .amount = liqeth_units(10), .paid = true}}};
   in.bonded   = {1, 2, 3};
   auto out = plan_actions(in);
   BOOST_CHECK((out.forfeit_claims == std::vector<uint64_t>{1, 2}));
   BOOST_CHECK(out.claims.empty());   // never claimed every pass: a bond that earned nothing has nothing to claim
   BOOST_CHECK((out.forfeited == std::vector<uint64_t>{1, 2, 3}));

   in.frozen = true;   // claims are refused while the cord is pulled
   out = plan_actions(in);
   BOOST_CHECK(out.forfeit_claims.empty());
}

BOOST_AUTO_TEST_CASE(owed_yield_is_claimed_after_the_principal) {
   auto in = eth_inputs();
   auto yield_owed = synd_request(1, eth, 1, liqeth, 10);
   yield_owed.state = request_state::APPROVED;
   auto unpaid = synd_request(2, eth, 2, liqeth, 10);
   unpaid.state = request_state::VALID;
   auto settled = synd_request(3, eth, 3, liqeth, 10);
   settled.state = request_state::APPROVED;
   auto forfeited = synd_request(4, eth, 4, liqeth, 10);
   forfeited.state = request_state::INVALID;
   in.requests = {yield_owed, unpaid, settled, forfeited};
   in.bonds    = {{1, {.request_id = 1, .amount = liqeth_units(10), .paid = true, .owed_wire = wire_units(1)}},
                  {2, {.request_id = 2, .amount = liqeth_units(10)}},
                  {3, {.request_id = 3, .amount = liqeth_units(10), .paid = true}},
                  {4, {.request_id = 4, .amount = liqeth_units(10), .paid = true, .owed_wire = wire_units(5)}}};
   auto out = plan_actions(in);
   BOOST_CHECK((out.claims == std::vector<uint64_t>{1, 2}));   // never the settled or the forfeited one
   BOOST_CHECK(out.forfeit_claims == std::vector<uint64_t>{4});

   in.frozen = true;
   out = plan_actions(in);
   BOOST_CHECK(out.claims.empty());
   BOOST_CHECK(out.forfeit_claims.empty());
}

BOOST_AUTO_TEST_CASE(only_approved_valid_and_invalid_are_terminal) {
   BOOST_CHECK(!is_terminal(request_state::OPEN));
   BOOST_CHECK(!is_terminal(request_state::BONDED));
   BOOST_CHECK(!is_terminal(request_state::HELD));
   BOOST_CHECK(is_terminal(request_state::APPROVED));
   BOOST_CHECK(is_terminal(request_state::VALID));
   BOOST_CHECK(is_terminal(request_state::INVALID));
}

BOOST_AUTO_TEST_SUITE_END()
