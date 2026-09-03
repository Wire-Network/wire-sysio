#include <sysio.chalg/sysio.chalg.hpp>

#include <sysio.roa.hpp>                 // authoritative T1 electorate snapshot at open
#include <sysio.uwrit/sysio.uwrit.hpp>   // uwreq + lock reads for the underwriter challenge
#include <sysio.reserv/sysio.reserv.hpp> // reserve books that price the challenge bond
#include <sysio.opreg/sysio.opreg.hpp>   // operator status guard before slashing
#include <sysio.opp.common/amm_math.hpp> // token_to_wire — the bond's WIRE valuation
#include <sysio.opp.common/wire_asset.hpp>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <limits>

namespace sysio {

using opp::types::DisputeStatus;
using opp::types::NodeOwnerTier;
using opp::types::OperatorStatus;
using opp::types::UnderwriteRequestStatus;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

namespace {

/// Rejection text for a dispute without enough competing envelope versions to adjudicate.
constexpr const char* DISPUTE_REQUIRES_TWO_CANDIDATES =
   "a dispute requires at least two candidate envelope versions";

/// Floor valuation, in WIRE atomic units, for a NONZERO collateral bucket the depot cannot price —
/// its `(chain_code, token_code)` pair carries no ACTIVE reserve, or the pair's books floor the
/// conversion to zero (dust).
///
/// Pricing such a bucket at this floor rather than refusing the whole quote is deliberate, and the
/// direction matters. Collateral ingress (`opreg::depositinle`) accepts any positive amount for any
/// pair without requiring one that quotes, so refusing made unpriceability an ATTACKER-CONTROLLED
/// switch: an underwriter could seed a single dust or unreserved bucket and permanently immunize
/// every later CONFIRMED commitment against challenge. An understated stake weakens the deterrent
/// for one filing; an unchallengeable commitment defeats the mechanism outright and cannot be
/// recovered from. The floor is the strictly safer failure, and it is deterministic — the same
/// bucket always contributes the same amount, so `uwchalbond` and `openuwchal` still agree.
///
/// Shares the provisional-pricing caveat on `compute_uwchal_bond`: acceptable while collateral is
/// small, and a real valuation for unreserved pairs is part of that same pre-launch revisit.
constexpr uint64_t MIN_UWCHAL_BUCKET_WIRE = 1;

/// Wall-clock now in ms — the clock `sysio.uwrit`'s lock window runs on
/// (`lock_entry.expires_at_ms`), so challenge deadlines compare like-for-like.
uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

/// Snapshot the Tier-1 electorate: the Tier-1 rows of `sysio.roa::nodeowners` for `network_gen`,
/// walked via the `bytier` index (bounded by the Tier-1 registration cap). Shared by
/// `opendispute` and `openuwchal` so both case types freeze eligibility and quorum from the SAME
/// list at open — later registrations or a generation rotation can never change an in-flight
/// case's electorate.
std::vector<name> snapshot_t1_electorate(name roa_account, uint8_t network_gen) {
   roa::nodeowners_t nodeowners(roa_account, network_gen);
   auto by_tier = nodeowners.get_index<"bytier"_n>();
   const uint64_t t1_tier = magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1);
   std::vector<name> electorate;
   for (auto it = by_tier.lower_bound(t1_tier); it != by_tier.end() && it->by_tier() == t1_tier; ++it) {
      electorate.push_back(it->owner);
   }
   return electorate;
}

/// The bond quote for challenging one commitment — see `compute_uwchal_bond`.
struct uwchal_bond_quote {
   uint64_t bond        = 0; ///< Σ the underwriter's collateral quoted to WIRE; 0 = unquotable
   uint64_t deadline_ms = 0; ///< the locks' shared `expires_at_ms` — becomes the vote deadline
   uint32_t live_locks  = 0; ///< locks counted into the bond
};

/// THE bond formula — one function behind both the read-only quote (`uwchalbond`) and the charge
/// (`openuwchal`), so the two can never drift (the `split_wire_fee` principle).
///
/// The bond is the underwriter's ENTIRE collateral, quoted to WIRE (Jonathan, 2026-08-11:
/// *"sum their outpost deposits based on current quotes to convert TO WIRE … and use that amount
/// for now"*). That is what an upheld verdict actually costs them: `opreg::slash` drains every
/// bucket's slashable-now portion and `sweeplocks` deferred-slashes the locked remainder, so the
/// challenger stakes what the underwriter stands to lose.
///
/// Deposits are per-outpost and per-token — native and ERC-20/SPL alike — so each bucket is
/// quoted on its pair's live reserve books and the quotes summed. A WIRE-denominated bucket, if
/// one exists, contributes directly.
///
/// **REVISIT (Jonathan, 2026-08-11 — "make a note to revisit").** Two provisional choices here:
///   1. A (chain, token) pair may carry SEVERAL active reserves — PRIMARY plus private ones — at
///      different depths and therefore different prices. This takes the first ACTIVE row from the
///      `bychaintok` index, which is arbitrary when more than one qualifies.
///   2. The quote is a spot read of pool depth, so the bond a challenger is quoted can move
///      before they file. A configured WIRE figure would be stable; a curve quote is not.
/// Both are acceptable while collateral is small and pairs carry one public reserve; neither
/// survives a deep private-reserve market. Revisit before launch.
///
/// The lock walk that remains is the LIVENESS gate, not the amount: filing is time-boxed to the
/// live lock window, a commitment already under challenge is not re-challengeable, and the locks
/// carry the shared `expires_at_ms` that becomes the vote deadline.
///
/// Returns a zeroed quote (unchallengeable / unquotable) when: the winner has no locks for the
/// uwreq, any lock is already held by another challenge or already expired (filing is time-gated
/// to the live window), the operator row is gone, the operator holds no collateral at all, or the
/// sum exceeds `asset::max_amount`.
///
/// A bucket the depot cannot price does NOT void the quote — it contributes
/// `MIN_UWCHAL_BUCKET_WIRE`. Voiding made unpriceability an underwriter-controlled immunity
/// switch, since collateral ingress never required a quoteable pair; that constant carries the
/// full argument.
///
/// The bound is the TRANSFERABLE asset range (2^62-1), not `uint64_t`'s: `openuwchal` escrows the
/// quote as `asset(static_cast<int64_t>(quote.bond), opp::wire::asset_symbol)`, and `asset`'s range
/// check
/// rejects anything above `max_amount`. Bounding at uint64 instead let a two-leg quote land in
/// (`asset::max_amount`, `uint64_t::max`] — `uwchalbond` would advertise a nonzero bond that
/// `openuwchal` could never escrow, so filing reverted before any lock was held and the
/// commitment became unchallengeable. Each leg is independently valid there; the SUM is what
/// overflows (source and destination may price against the same imbalanced reserve). Quoting zero
/// keeps the advertised bond and what filing can actually escrow in agreement — the same range
/// discipline `credit_bond` applies on the payout side.
uwchal_bond_quote compute_uwchal_bond(name uwrit_account, name reserv_account, name opreg_account,
                                      uint64_t uwreq_id, name underwriter) {
   uwchal_bond_quote quote;
   const uint64_t now_ms = current_time_ms();

   uwrit::locks_t locks(uwrit_account);
   reserve::reserves_t reserves(reserv_account);

   // The lock walk is the LIVENESS gate, not the amount: filing is time-boxed to the live lock
   // window, and a commitment already under challenge is not re-challengeable. It also carries
   // the shared `expires_at_ms` that becomes the vote deadline.
   auto by_uwreq = locks.get_index<"byuwreq"_n>();
   for (auto it = by_uwreq.lower_bound(uwreq_id);
        it != by_uwreq.end() && it->uwreq_id == uwreq_id; ++it) {
      if (it->underwriter != underwriter) continue;
      if (it->challenge_id != 0 || now_ms >= it->expires_at_ms) return uwchal_bond_quote{};
      quote.deadline_ms = it->expires_at_ms;
      ++quote.live_locks;
   }
   if (quote.live_locks == 0) return uwchal_bond_quote{};

   // The bond is EVERYTHING the underwriter stands to lose: an upheld verdict drains every
   // collateral bucket (`opreg::slash` takes each bucket's slashable-now portion, and
   // `sweeplocks` deferred-slashes the locked remainder). Buckets are denominated per outpost
   // in that leg's own token — native and ERC-20/SPL alike — so each is quoted to WIRE on its
   // pair's live books and the quotes are summed.
   opreg::operators_t ops(opreg_account);
   auto op = ops.find(opreg::operator_key{underwriter.value});
   if (op == ops.end()) return uwchal_bond_quote{};

   auto by_chain_token = reserves.template get_index<"bychaintok"_n>();
   opp::amm::u128 total = 0;
   for (const auto& bal : op->balances) {
      if (bal.balance == 0) continue;
      if (bal.token_code == opp::wire::token_code) {
         total += bal.balance;   // already WIRE — no curve to ride
         continue;
      }
      // FIRST ACTIVE reserve for the pair. A pair may carry several (PRIMARY plus private
      // ones) at different depths, so this pricing is deliberately provisional — see the
      // revisit note on the declaration.
      const uint128_t ck = (static_cast<uint128_t>(bal.chain_code.value) << 64) | bal.token_code.value;
      uint64_t bucket_wire = 0;
      for (auto it = by_chain_token.lower_bound(ck);
           it != by_chain_token.end() && it->by_chain_token() == ck; ++it) {
         if (it->status != opp::types::RESERVE_STATUS_ACTIVE) continue;
         bucket_wire = opp::amm::token_to_wire(it->reserve_chain_amount, it->reserve_wire_amount,
                                               it->connector_weight_bps, bal.balance);
         break;
      }
      // A bucket the depot cannot price (no ACTIVE reserve for the pair, or books that floor the
      // conversion to zero) contributes its floor instead of voiding the whole quote. Refusing
      // here understated nothing but handed the underwriter a switch: seed one dust or unreserved
      // bucket at ingress and every later commitment becomes unchallengeable. See
      // `MIN_UWCHAL_BUCKET_WIRE` for why the understatement is the safer of the two.
      total += (bucket_wire > 0 ? bucket_wire : MIN_UWCHAL_BUCKET_WIRE);
   }
   if (total == 0) return uwchal_bond_quote{};
   if (total > static_cast<opp::amm::u128>(asset::max_amount)) return uwchal_bond_quote{};
   quote.bond = static_cast<uint64_t>(total);
   return quote;
}

/// Credit `amount` WIRE to `account`'s claimable-bond balance, accumulating onto an existing row.
///
/// This is how a resolved challenge's escrow leaves `chkuwchal` — deliberately NOT a transfer.
/// `chkuwchal` can run inline under `sysio.epoch::advance`, and `sysio.token::transfer` runs the
/// recipient's code through `require_recipient(to)`; an asserting recipient there would abort the
/// whole advance and, with the challenge rolled back to OPEN and its locks still held, stall epoch
/// advancement chain-wide. Crediting touches only this contract's own table. See `chalg::claimbond`.
///
/// Saturates at `asset::max_amount` (the `sysio.dclaim::add_wire_capped` discipline): the credit
/// is paid out as an `asset`, whose amount is a signed 62-bit quantity, so an unclamped sum could
/// build a row that `claimbond` can never construct a payout for.
void credit_bond(name self, name account, uint64_t amount) {
   chalg::bondcredits_t credits(self);
   const auto pk = chalg::bond_credit_key{account.value};
   auto it = credits.find(pk);
   if (it == credits.end()) {
      const uint64_t seed = std::min<uint64_t>(amount, static_cast<uint64_t>(asset::max_amount));
      credits.emplace(ram_payer, pk, chalg::bond_credit{ .account = account, .amount = seed });
      return;
   }
   credits.modify(same_payer, pk, [&](auto& r) {
      const uint64_t room = static_cast<uint64_t>(asset::max_amount) - r.amount;
      r.amount += (amount <= room ? amount : room);
   });
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  slashop — execute a slash on an operator via sysio.opreg
// ---------------------------------------------------------------------------
void chalg::slashop(name operator_acct, std::string reason) {
   // Authorised callers: sysio.chalg itself (dispute resolution) and sysio.epoch (the single-path
   // slash of non-canonical OPP envelope deliverers at epoch close, per the dispute-vote design).
   // Both are trusted system contracts; chalg is the single slashing chokepoint that holds
   // opreg::slash authority.
   check(has_auth(get_self()) || has_auth(EPOCH_ACCOUNT),
         "slashop requires sysio.chalg or sysio.epoch authority");

   // Slash via sysio.opreg — the canonical bond ledger. opreg routes the slashable portion
   // (`balance - sum(active locks)`) to the matching LP on each (chain, token_kind) the operator has
   // bond on, marks the operator SLASHED, and lets sysio.uwrit::release deferred-slash the locked
   // portion as each underwriter lock resolves.
   action(
      permission_level{get_self(), "active"_n},
      OPREG_ACCOUNT,
      "slash"_n,
      std::make_tuple(operator_acct, reason)
   ).send();
}

// ---------------------------------------------------------------------------
//  opendispute — open an OPP envelope dispute (called inline by sysio.msgch)
// ---------------------------------------------------------------------------
void chalg::opendispute(uint64_t chain_code,
                        uint32_t epoch_index,
                        std::vector<dispute_candidate> candidates) {
   require_auth(MSGCH_ACCOUNT);
   check(candidates.size() >= chalg_limits::minimum_dispute_candidate_versions,
         DISPUTE_REQUIRES_TWO_CANDIDATES);

   disputes_t disputes(get_self());

   // At most one dispute per (outpost, epoch). evalcons gates on this too, but enforce it here so
   // the inline call is idempotent under re-fired deliveries.
   auto oe_idx = disputes.get_index<"byoutepoch"_n>();
   uint128_t composite = opp::outpost_epoch_key(chain_code, epoch_index);
   check(oe_idx.find(composite) == oe_idx.end(),
         "a dispute already exists for this outpost+epoch");

   // Snapshot the electorate: the Tier-1 rows of sysio.roa::nodeowners for the active network
   // generation, walked via the `bytier` index (bounded by the Tier-1 registration cap). Voter
   // eligibility (votedispute) and the tally quorum (chkdispute) are both served from this
   // snapshot for the dispute's whole life, so they cannot diverge from each other, and later
   // registrations or a generation rotation cannot change an in-flight dispute's electorate or
   // quorum.
   const uint8_t network_gen = roa::current_network_gen(ROA_ACCOUNT);
   auto electorate = snapshot_t1_electorate(ROA_ACCOUNT, network_gen);

   // Defense in depth for direct calls: msgch preflights this invariant and soft-returns so a
   // terminal delivery remains retryable, while this assertion keeps every other caller from
   // opening an unresolvable, permanently-pausing dispute.
   check(!electorate.empty(), "cannot open a dispute with no registered tier-1 node owners");
   const uint32_t quorum = static_cast<uint32_t>(electorate.size()) / 2 + 1;

   auto     now     = current_time_point();
   uint64_t next_id = std::max<uint64_t>(1, disputes.available_primary_key());

   disputes.emplace(ram_payer, dispute_key{next_id}, dispute_entry{
      .id               = next_id,
      .chain_code       = chain_code,
      .epoch_index      = epoch_index,
      .status           = DisputeStatus::DISPUTE_STATUS_OPEN,
      .winning_checksum = checksum256{},
      .opened_at        = now,
      .deadline         = now + sysio::seconds(dispute_deadline_sec),
      .candidates       = std::move(candidates),
      .network_gen      = network_gen,
      .electorate       = std::move(electorate),
      .quorum           = quorum,
   });

   chalgstate_t chalgstate(get_self());
   auto st = chalgstate.get_or_default(chalg_state{});
   ++st.open_disputes;
   chalgstate.set(st, ram_payer);

   // Pause epoch advancement until every open dispute resolves. chkdispute releases the pause when
   // the last open dispute goes RESOLVED; re-sending pause for a second concurrent dispute is an
   // idempotent flag set.
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "pause"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  votedispute — Tier-1 node owner votes for the canonical envelope checksum
// ---------------------------------------------------------------------------
void chalg::votedispute(name owner, uint64_t dispute_id, checksum256 chosen_checksum) {
   require_auth(owner);

   disputes_t disputes(get_self());
   auto d = disputes.get(dispute_key{dispute_id}, "dispute not found");
   check(d.status == DisputeStatus::DISPUTE_STATUS_OPEN, "dispute is not open");

   // `chosen_checksum` must be one of the dispute's candidate versions.
   bool is_candidate = false;
   for (const auto& c : d.candidates) {
      if (c.checksum == chosen_checksum) { is_candidate = true; break; }
   }
   check(is_candidate, "chosen checksum is not a candidate in this dispute");

   // Voter eligibility: membership in the dispute's snapshotted Tier-1 electorate. The snapshot is
   // the same list chkdispute's quorum is measured against, so a voter the tally would not count
   // can never cast a vote, and an owner registered after the dispute opened cannot join it.
   check(std::find(d.electorate.begin(), d.electorate.end(), owner) != d.electorate.end(),
         "voter is not in the dispute's tier-1 electorate");

   // One vote per owner (the vote table is scoped by dispute_id).
   disputevotes_t votes(get_self(), dispute_id);
   auto v_pk = dispute_vote_key{owner.value};
   check(!votes.contains(v_pk), "owner has already voted in this dispute");

   votes.emplace(ram_payer, v_pk, dispute_vote{
      .owner           = owner,
      .chosen_checksum = chosen_checksum,
      .voted_at        = current_time_point(),
   });
}

// ---------------------------------------------------------------------------
//  chkdispute — tally votes; on resolution dispatch the winner and unpause
// ---------------------------------------------------------------------------
void chalg::chkdispute(uint64_t dispute_id) {
   // Permissionless crank, driven by `batch_operator_plugin`'s epoch tick
   // (`--batch-epoch-poll-ms`, 15s default) from every ACTIVE batch operator.
   // That cadence is this action's ONLY driver: unlike `chkuwchal`, which
   // `sysio.uwrit::chklocks` pokes from every `sysio.epoch::advance`, a dispute
   // pauses `advance` itself, so no inline poke can reach here.
   disputes_t disputes(get_self());
   auto d_pk = dispute_key{dispute_id};
   auto d = disputes.get(d_pk, "dispute not found");
   check(d.status == DisputeStatus::DISPUTE_STATUS_OPEN, "dispute is not open");

   // N = the dispute's snapshotted Tier-1 electorate size; Q = its quorum, both fixed when the
   // dispute opened, so registry changes while it is open can never raise, lower, or zero the
   // threshold. opendispute guarantees a non-empty snapshot; the invariant check guards rows
   // written before electorate snapshots existed from silently resolving with Q == 0.
   const uint32_t N = static_cast<uint32_t>(d.electorate.size());
   const uint32_t Q = d.quorum;
   check(Q > 0 && Q <= N, "dispute has no electorate snapshot");

   // Tally the cast votes by chosen checksum (parallel vectors; the set is <= 21).
   disputevotes_t votes(get_self(), dispute_id);
   std::vector<checksum256> seen;
   std::vector<uint32_t>    counts;
   uint32_t cast = 0;
   for (auto it = votes.begin(); it != votes.end(); ++it) {
      bool found = false;
      for (size_t i = 0; i < seen.size(); ++i) {
         if (seen[i] == it->chosen_checksum) { counts[i]++; found = true; break; }
      }
      if (!found) { seen.push_back(it->chosen_checksum); counts.push_back(1); }
      ++cast;
   }

   // Resolve per the tally rule. Fast path (any time): a checksum reaches a majority of the
   // snapshotted electorate. After the deadline: a quorum of cast votes AND a strict majority of
   // cast votes. No plurality / tie-break — an unresolved tally just keeps waiting for more votes.
   const bool  past_deadline = current_time_point() >= d.deadline;
   bool        resolved      = false;
   checksum256 winner{};
   for (size_t i = 0; i < seen.size(); ++i) {
      if (counts[i] >= Q) { winner = seen[i]; resolved = true; break; }
      if (past_deadline && cast >= Q && 2 * counts[i] > cast) {
         winner = seen[i]; resolved = true; break;
      }
   }
   if (!resolved) return;

   disputes.modify(same_payer, d_pk, [&](auto& r) {
      r.status           = DisputeStatus::DISPUTE_STATUS_RESOLVED;
      r.winning_checksum = winner;
   });

   // This dispute leaves OPEN: drop the open-dispute count. The decrement is guarded so a row
   // written before the counter existed cannot underflow it.
   chalgstate_t chalgstate(get_self());
   auto st = chalgstate.get_or_default(chalg_state{});
   if (st.open_disputes > 0) --st.open_disputes;
   chalgstate.set(st, ram_payer);

   // Dispatch the winning envelope via sysio.msgch (it still holds the raw bytes). The next
   // chkcons advances the epoch, where the single-path slash of every non-canonical deliverer
   // runs in sysio.epoch::advance.
   action(
      permission_level{get_self(), "active"_n},
      MSGCH_ACCOUNT,
      "resolvedisp"_n,
      std::make_tuple(d.chain_code, d.epoch_index, winner)
   ).send();

   // Release the epoch pause only when no other dispute remains open -- sysio.epoch::is_paused is
   // a single flag shared by every dispute, so an earlier resolution must not resume advancement
   // while another (outpost, epoch) dispute is still voting.
   if (st.open_disputes == 0) {
      action(
         permission_level{get_self(), "active"_n},
         EPOCH_ACCOUNT,
         "unpause"_n,
         std::make_tuple()
      ).send();
   }
}

// ---------------------------------------------------------------------------
//  openuwchal — file an underwriter-fault challenge (WIRE-297)
// ---------------------------------------------------------------------------
void chalg::openuwchal(name challenger, uint64_t uwreq_id, name underwriter,
                       uint8_t reason, std::string detail) {
   require_auth(challenger);

   // Trust boundary: the ABI carries the numeric value; the checked cast is the validation
   // (never static_cast — out-of-range would be UB and hide bad input).
   const auto fault = magic_enum::enum_cast<underwrite_fault_reason>(reason);
   check(fault.has_value(), "openuwchal: unknown fault reason");
   check(detail.size() <= max_uwchal_detail_bytes,
         "openuwchal: detail exceeds max_uwchal_detail_bytes");

   // The challenged commitment: a CONFIRMED uwreq whose recorded winner is `underwriter`.
   uwrit::uwreqs_t reqs(UWRIT_ACCOUNT);
   auto rq = reqs.find(uwrit::id_key{uwreq_id});
   check(rq != reqs.end(), "openuwchal: underwrite request not found");
   check(rq->status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED,
         "openuwchal: underwrite request is not CONFIRMED");
   check(rq->winner == underwriter, "openuwchal: underwriter is not this request's winner");

   // A verdict is final per commitment: at most one challenge EVER per (uwreq, underwriter).
   // Other commitments — other swaps, any epoch — are independently challengeable.
   uwchals_t chals(get_self());
   auto uq_idx = chals.get_index<"byuwrequw"_n>();
   const uint128_t composite = (static_cast<uint128_t>(uwreq_id) << 64) | underwriter.value;
   check(uq_idx.find(composite) == uq_idx.end(),
         "openuwchal: this commitment has already been challenged");

   // Price the bond off the SAME formula `uwchalbond` quotes. Zero live locks means the window
   // has closed (or never opened); an unpriceable bond refuses the filing rather than guessing.
   const auto quote = compute_uwchal_bond(UWRIT_ACCOUNT, RESERV_ACCOUNT, OPREG_ACCOUNT, uwreq_id, underwriter);
   check(quote.live_locks > 0,
         "openuwchal: no live collateral locks to challenge (the lock window has closed)");
   check(quote.bond > 0, "openuwchal: challenge bond cannot be priced");

   const uint8_t network_gen = roa::current_network_gen(ROA_ACCOUNT);
   auto electorate = snapshot_t1_electorate(ROA_ACCOUNT, network_gen);
   // Same refusal as opendispute: an electorate that cannot vote could never resolve the
   // challenge — it would only lapse at the window's end, having held the bond for nothing.
   check(!electorate.empty(), "cannot open a challenge with no registered tier-1 node owners");
   const uint32_t quorum = static_cast<uint32_t>(electorate.size()) / 2 + 1;

   // Escrow the bond under the challenger's OWN authority (the swapfromwire escrow pattern) —
   // this contract holds it until resolution routes it per the verdict.
   action(
      permission_level{challenger, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(challenger, get_self(),
         asset(static_cast<int64_t>(quote.bond), opp::wire::asset_symbol),
         std::string("sysio.chalg::openuwchal challenge bond"))
   ).send();

   const auto now     = current_time_point();
   uint64_t   next_id = std::max<uint64_t>(1, chals.available_primary_key());

   chals.emplace(ram_payer, uwchal_key{next_id}, uwchal_entry{
      .id          = next_id,
      .uwreq_id    = uwreq_id,
      .underwriter = underwriter,
      .challenger  = challenger,
      .reason      = *fault,
      .detail      = std::move(detail),
      .status      = DisputeStatus::DISPUTE_STATUS_OPEN,
      .verdict     = uwchal_verdict::NONE,
      .bond_amount = quote.bond,
      .deadline_ms = quote.deadline_ms,
      .opened_at   = now,
      .network_gen = network_gen,
      .electorate  = std::move(electorate),
      .quorum      = quorum,
   });

   // Mark the locks held. uwrit owns the lock data and re-validates liveness authoritatively —
   // a stale filing aborts the WHOLE open, bond escrow included. A held lock is skipped by
   // chklocks until the challenge resolves or lapses. NOTE: deliberately NO epoch pause — an
   // unresolved challenge is survivable, so the chain keeps advancing.
   action(
      permission_level{get_self(), "active"_n},
      UWRIT_ACCOUNT, "holdlocks"_n,
      std::make_tuple(uwreq_id, underwriter, next_id)
   ).send();
}

// ---------------------------------------------------------------------------
//  voteuwchal — Tier-1 ballot in an open challenge (record-only, like votedispute)
// ---------------------------------------------------------------------------
void chalg::voteuwchal(name owner, uint64_t chal_id, uint8_t ballot) {
   require_auth(owner);

   const auto cast_ballot = magic_enum::enum_cast<uwchal_ballot>(ballot);
   check(cast_ballot.has_value(), "voteuwchal: unknown ballot value");

   uwchals_t chals(get_self());
   auto c = chals.get(uwchal_key{chal_id}, "challenge not found");
   check(c.status == DisputeStatus::DISPUTE_STATUS_OPEN, "challenge is not open");

   // Ballots are valid only inside the challenge window. Past deadline_ms the sole lawful
   // outcome is LAPSED (there is deliberately no after-deadline relaxed tally), so a late
   // quorum must not be assemblable for a manual chkuwchal crank in the gap between expiry
   // and the next epoch-tick poke.
   check(current_time_ms() < c.deadline_ms, "voteuwchal: the challenge window has expired");

   // Voter eligibility: membership in the challenge's snapshotted Tier-1 electorate — the same
   // list chkuwchal's quorum is measured against, so a voter the tally would not count can never
   // cast a ballot.
   check(std::find(c.electorate.begin(), c.electorate.end(), owner) != c.electorate.end(),
         "voter is not in the challenge's tier-1 electorate");

   // One ballot per owner (the vote table is scoped by chal_id).
   uwchalvotes_t votes(get_self(), chal_id);
   auto v_pk = uwchal_vote_key{owner.value};
   check(!votes.contains(v_pk), "owner has already voted in this challenge");

   votes.emplace(ram_payer, v_pk, uwchal_vote{
      .owner    = owner,
      .ballot   = *cast_ballot,
      .voted_at = current_time_point(),
   });
}

// ---------------------------------------------------------------------------
//  chkuwchal — tally an open challenge; resolve or lapse (chkdispute's sibling)
// ---------------------------------------------------------------------------
void chalg::chkuwchal(uint64_t chal_id) {
   // Permissionless crank — poked inline by `sysio.uwrit::chklocks` for any expired-but-
   // challenged lock (the epoch tick IS the cadence; possible because the chain is not paused),
   // or called manually right after the deciding vote for a faster resolution.
   uwchals_t chals(get_self());
   auto c_pk = uwchal_key{chal_id};
   auto c = chals.get(c_pk, "challenge not found");
   check(c.status == DisputeStatus::DISPUTE_STATUS_OPEN, "challenge is not open");

   const uint32_t N = static_cast<uint32_t>(c.electorate.size());
   const uint32_t Q = c.quorum;
   check(Q > 0 && Q <= N, "challenge has no electorate snapshot");

   // Tally the cast ballots.
   uint32_t uphold = 0, reject_refund = 0, reject_forfeit = 0;
   uwchalvotes_t votes(get_self(), chal_id);
   for (auto it = votes.begin(); it != votes.end(); ++it) {
      switch (it->ballot) {
         case uwchal_ballot::UPHOLD:         ++uphold;         break;
         case uwchal_ballot::REJECT_REFUND:  ++reject_refund;  break;
         case uwchal_ballot::REJECT_FORFEIT: ++reject_forfeit; break;
      }
   }

   // Resolution rules — quorum thresholds any time; the deadline only LAPSES (there is
   // deliberately no after-deadline relaxed tally: a challenge must be voted before the lock
   // window expires; envelope disputes relax only because a paused chain MUST resolve).
   uwchal_verdict verdict = uwchal_verdict::NONE;
   if (uphold >= Q) {
      verdict = uwchal_verdict::UPHELD;
   } else if (reject_refund + reject_forfeit >= Q) {
      // Disposition by majority among the rejectors; a tie favours the refund — forfeiture
      // only ever happens by explicit council judgment.
      verdict = (reject_forfeit > reject_refund) ? uwchal_verdict::REJECTED_FORFEIT
                                                 : uwchal_verdict::REJECTED_REFUND;
   } else if (current_time_ms() >= c.deadline_ms) {
      verdict = uwchal_verdict::LAPSED;
   }
   if (verdict == uwchal_verdict::NONE) return; // keep waiting for ballots

   chals.modify(same_payer, c_pk, [&](auto& r) {
      r.status  = DisputeStatus::DISPUTE_STATUS_RESOLVED;
      r.verdict = verdict;
      // Compact to a fixed-width tombstone. Past resolution the row has exactly two on-chain
      // jobs — the `byuwrequw` uniqueness gate (a verdict is final per commitment) and the
      // verdict record — and neither needs the variable-length fields. `detail` was
      // caller-controlled evidence FOR the council; `electorate` was the ballot roll. Both stay
      // permanently readable in the filing's action trace, and dropping them here is what bounds
      // this contract's retained variable-length state by the number of CONCURRENTLY OPEN
      // challenges (each backed by a live lock set and an escrowed bond) rather than by every
      // challenge ever filed — the per-note cap alone cannot do that, because filing is
      // permissionless and the bond comes back on every non-forfeit outcome.
      r.detail.clear();
      r.electorate.clear();
   });

   // The ballots go with it: they were the tally input above and the one-vote-per-owner gate,
   // neither of which applies once the row leaves OPEN. Collect first, erase second (an erase
   // invalidates the iterator) — bounded by the Tier-1 electorate this challenge snapshotted, the
   // same bound the tally walk above already runs under.
   std::vector<uint64_t> ballot_owners;
   ballot_owners.reserve(uphold + reject_refund + reject_forfeit);
   for (auto it = votes.begin(); it != votes.end(); ++it) {
      ballot_owners.push_back(it->owner.value);
   }
   for (uint64_t owner : ballot_owners) {
      votes.erase(uwchal_vote_key{owner});
   }

   if (verdict == uwchal_verdict::UPHELD) {
      // Slash FIRST, then sweep: inline actions run depth-first in send order, so the SLASHED
      // status flip lands before sweeplocks' releaselock calls — which then take the
      // deferred-slash branch, debiting the locked collateral and emitting the outbound SLASH
      // attestations. Guard on live status: this can run inline from the epoch tick, where an
      // aborted slash (already SLASHED/TERMINATED out-of-band) must not stall advancement.
      opreg::operators_t ops(OPREG_ACCOUNT);
      auto op = ops.find(opreg::operator_key{c.underwriter.value});
      const bool slashable = op != ops.end() &&
                             op->status != OperatorStatus::OPERATOR_STATUS_SLASHED &&
                             op->status != OperatorStatus::OPERATOR_STATUS_TERMINATED;
      if (slashable) {
         action(
            permission_level{get_self(), "active"_n},
            get_self(), "slashop"_n,
            std::make_tuple(c.underwriter,
               std::string("underwriter fault upheld (")
                  + std::string(magic_enum::enum_name(c.reason))
                  + "): challenge " + std::to_string(chal_id))
         ).send();
      }
      action(
         permission_level{get_self(), "active"_n},
         UWRIT_ACCOUNT, "sweeplocks"_n,
         std::make_tuple(c.uwreq_id, c.underwriter)
      ).send();
   } else {
      // REJECTED or LAPSED: clear the hold; the locks live out their natural window and release
      // on the next normal sweep (a healthy release — no collateral moves).
      action(
         permission_level{get_self(), "active"_n},
         UWRIT_ACCOUNT, "freelocks"_n,
         std::make_tuple(c.uwreq_id, c.underwriter)
      ).send();
   }

   // Route the bond per the verdict: forfeiture to the wrongly-challenged underwriter ONLY on an
   // explicit REJECT_FORFEIT majority; every other outcome returns it to the challenger.
   //
   // CREDITED, never transferred. This whole function can run inline under
   // `sysio.epoch::advance -> sysio.uwrit::chklocks`, where `sysio.token::transfer`'s
   // `require_recipient(to)` would run the recipient's own code and let it abort epoch
   // advancement — see `chalg::claimbond` for the full argument. The escrow stays in this
   // contract's custody until the recipient pulls it.
   if (c.bond_amount > 0) {
      const bool forfeited = (verdict == uwchal_verdict::REJECTED_FORFEIT);
      credit_bond(get_self(), forfeited ? c.underwriter : c.challenger, c.bond_amount);
   }
}

// ---------------------------------------------------------------------------
//  claimbond — pull a resolved challenge's bond out of this contract's custody
// ---------------------------------------------------------------------------
void chalg::claimbond(name account) {
   // The recipient's own authority, as `sysio.dclaim::claim` requires it: the transfer below
   // notifies `account`, so only `account` can trigger code execution on its own behalf here.
   require_auth(account);

   bondcredits_t credits(get_self());
   const auto pk = bond_credit_key{account.value};
   auto it = credits.find(pk);
   check(it != credits.end(), "claimbond: no claimable bond");
   const uint64_t amount = it->amount;
   check(amount > 0, "claimbond: zero claimable balance");

   // Erase BEFORE sending: the inline transfer runs after this action returns and notifies
   // `account`, which may re-enter `claimbond`. The row must already be gone when it does.
   credits.erase(pk);

   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), account,
         asset(static_cast<int64_t>(amount), opp::wire::asset_symbol),
         std::string("sysio.chalg challenge bond payout"))
   ).send();
}

// ---------------------------------------------------------------------------
//  uwchalbond — read-only quote of the bond openuwchal would charge right now
// ---------------------------------------------------------------------------
uint64_t chalg::uwchalbond(uint64_t uwreq_id, name underwriter) {
   // Soft gating — a quote, not a filing: every not-currently-challengeable state answers 0
   // (callers treat 0 as "no quote"), mirroring `sysio.reserv::swapquote`'s contract.
   uwrit::uwreqs_t reqs(UWRIT_ACCOUNT);
   auto rq = reqs.find(uwrit::id_key{uwreq_id});
   if (rq == reqs.end()) return 0;
   if (rq->status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED) return 0;
   if (rq->winner != underwriter) return 0;

   uwchals_t chals(get_self());
   auto uq_idx = chals.get_index<"byuwrequw"_n>();
   const uint128_t composite = (static_cast<uint128_t>(uwreq_id) << 64) | underwriter.value;
   if (uq_idx.find(composite) != uq_idx.end()) return 0; // already challenged — a verdict is final

   return compute_uwchal_bond(UWRIT_ACCOUNT, RESERV_ACCOUNT, OPREG_ACCOUNT, uwreq_id, underwriter).bond;
}

} // namespace sysio
