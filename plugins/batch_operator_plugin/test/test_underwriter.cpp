#include "../src/underwriter.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <cstring>
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

/// An OPEN `sysio.synd` envelope request.
request synd_request(uint64_t id, fc::slug_name chain_code, uint32_t epoch, fc::slug_name token, uint64_t covered) {
   return request{
      .id         = id,
      .issuer     = sysio::chain::name(synd::account),
      .schema     = sysio::chain::name(envelope_schema),
      .statement  = statement_bytes(chain_code, epoch, digest_of(std::to_string(id)), token),
      .token_code = token,
      .covered    = covered,
      .window_sec = 10800,
   };
}

/// Inputs where ETH is confirmed through epoch 10 and LIQETH has room for 100 with 100 held.
plan_inputs eth_inputs() {
   plan_inputs in;
   in.verified = {{eth, 10}};
   in.caps     = {{liqeth, 100}};
   in.balances = {{liqeth, 100}};
   in.now      = fc::time_point::now();
   return in;
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
      ("bonded_at", "2026-10-07T00:00:05.000");
   const auto r = decode_request(row);
   BOOST_REQUIRE(r.has_value());
   BOOST_CHECK_EQUAL(42u, r->id);
   BOOST_CHECK_EQUAL(std::string("sysio.synd"), r->issuer.to_string());
   BOOST_CHECK_EQUAL(std::string("oppenvelope"), r->schema.to_string());
   BOOST_CHECK(statement == r->statement);
   BOOST_CHECK_EQUAL(liqeth, r->token_code);
   BOOST_CHECK_EQUAL(300u, r->covered);
   BOOST_CHECK_EQUAL(100u, r->bonded);
   BOOST_CHECK_EQUAL(10800u, r->window_sec);
   BOOST_CHECK(request_state::BONDED == r->state);
   BOOST_CHECK(fc::time_point::from_iso_string("2026-10-07T00:00:05.000") == r->bonded_at);

   auto unknown_state = row;
   unknown_state.set("state", 9);   // a value the enum does not name renders as its integer
   BOOST_CHECK(!decode_request(unknown_state).has_value());
   auto missing = row;
   missing.erase("covered");
   BOOST_CHECK(!decode_request(missing).has_value());
}

BOOST_AUTO_TEST_CASE(decode_bond_keeps_only_the_underwriters_rows) {
   const auto row = mvo()("request_id", 5)("underwriter", "uw.acct")("amount", 70)
                       ("yield", mvo()("index_checkpoint", "0")("owed_wire", 0))("paid", false);
   const auto ours = decode_bond(row, underwriter_account);
   BOOST_REQUIRE(ours.has_value());
   BOOST_CHECK_EQUAL(5u, ours->request_id);
   BOOST_CHECK_EQUAL(70u, ours->amount);
   BOOST_CHECK(!ours->paid);
   BOOST_CHECK(!decode_bond(row, sysio::chain::name{"someone"}).has_value());
}

BOOST_AUTO_TEST_CASE(the_outpost_confirms_the_tip_by_emitting_the_same_bytes) {
   const depot_tip tip{.epoch_index = 9, .winning_checksum = digest_of("bytes9"), .envelope_digest = digest_of("d9")};
   BOOST_CHECK(tip_verdict::CONFIRMED == verify_tip(tip, {.epoch_index = 9, .bytes_sha256 = digest_of("bytes9")}));
   // Other bytes for the same epoch: the depot accepted something the outpost did not emit.
   BOOST_CHECK(tip_verdict::CONTRADICTED == verify_tip(tip, {.epoch_index = 9, .bytes_sha256 = digest_of("forged")}));
}

BOOST_AUTO_TEST_CASE(the_outpost_one_epoch_on_confirms_the_tip_through_its_previous_hash) {
   const depot_tip tip{.epoch_index = 9, .winning_checksum = digest_of("bytes9"), .envelope_digest = digest_of("d9")};
   const auto      d9 = digest_of("d9");
   outpost_envelope next{.epoch_index = 10, .bytes_sha256 = digest_of("bytes10")};
   next.previous_envelope_hash.assign(d9.data(), d9.data() + 32);
   BOOST_CHECK(tip_verdict::CONFIRMED == verify_tip(tip, next));

   const auto other = digest_of("other");
   next.previous_envelope_hash.assign(other.data(), other.data() + 32);
   BOOST_CHECK(tip_verdict::CONTRADICTED == verify_tip(tip, next));
   next.previous_envelope_hash.assign(d9.data(), d9.data() + 31);   // not a digest
   BOOST_CHECK(tip_verdict::CONTRADICTED == verify_tip(tip, next));
}

