#pragma once
/**
 * @file underwriter.hpp
 * @brief The decisions behind the underwriter: which `sysio.bond` requests issued by `sysio.synd` to bond, approve
 *        and claim, given what each outpost recorded of the envelopes it emitted. Pure functions over decoded rows, so
 *        the plugin's tests drive them without a chain.
 */

#include "exposure_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/endian/conversion.hpp>
#include <magic_enum/magic_enum.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/slug_name.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/symbol.hpp>
#include <sysio.bond/bond_increment.hpp>

namespace sysio::batch_operator_detail::underwriter {

/// `sysio.bond` identifiers the underwriter touches.
namespace bond {
   constexpr auto account        = "sysio.bond";
   constexpr auto table_requests = "requests";
   constexpr auto table_bonds    = "bonds";
   constexpr auto action_accept  = "accept";
   constexpr auto action_approve = "approve";
   constexpr auto action_claim   = "claim";
   constexpr auto action_prune   = "prune";
   namespace field {
      constexpr auto id          = "id";
      constexpr auto issuer      = "issuer";
      constexpr auto schema      = "schema";
      constexpr auto statement   = "statement";
      constexpr auto token_code  = "token_code";
      constexpr auto covered     = "covered";
      constexpr auto bonded      = "bonded";
      constexpr auto window_sec  = "window_sec";
      constexpr auto state       = "state";
      constexpr auto bonded_at   = "bonded_at";
      constexpr auto outcome_acknowledged = "outcome_acknowledged";
      constexpr auto request_id  = "request_id";
      constexpr auto underwriter = "underwriter";
      constexpr auto amount      = "amount";
      constexpr auto paid        = "paid";
      constexpr auto yield       = "yield";       ///< a `bonds` row's yield position
      constexpr auto owed_wire   = "owed_wire";   ///< WIRE of it still owed
      constexpr auto account     = "account";
      constexpr auto from_id     = "from_id";
      constexpr auto limit       = "limit";
   }
}

/// `sysio.synd` identifiers the underwriter touches.
namespace synd {
   constexpr auto account         = "sysio.synd";
   constexpr auto action_crank    = "crank";
   constexpr auto action_pruneenv = "pruneenv";
   namespace field {
      constexpr auto limit      = "limit";
      constexpr auto chain_code = "chain_code";
      constexpr auto token_code = "token_code";
   }
}

/// `sysio.andon` identifiers the underwriter reads: while the cord is pulled it bonds, approves and claims nothing.
namespace andon {
   constexpr auto account    = "sysio.andon";
   constexpr auto table_cord = "cord";
   namespace field {
      constexpr auto pulled = "pulled";
   }
}

/// The schema `sysio.synd` registers every envelope statement under (`sysio.opp.common`'s
/// `envelope_statement::schema`).
constexpr auto envelope_schema = "oppenvelope";

/// WIRE as `sysio.bond` handles it (`sysio.opp.common`'s `opp::wire`): the registry code of a request bonded in WIRE,
/// and the token a bond's yield is paid in.
namespace wire {
   inline const fc::slug_name token_code{"WIRE"};
   inline const chain::symbol asset_symbol{9, "WIRE"};
}

/// The symbol of each token a request can be bonded in, by registry code: every `sysio.liq` shadow, and WIRE.
using token_symbols = std::map<fc::slug_name, chain::symbol>;

/// `units` base units of `sym`, as `sysio.bond` stores an amount. Throws when no asset can carry them.
inline chain::asset asset_of(uint64_t units, chain::symbol sym) {
   FC_ASSERT(units <= static_cast<uint64_t>(chain::asset::max_amount), "{} base units exceed the asset range", units);
   return chain::asset(static_cast<int64_t>(units), sym);
}

/// `sysio.bond`'s request lifecycle, member for member: the chain renders the enum by these names.
enum class request_state : uint8_t { OPEN, BONDED, APPROVED, HELD, VALID, INVALID };

/// A `sysio.bond::requests` row id. `sysio.bond` assigns them in increasing order and never reuses one.
using request_id_t = uint64_t;

/// One `sysio.bond::requests` row.
struct request {
   request_id_t      id         = 0;
   chain::name       issuer{};
   chain::name       schema{};
   std::vector<char> statement;
   fc::slug_name     token_code{};     ///< registry code of the bond token
   chain::asset      covered{};        ///< what the underwriters must bond in total, in the bond token
   chain::asset      bonded{};         ///< what they have bonded so far
   fc::microseconds  window{};         ///< challenge window after full bonding
   request_state     state      = request_state::OPEN;
   fc::time_point    bonded_at{};      ///< when bonded reached covered
   bool              outcome_acknowledged = false;   ///< the issuer has processed the request's terminal state
};

/// Whether `r` is an envelope request: issued by `sysio.synd` under the envelope schema. No other request is
/// underwritten.
inline bool is_envelope_request(const request& r) {
   return r.issuer == chain::name(synd::account) && r.schema == chain::name(envelope_schema);
}

/// Whether no action moves a request out of `state`.
inline bool is_terminal(request_state state) {
   return state == request_state::APPROVED || state == request_state::VALID || state == request_state::INVALID;
}

/// Decode a `requests` row (`values_only`), its amounts in the symbol `symbols` names for its token, or nullopt when
/// it does not decode or its token has no symbol there.
inline std::optional<request> decode_request(const fc::variant_object& row, const token_symbols& symbols) {
   try {
      const std::optional<request_state> state =
         magic_enum::enum_cast<request_state>(row[bond::field::state].as_string());
      if (!state) return std::nullopt;
      const fc::slug_name token_code = row[bond::field::token_code].as<fc::slug_name>();
      const auto          symbol     = symbols.find(token_code);
      if (symbol == symbols.end()) return std::nullopt;
      return request{
         .id         = row[bond::field::id].as_uint64(),
         .issuer     = chain::name(row[bond::field::issuer].as_string()),
         .schema     = chain::name(row[bond::field::schema].as_string()),
         .statement  = row[bond::field::statement].as<std::vector<char>>(),
         .token_code = token_code,
         .covered    = asset_of(row[bond::field::covered].as_uint64(), symbol->second),
         .bonded     = asset_of(row[bond::field::bonded].as_uint64(), symbol->second),
         .window     = fc::seconds(row[bond::field::window_sec].as_int64()),
         .state      = *state,
         .bonded_at  = row[bond::field::bonded_at].as<fc::time_point>(),
         .outcome_acknowledged = row[bond::field::outcome_acknowledged].as_bool(),
      };
   } catch (const fc::exception&) {
      return std::nullopt;
   }
}

/// One underwriter's bond on one request (`sysio.bond::bonds`).
struct bond_position {
   request_id_t request_id = 0;
   chain::asset amount{};                                       ///< bonded, in the request's token
   bool         paid      = false;                              ///< `claim` returned the principal
   /// WIRE yield still owed: a claim the yield pool could not cover in full leaves some.
   chain::asset owed_wire = chain::asset(0, wire::asset_symbol);
};

/// One submitted accept, retained even if its callback times out or reports a refusal.
struct pending_accept {
   fc::slug_name token_code{};       ///< registry code of the token reserved by this submission
   chain::asset amount{};           ///< submitted contribution, reserved against exposure and wallet funds
   chain::asset bonded_before{};    ///< our bond row's amount before submission, used to recognize inclusion
   fc::time_point_sec expires_at{};   ///< the exact signed transaction expiration, judged by LIB time
};

/// At most one unobserved accept per request for this underwriter.
using pending_accept_map = std::map<request_id_t, pending_accept>;

/// Wait past old submissions even on failover to a node with an older LIB; only LIB may clear the deadline.
inline fc::time_point_sec startup_accept_deadline(fc::time_point lib_time, fc::time_point wall_time,
                                                  fc::microseconds transaction_lifetime) {
   return fc::time_point_sec(std::max(lib_time, wall_time) + transaction_lifetime);
}

/// Decode a `bonds` row (`values_only`) when it is `underwriter`'s, else nullopt; its amount is in the token of its
/// request among `requests`. Throws when the row is the underwriter's but does not decode, names no underwriter, or
/// names a request missing from `requests`: a bond it cannot read or attribute to a token must stop the pass rather
/// than drop out of its exposure. `sysio.bond` keeps a request until every bond on it is paid, so only a request row
/// that did not decode leaves one missing.
inline std::optional<bond_position> decode_bond(const fc::variant_object& row, chain::name underwriter,
                                                const std::vector<request>& requests) {
   const auto owner = row.find(bond::field::underwriter);
   FC_ASSERT(owner != row.end(), "a sysio.bond::bonds row names no underwriter");
   if (chain::name(owner->value().as_string()) != underwriter) return std::nullopt;
   const request_id_t request_id = row[bond::field::request_id].as_uint64();
   const auto         r          = std::ranges::find(requests, request_id, &request::id);
   FC_ASSERT(r != requests.end(), "request {} holds a bond of ours, but its sysio.bond::requests row did not decode",
             request_id);
   const uint64_t owed_wire = row[bond::field::yield].get_object()[bond::field::owed_wire].as_uint64();
   return bond_position{
      .request_id = request_id,
      .amount     = asset_of(row[bond::field::amount].as_uint64(), r->covered.get_symbol()),
      .paid       = row[bond::field::paid].as_bool(),
      .owed_wire  = asset_of(owed_wire, wire::asset_symbol),
   };
}

/// Whether `sysio.synd` has queue work that only a crank moves: an envelope request still in play (OPEN, BONDED or
/// HELD), or one ruled whose outcome it has not acknowledged yet.
inline bool synd_has_work(const std::vector<request>& requests) {
   return std::ranges::any_of(requests, [](const request& r) {
      return is_envelope_request(r) && (!is_terminal(r.state) || !r.outcome_acknowledged);
   });
}

/// The lowest request id a later pass still needs to read: every request in play, ruled but not acknowledged, or
/// bonded by the underwriter or awaiting submission finality. Request ids only grow and a settled request never comes
/// back into play, so earlier rows can be skipped. One past the newest read when nothing needs a later look;
/// `from` when nothing was read.
inline request_id_t next_scan_start(request_id_t from, const std::vector<request>& requests,
                                    const std::map<request_id_t, bond_position>& bonds,
                                    const std::set<request_id_t>&                bonded,
                                    const pending_accept_map& pending = {}) {
   std::optional<request_id_t> lowest;
   const auto                  keep = [&](request_id_t id) { lowest = std::min(lowest.value_or(id), id); };
   request_id_t                next = from;
   for (const request& r : requests) {
      next = std::max(next, r.id + 1);
      if (is_envelope_request(r) && (!is_terminal(r.state) || !r.outcome_acknowledged)) keep(r.id);
   }
   for (const auto& [id, bond] : bonds) keep(id);
   for (const request_id_t id : bonded) keep(id);
   for (const auto& [id, accept] : pending) keep(id);
   return lowest.value_or(next);
}

/// Request ids read in id order from some id on, and whether the read stopped before the end of the table.
struct request_id_page {
   std::vector<request_id_t> ids;
   bool                      more = false;   ///< rows past the last id were left unread
};

/// Successive `sysio.bond::prune` calls over the requests issued before a sweep began. Requests issued during a sweep
/// wait for the next one, so steady intake cannot hold the cursor among new requests and away from old ones.
struct prune_sweep {
   request_id_t                from = 0;   ///< where the next prune starts
   std::optional<request_id_t> last;       ///< newest request id when this sweep began (0: none); unset between sweeps

   /// Move past a prune of `limit` from `from`, given `page`, the ids from `from` read before the push: to the id after
   /// the first `limit`, or to a new sweep once that passes `last` or the end of the table. A read cut short before
   /// that id stays put. No request is skipped: erased rows only make a prune examine further.
   void advance(const request_id_page& page, uint32_t limit) {
      if (page.ids.size() <= limit && page.more) return;
      if (page.ids.size() > limit && last && page.ids[limit] <= *last) {
         from = page.ids[limit];
         return;
      }
      from = 0;
      last.reset();
   }
};

/// The envelope requests among `bonded` ruled INVALID, in id order: the underwriter's bond on each is forfeited.
/// Whether the bond row is paid, or even still there, does not matter: anyone may `claim` a forfeited bond's yield,
/// which marks it paid, and `prune` then erases the row.
inline std::vector<request_id_t> forfeited_requests(const std::vector<request>&    requests,
                                                    const std::set<request_id_t>& bonded) {
   std::vector<request_id_t> out;
   for (const request& r : requests) {
      if (is_envelope_request(r) && r.state == request_state::INVALID && bonded.contains(r.id)) out.push_back(r.id);
   }
   std::ranges::sort(out);
   return out;
}

/// Add the request behind each of the underwriter's bond rows to `bonded`, and drop the requests no longer among
/// `requests`. A bond row can go before its request (a paid one is pruned), and a forfeit on it must still be seen.
inline void remember_bonded(std::set<request_id_t>& bonded, const std::map<request_id_t, bond_position>& bonds,
                            const std::vector<request>& requests) {
   for (const auto& [id, bond] : bonds) bonded.insert(id);
   std::erase_if(bonded,
                 [&](request_id_t id) { return std::ranges::find(requests, id, &request::id) == requests.end(); });
}

/// Which forfeit stops bonding. The forfeits on chain at the first pass do not: the (re)start that began this run is
/// what resumes bonding after one. Any later forfeit halts bonding until the next restart.
struct forfeit_watch {
   std::optional<std::set<request_id_t>> known;       ///< the forfeits seen at the first pass
   std::optional<request_id_t>           halted_by;   ///< the request whose forfeit halted bonding