BOOST_AUTO_TEST_CASE(an_outpost_further_on_or_behind_says_nothing) {
   const depot_tip tip{.epoch_index = 9, .winning_checksum = digest_of("bytes9"), .envelope_digest = digest_of("d9")};
   const auto      d9 = digest_of("d9");
   outpost_envelope two_on{.epoch_index = 11, .bytes_sha256 = digest_of("bytes9")};
   two_on.previous_envelope_hash.assign(d9.data(), d9.data() + 32);
   BOOST_CHECK(tip_verdict::UNKNOWN == verify_tip(tip, two_on));
   BOOST_CHECK(tip_verdict::UNKNOWN == verify_tip(tip, {.epoch_index = 8, .bytes_sha256 = digest_of("bytes9")}));

   // The epoch after the last one a uint32 holds is never "one on".
   const depot_tip last{.epoch_index = UINT32_MAX, .winning_checksum = digest_of("bytesN"),
                        .envelope_digest = digest_of("dN")};
   BOOST_CHECK(tip_verdict::UNKNOWN == verify_tip(last, {.epoch_index = 0, .bytes_sha256 = digest_of("bytes0")}));
}

BOOST_AUTO_TEST_CASE(a_confirmed_request_is_bonded_for_its_remainder) {
   auto in = eth_inputs();
   auto r  = synd_request(1, eth, 10, liqeth, 60);
   r.bonded = 20;   // someone else bonded part of it
   in.requests = {r};
   const auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(1u, out.accepts[0].request_id);
   BOOST_CHECK_EQUAL(40u, out.accepts[0].amount);
   BOOST_CHECK(out.waiting.empty());
}

BOOST_AUTO_TEST_CASE(an_unconfirmed_request_waits) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 11, liqeth, 10),    // after the confirmed epoch
                  synd_request(2, sol, 1, liqeth, 10)};    // an outpost with nothing confirmed
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_REQUIRE_EQUAL(2u, out.waiting.size());
   BOOST_CHECK(wait_reason::UNVERIFIED == out.waiting[0].reason);
   BOOST_CHECK(wait_reason::UNVERIFIED == out.waiting[1].reason);
}

BOOST_AUTO_TEST_CASE(the_cap_counts_unsettled_bonds_and_this_pass_oldest_first) {
   auto in = eth_inputs();
   // 30 already bonded on an approved request not yet claimed counts; a claimed bond does not.
   auto approved = synd_request(1, eth, 1, liqeth, 30);
   approved.state = request_state::APPROVED;
   auto settled = synd_request(2, eth, 2, liqeth, 50);
   settled.state = request_state::APPROVED;
   in.bonds = {{1, {.request_id = 1, .amount = 30}}, {2, {.request_id = 2, .amount = 50, .paid = true}}};
   // Listed newest first: the plan still takes request 3 before request 4.
   in.requests = {approved, settled, synd_request(4, eth, 4, liqeth, 40), synd_request(3, eth, 3, liqeth, 60)};
   const auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(3u, out.accepts[0].request_id);   // 30 + 60 = 90 fits under 100
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK_EQUAL(4u, out.waiting[0].request_id);   // 90 + 40 does not
   BOOST_CHECK(wait_reason::OVER_CAP == out.waiting[0].reason);
   BOOST_CHECK(out.claims == std::vector<uint64_t>{1});
}

BOOST_AUTO_TEST_CASE(no_cap_or_too_little_balance_waits) {
   auto in = eth_inputs();
   in.requests = {synd_request(1, eth, 1, liqsol, 10)};
   in.verified = {{eth, 10}};
   auto out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
   BOOST_CHECK(wait_reason::NO_CAP == out.waiting[0].reason);

   in = eth_inputs();
   in.balances = {{liqeth, 50}};
   in.requests = {synd_request(1, eth, 1, liqeth, 40), synd_request(2, eth, 2, liqeth, 40)};
   out = plan_actions(in);
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_REQUIRE_EQUAL(1u, out.waiting.size());
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
   full.bonded = 10;
   in.requests = {other_issuer, other_schema, full};
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_CHECK(out.waiting.empty());
}

BOOST_AUTO_TEST_CASE(a_bonded_request_is_approved_once_its_window_passes) {
   auto in = eth_inputs();
   auto r = synd_request(1, eth, 1, liqeth, 10);
   r.state     = request_state::BONDED;
   r.bonded    = 10;
   r.bonded_at = in.now - fc::seconds(r.window_sec) + fc::seconds(1);
   in.requests = {r};
   in.bonds    = {{1, {.request_id = 1, .amount = 10}}};
   BOOST_CHECK(plan_actions(in).approves.empty());   // one second to go
   in.requests[0].bonded_at = in.now - fc::seconds(r.window_sec);
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
   in.bonds = {{1, {.request_id = 1, .amount = 10}},
               {2, {.request_id = 2, .amount = 10}},
               {3, {.request_id = 3, .amount = 90}}};
   const auto out = plan_actions(in);
   BOOST_CHECK(out.claims == std::vector<uint64_t>{1});   // never the forfeited one
   BOOST_CHECK(out.held == std::vector<uint64_t>{2});
   BOOST_CHECK(out.blocked.empty());
   BOOST_CHECK(out.forfeited == std::vector<uint64_t>{3});
   // The forfeited 90 no longer counts against the cap; the unclaimed valid and held 10s do: 20 + 80 fits in 100.
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(4u, out.accepts[0].request_id);
   BOOST_CHECK_EQUAL(80u, out.accepts[0].amount);
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
   bonded.bonded    = 10;
   bonded.bonded_at = in.now - fc::seconds(bonded.window_sec);
   auto approved = synd_request(2, eth, 2, liqeth, 10);
   approved.state = request_state::APPROVED;
   auto held = synd_request(3, eth, 3, liqeth, 10);
   held.state = request_state::HELD;
   auto invalid = synd_request(4, eth, 4, liqeth, 10);
   invalid.state = request_state::INVALID;
   in.requests = {bonded, approved, held, invalid, synd_request(5, eth, 5, liqeth, 10)};
   in.bonds    = {{1, {.request_id = 1, .amount = 10}},
                  {2, {.request_id = 2, .amount = 10}},
                  {3, {.request_id = 3, .amount = 10}},
                  {4, {.request_id = 4, .amount = 10}}};
   auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_CHECK(out.approves.empty());
   BOOST_CHECK(out.claims.empty());
   BOOST_CHECK(out.waiting.empty());   // a frozen pass does not report waits
   // What needs people is still reported.
   BOOST_CHECK(out.held == std::vector<uint64_t>{3});
   BOOST_CHECK(out.forfeited == std::vector<uint64_t>{4});

   in.frozen = false;
   out = plan_actions(in);
   BOOST_CHECK(out.approves == std::vector<uint64_t>{1});
   BOOST_CHECK(out.claims == std::vector<uint64_t>{2});
   BOOST_REQUIRE_EQUAL(1u, out.accepts.size());
   BOOST_CHECK_EQUAL(5u, out.accepts[0].request_id);
}

BOOST_AUTO_TEST_CASE(a_halted_underwriter_bonds_nothing_but_settles_what_it_bonded) {
   auto in = eth_inputs();
   in.halted = true;
   auto bonded = synd_request(1, eth, 1, liqeth, 10);
   bonded.state     = request_state::BONDED;
   bonded.bonded    = 10;
   bonded.bonded_at = in.now - fc::seconds(bonded.window_sec);
   auto approved = synd_request(2, eth, 2, liqeth, 10);
   approved.state = request_state::APPROVED;
   in.requests = {bonded, approved, synd_request(3, eth, 3, liqeth, 10)};
   in.bonds    = {{1, {.request_id = 1, .amount = 10}}, {2, {.request_id = 2, .amount = 10}}};
   const auto out = plan_actions(in);
   BOOST_CHECK(out.accepts.empty());
   BOOST_CHECK(out.waiting.empty());
   BOOST_CHECK(out.approves == std::vector<uint64_t>{1});
   BOOST_CHECK(out.claims == std::vector<uint64_t>{2});
}

BOOST_AUTO_TEST_CASE(forfeits_are_unpaid_bonds_on_invalid_envelope_requests) {
   auto mine = synd_request(5, eth, 5, liqeth, 10);
   mine.state = request_state::INVALID;
   auto paid = synd_request(3, eth, 3, liqeth, 10);
   paid.state = request_state::INVALID;
   auto not_mine = synd_request(4, eth, 4, liqeth, 10);
   not_mine.state = request_state::INVALID;
   auto other_issuer = synd_request(2, eth, 2, liqeth, 10);
   other_issuer.state  = request_state::INVALID;
   other_issuer.issuer = sysio::chain::name{"someone"};
   auto valid = synd_request(1, eth, 1, liqeth, 10);
   valid.state = request_state::VALID;
   auto earlier = synd_request(0, eth, 0, liqeth, 10);
   earlier.state = request_state::INVALID;
   const std::map<uint64_t, bond_position> bonds{{5, {.request_id = 5, .amount = 10}},
                                                 {3, {.request_id = 3, .amount = 10, .paid = true}},
                                                 {2, {.request_id = 2, .amount = 10}},
                                                 {1, {.request_id = 1, .amount = 10}},
                                                 {0, {.request_id = 0, .amount = 10}}};
   const auto forfeits = forfeited_requests({mine, paid, not_mine, other_issuer, valid, earlier}, bonds);
   BOOST_CHECK((forfeits == std::vector<uint64_t>{0, 5}));   // in id order
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