   /// Note this pass's forfeits. True when one of them halts bonding now.
   bool note(const std::vector<request_id_t>& forfeited) {
      if (!known) {
         known.emplace(forfeited.begin(), forfeited.end());
         return false;
      }
      if (halted_by) return false;
      for (const request_id_t id : forfeited) {
         if (known->contains(id)) continue;
         halted_by = id;
         return true;
      }
      return false;
   }
};

/// An envelope statement, as `sysio.synd` packs it for `sysio.bond` (`sysio.opp.common`'s envelope_statement.hpp).
struct envelope_statement {
   fc::slug_name chain_code{};       ///< the outpost that sent the envelope
   uint32_t      epoch_index = 0;    ///< the depot epoch the envelope was accepted for
   fc::sha256    digest{};           ///< the envelope's epoch digest, as `sysio.msgch` accepted it
   fc::slug_name token_code{};       ///< the liq token the statement covers
};

/// Bytes of one encoded statement: the chain code, the epoch, the digest and the token code, little-endian and
/// without a size prefix.
inline constexpr size_t STATEMENT_BYTES = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(fc::sha256) + sizeof(uint64_t);

/// Decode statement bytes, or nullopt when they are not exactly one encoded statement.
inline std::optional<envelope_statement> decode_statement(const std::vector<char>& bytes) {
   if (bytes.size() != STATEMENT_BYTES) return std::nullopt;
   const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
   envelope_statement   s;
   s.chain_code  = fc::slug_name{boost::endian::load_little_u64(p)};
   s.epoch_index = boost::endian::load_little_u32(p + sizeof(uint64_t));
   std::memcpy(s.digest.data(), p + sizeof(uint64_t) + sizeof(uint32_t), sizeof(fc::sha256));
   s.token_code  = fc::slug_name{
      boost::endian::load_little_u64(p + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(fc::sha256))};
   return s;
}

/// The (outpost chain, depot epoch) an envelope statement names.
using envelope_key = std::pair<fc::slug_name, uint32_t>;

/// The (chain, epoch) of every OPEN envelope request with a remainder to bond: the outpost records a pass reads.
inline std::set<envelope_key> wanted_envelopes(const std::vector<request>& requests) {
   std::set<envelope_key> wanted;
   for (const request& r : requests) {
      if (r.state != request_state::OPEN || !is_envelope_request(r) || r.bonded >= r.covered) continue;
      if (const std::optional<envelope_statement> s = decode_statement(r.statement)) {
         wanted.emplace(s->chain_code, s->epoch_index);
      }
   }
   return wanted;
}

/// The digest each outpost recorded for the `wanted` epochs, read with `read(chain, epoch)`. A chain whose read
/// throws is reported through `on_failure(chain, why)` once and not read again, so it holds back only its own
/// requests.
template <typename Read, typename OnFailure>
std::map<envelope_key, fc::sha256> collect_emitted(const std::set<envelope_key>& wanted, Read&& read,
                                                   OnFailure&& on_failure) {
   std::map<envelope_key, fc::sha256> emitted;
   std::set<fc::slug_name>            failed;
   for (const envelope_key& key : wanted) {
      const auto& [chain_code, epoch] = key;
      if (failed.contains(chain_code)) continue;
      try {
         if (const std::optional<fc::sha256> digest = read(chain_code, epoch)) emitted.emplace(key, *digest);
      } catch (const fc::exception& e) {
         on_failure(chain_code, e.top_message());
         failed.insert(chain_code);
      } catch (const std::exception& e) {
         on_failure(chain_code, std::string(e.what()));
         failed.insert(chain_code);
      }
   }
   return emitted;
}

/// Why an OPEN request is not bonded this pass.
enum class wait_reason : uint8_t {
   BAD_STATEMENT,    ///< the statement does not decode
   TOKEN_MISMATCH,   ///< the statement's token is not the request's bond token
   UNVERIFIED,       ///< the outpost holds no final record of an envelope for that epoch
   CONTRADICTED,     ///< the outpost recorded another digest for that epoch: the statement is false
   NO_CAP,           ///< neither capped nor unlimited exposure is configured for the token
   INVALID_PRECISION, ///< the token cannot represent the contract's bond increment
   PENDING_ACCEPT,   ///< a submitted contribution is not yet visible in the irreversible view
   STARTUP_FINALITY, ///< pre-restart transactions may still be live beyond the irreversible view
   OVER_CAP,         ///< the token's remaining exposure allowance cannot fund one bond increment
   LOW_BALANCE,      ///< the underwriter cannot fund one bond increment from its token balance
   BELOW_MINIMUM     ///< a first contribution would fall below the request's minimum first bond
};

/// Bond `amount` on request `request_id`.
struct accept_action {
   request_id_t request_id = 0;
   chain::asset amount{};
   fc::slug_name token_code{};   ///< registry code needed to reserve the submitted contribution
   chain::asset bonded_before{}; ///< our row amount in the view used to plan this contribution
};

/// Reserve every transaction that reached submission, including one whose callback did not confirm success.
inline void remember_submitted_accept(pending_accept_map& pending, const accept_action& accept,
                                      std::optional<fc::time_point_sec> expiration) {
   if (expiration) {
      pending.insert_or_assign(accept.request_id, pending_accept{accept.token_code, accept.amount,
                                                               accept.bonded_before, *expiration});
   }
}

/// An OPEN request left for a later pass.
struct waiting_request {
   request_id_t request_id = 0;
   wait_reason  reason     = wait_reason::UNVERIFIED;
};

/// Everything one pass decides.
struct plan {
   pending_accept_map           pending_accepts; ///< submitted accepts still unaccounted for by the view
   std::vector<accept_action>   accepts;
   std::vector<request_id_t>    approves;       ///< BONDED by the underwriter with the window passed
   std::vector<request_id_t>    claims;         ///< APPROVED or VALID with principal or WIRE yield still owed to it
   std::vector<request_id_t>    held;           ///< bonded by the underwriter and challenged: `sysio` must rule
   std::vector<request_id_t>    blocked;        ///< challenged before anyone bonded: stalls its pair until ruled
   std::vector<request_id_t>    forfeited;      ///< bonded by the underwriter and ruled INVALID
   /// Ruled INVALID with what the underwriter's bond earned still unsettled: claimed with the housekeeping. The claim
   /// returns no principal; it pays the earnings into `sysio.liq`, which lets `sysio.bond` prune the request.
   std::vector<request_id_t>    forfeit_claims;
   std::vector<waiting_request> waiting;
};

/// What one pass decides from.
struct plan_inputs {
   std::vector<request>                  requests;   ///< `requests` rows, any issuer
   std::map<request_id_t, bond_position> bonds;      ///< the underwriter's bonds, by request id
   /// Every request the underwriter bonded, including those whose bond row is gone.
   std::set<request_id_t>                bonded;
   /// The digest each outpost recorded for the envelope it emitted, for the (chain, epoch) pairs read this pass.
   std::map<envelope_key, fc::sha256> emitted;
   /// Token code -> exposure policy. Absent and default-created policies permit no new underwriting.
   std::map<fc::slug_name, exposure_policy> exposure_policies;
   std::map<fc::slug_name, chain::asset> balances;   ///< token code -> the underwriter's balance
   pending_accept_map                 pending_accepts;
   /// On startup, wait until LIB passes one transaction lifetime after the later of startup LIB and wall time.
   std::optional<fc::time_point_sec>   accepts_after;
   fc::time_point                    now{};
   /// The `sysio.andon` cord is pulled: nothing is bonded, approved or claimed until it clears.
   bool                              frozen = false;
   /// The underwriter stopped bonding after a forfeit: it still approves and claims what it bonded.
   bool                              halted = false;
};

/// Decide one pass. Only envelope requests are underwritten, and only when the outpost's own record of the envelope
/// it emitted for the statement's epoch carries the statement's digest. OPEN requests are taken oldest first, so a
/// pair's envelopes are bonded in the order `sysio.synd` releases them. Each contribution is limited by the uncovered
/// remainder, available exposure and wallet balance, rounded down to a 0.01-token increment so other underwriters can
/// complete the coverage. A bond counts against its token's cap until `claim` returns it, unless it was forfeited.
inline plan plan_actions(const plan_inputs& in) {
   std::map<request_id_t, const request*> by_id;
   for (const request& r : in.requests) by_id.emplace(r.id, &r);

   std::map<fc::slug_name, chain::asset> outstanding;   // token code -> unsettled bonds
   for (const auto& [id, b] : in.bonds) {
      const auto r = by_id.find(id);
      if (b.paid || r == by_id.end() || r->second->state == request_state::INVALID) continue;
      chain::asset& sum = outstanding.try_emplace(r->second->token_code, 0, b.amount.get_symbol()).first->second;
      sum = sum + b.amount;
   }
   std::map<fc::slug_name, chain::asset> balances = in.balances;
   plan out;
   for (const auto& [id, pending] : in.pending_accepts) {
      const auto r = by_id.find(id);
      if (r != by_id.end() && r->second->state != request_state::OPEN) continue;
      const auto own = in.bonds.find(id);
      if (own != in.bonds.end() && own->second.amount >= pending.bonded_before + pending.amount) continue;
      // Equality is not expired: a transaction can still execute at its expiration timestamp.
      if (in.now > pending.expires_at.to_time_point()) continue;
      out.pending_accepts.emplace(id, pending);
      chain::asset& used = outstanding.try_emplace(pending.token_code, 0, pending.amount.get_symbol()).first->second;
      used = used + pending.amount;
      const auto balance = balances.find(pending.token_code);
      if (balance != balances.end()) {
         const int64_t left = std::max(int64_t{0}, balance->second.get_amount() - pending.amount.get_amount());
         balance->second = chain::asset(left, balance->second.get_symbol());
      }
   }

   std::vector<const request*> ordered;
   for (const request& r : in.requests) {
      if (is_envelope_request(r)) ordered.push_back(&r);
   }
   std::ranges::sort(ordered, {}, &request::id);

   out.forfeited = forfeited_requests(in.requests, in.bonded);
   for (const request* r : ordered) {
      const auto bond = in.bonds.find(r->id);
      const bool ours = bond != in.bonds.end();
      // Our bond row is unpaid, or WIRE the pool could not yet cover is still owed on it: a claim has work to do.
      const bool unsettled = ours && (!bond->second.paid || bond->second.owed_wire.get_amount() > 0);
      switch (r->state) {
      case request_state::OPEN: {
         if (in.frozen || in.halted || r->bonded >= r->covered) break;
         const chain::asset remainder = r->covered - r->bonded;
         auto wait = [&](wait_reason why) { out.waiting.push_back({r->id, why}); };
         if (out.pending_accepts.contains(r->id)) { wait(wait_reason::PENDING_ACCEPT); break; }
         if (in.accepts_after && in.now <= in.accepts_after->to_time_point()) {
            wait(wait_reason::STARTUP_FINALITY); break;
         }
         const std::optional<envelope_statement> s = decode_statement(r->statement);
         if (!s) { wait(wait_reason::BAD_STATEMENT); break; }
         if (s->token_code != r->token_code) { wait(wait_reason::TOKEN_MISMATCH); break; }
         const auto emitted = in.emitted.find({s->chain_code, s->epoch_index});
         if (emitted == in.emitted.end()) { wait(wait_reason::UNVERIFIED); break; }
         if (emitted->second != s->digest) { wait(wait_reason::CONTRADICTED); break; }
         const auto policy = in.exposure_policies.find(r->token_code);
         if (policy == in.exposure_policies.end() || std::holds_alternative<disabled_exposure>(policy->second)) {
            wait(wait_reason::NO_CAP); break;
         }
         chain::asset& used = outstanding.try_emplace(r->token_code, 0, remainder.get_symbol()).first->second;
         const auto step = bond_increment::for_decimals(remainder.get_symbol().decimals());
         if (!step) { wait(wait_reason::INVALID_PRECISION); break; }
         const int64_t increment = static_cast<int64_t>(*step);
         chain::asset available = remainder;
         if (const auto* cap = std::get_if<chain::asset>(&policy->second)) {
            if (used >= *cap) { wait(wait_reason::OVER_CAP); break; }
            available = *cap - used;
            if (available.get_amount() < increment) { wait(wait_reason::OVER_CAP); break; }
         }
         const auto balance = balances.find(r->token_code);
         if (balance == balances.end() || balance->second.get_amount() < increment) {
            wait(wait_reason::LOW_BALANCE); break;
         }
         const int64_t affordable = std::min({remainder.get_amount(), available.get_amount(),
                                              balance->second.get_amount()});
         const chain::asset contribution(affordable - affordable % increment, remainder.get_symbol());
         // sysio.bond refuses a first bond below the request's minimum; a submitted accept reserves funds either way.
         if (!ours && !bond_increment::meets_first_bond_minimum(contribution.get_amount(), r->covered.get_amount(),
                                                                remainder.get_amount())) {
            wait(wait_reason::BELOW_MINIMUM); break;
         }
         out.accepts.push_back({r->id, contribution, r->token_code,
                               ours ? bond->second.amount : chain::asset(0, remainder.get_symbol())});
         used            = used + contribution;
         balance->second = balance->second - contribution;
         break;
      }
      case request_state::BONDED:
         if (!in.frozen && ours && in.now >= r->bonded_at + r->window) out.approves.push_back(r->id);
         break;
      case request_state::APPROVED:
      case request_state::VALID:
         if (!in.frozen && unsettled) out.claims.push_back(r->id);
         break;
      case request_state::INVALID:   // reported through `forfeited`
         if (!in.frozen && unsettled) out.forfeit_claims.push_back(r->id);
         break;
      case request_state::HELD:
         if (ours) out.held.push_back(r->id);
         else out.blocked.push_back(r->id);
         break;
      }
   }
   return out;
}

} // namespace sysio::batch_operator_detail::underwriter
