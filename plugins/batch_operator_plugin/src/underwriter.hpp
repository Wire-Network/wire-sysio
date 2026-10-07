#pragma once
/**
 * @file underwriter.hpp
 * @brief The decisions behind the underwriter: which `sysio.bond` requests issued by `sysio.synd` to bond,
 *        approve and claim, and what an outpost says about the depot's accepted envelope chain. Pure functions over
 *        decoded rows, so the plugin's tests drive them without a chain.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include <boost/endian/conversion.hpp>
#include <magic_enum/magic_enum.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/slug_name.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <sysio/chain/name.hpp>

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
      constexpr auto request_id  = "request_id";
      constexpr auto underwriter = "underwriter";
      constexpr auto amount      = "amount";
      constexpr auto paid        = "paid";
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

/// `sysio.bond`'s request lifecycle, member for member: the chain renders the enum by these names.
enum class request_state : uint8_t { OPEN, BONDED, APPROVED, HELD, VALID, INVALID };

/// One `sysio.bond::requests` row.
struct request {
   uint64_t          id         = 0;
   chain::name       issuer{};
   chain::name       schema{};
   std::vector<char> statement;
   fc::slug_name     token_code{};     ///< registry code of the bond token
   uint64_t          covered    = 0;   ///< base units the underwriters must bond in total
   uint64_t          bonded     = 0;   ///< base units bonded so far
   uint32_t          window_sec = 0;   ///< challenge window after full bonding
   request_state     state      = request_state::OPEN;
   fc::time_point    bonded_at{};      ///< when bonded reached covered
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

/// Decode a `requests` row (`values_only`), or nullopt when it does not decode.
inline std::optional<request> decode_request(const fc::variant_object& row) {
   try {
      const auto state = magic_enum::enum_cast<request_state>(row[bond::field::state].as_string());
      if (!state) return std::nullopt;
      return request{
         .id         = row[bond::field::id].as_uint64(),
         .issuer     = chain::name(row[bond::field::issuer].as_string()),
         .schema     = chain::name(row[bond::field::schema].as_string()),
         .statement  = row[bond::field::statement].as<std::vector<char>>(),
         .token_code = row[bond::field::token_code].as<fc::slug_name>(),
         .covered    = row[bond::field::covered].as_uint64(),
         .bonded     = row[bond::field::bonded].as_uint64(),
         .window_sec = static_cast<uint32_t>(row[bond::field::window_sec].as_uint64()),
         .state      = *state,
         .bonded_at  = row[bond::field::bonded_at].as<fc::time_point>(),
      };
   } catch (const fc::exception&) {
      return std::nullopt;
   }
}

/// One underwriter's bond on one request (`sysio.bond::bonds`).
struct bond_position {
   uint64_t request_id = 0;
   uint64_t amount     = 0;
   bool     paid       = false;   ///< `claim` has settled it
};

/// Decode a `bonds` row (`values_only`) when it is `underwriter`'s, else nullopt.
inline std::optional<bond_position> decode_bond(const fc::variant_object& row, chain::name underwriter) {
   try {
      if (chain::name(row[bond::field::underwriter].as_string()) != underwriter) return std::nullopt;
      return bond_position{
         .request_id = row[bond::field::request_id].as_uint64(),
         .amount     = row[bond::field::amount].as_uint64(),
         .paid       = row[bond::field::paid].as_bool(),
      };
   } catch (const fc::exception&) {
      return std::nullopt;
   }
}

/// The envelope requests ruled INVALID that hold an unpaid bond of the underwriter's, in id order. Such a bond is
/// forfeited and never claimed: it pays the underwriter nothing, and `claim` refuses one that earned no yield.
inline std::vector<uint64_t> forfeited_requests(const std::vector<request>& requests,
                                                const std::map<uint64_t, bond_position>& bonds) {
   std::vector<uint64_t> out;
   for (const auto& r : requests) {
      if (!is_envelope_request(r) || r.state != request_state::INVALID) continue;
      const auto bond = bonds.find(r.id);
      if (bond != bonds.end() && !bond->second.paid) out.push_back(r.id);
   }
   std::ranges::sort(out);
   return out;
}

/// An envelope statement, as `sysio.synd` packs it for `sysio.bond` (`sysio.opp.common`'s envelope_statement.hpp).
struct envelope_statement {
   fc::slug_name chain_code{};       ///< the outpost that sent the envelope
   uint32_t      epoch_index = 0;    ///< the depot epoch the envelope was accepted for
   fc::sha256    digest{};           ///< the envelope's canonical epoch digest, as `sysio.msgch` accepted it
   fc::slug_name token_code{};       ///< the liq token the statement covers
};

/// Bytes of one encoded statement: the chain code, the epoch, the digest and the token code, little-endian and
/// without a size prefix.
inline constexpr size_t STATEMENT_BYTES = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(fc::sha256) + sizeof(uint64_t);

/// Decode statement bytes, or nullopt when they are not exactly one encoded statement.
inline std::optional<envelope_statement> decode_statement(const std::vector<char>& bytes) {
   if (bytes.size() != STATEMENT_BYTES) return std::nullopt;
   const auto*        p = reinterpret_cast<const unsigned char*>(bytes.data());
   envelope_statement s;
   s.chain_code  = fc::slug_name{boost::endian::load_little_u64(p)};
   s.epoch_index = boost::endian::load_little_u32(p + sizeof(uint64_t));
   std::memcpy(s.digest.data(), p + sizeof(uint64_t) + sizeof(uint32_t), sizeof(fc::sha256));
   s.token_code  = fc::slug_name{
      boost::endian::load_little_u64(p + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(fc::sha256))};
   return s;
}

/// The depot's latest accepted envelope from one outpost (`sysio.msgch::outpcons`, written only on acceptance).
struct depot_tip {
   uint32_t   epoch_index = 0;
   fc::sha256 winning_checksum{}; ///< sha256 of the accepted envelope's delivered bytes
   fc::sha256 envelope_digest{};  ///< its canonical epoch digest
};

/// The outpost's own latest emitted envelope, read at finality.
struct outpost_envelope {
   uint32_t          epoch_index = 0;
   fc::sha256        bytes_sha256{};            ///< sha256 of the bytes as read: the bytes a relay delivers
   std::vector<char> previous_envelope_hash;    ///< the digest of the envelope before it, as the outpost computed it
};

/// What the outpost says about the depot's latest accepted envelope.
enum class tip_verdict : uint8_t {
   CONFIRMED,      ///< the outpost emitted the same bytes, or chained its next envelope to the accepted digest
   CONTRADICTED,   ///< the outpost emitted other bytes for that epoch, or chained its next envelope elsewhere
   UNKNOWN         ///< the outpost's latest envelope is for neither the tip's epoch nor the one after it
};

/// Compare the depot's tip with the outpost's own latest envelope. `sysio.msgch` accepts an envelope only when its
/// `previous_envelope_hash` continues the accepted chain, so a confirmed tip confirms every envelope accepted
/// before it, and a contradicted one means the depot accepted bytes the outpost never emitted.
inline tip_verdict verify_tip(const depot_tip& tip, const outpost_envelope& latest) {
   if (latest.epoch_index == tip.epoch_index) {
      return latest.bytes_sha256 == tip.winning_checksum ? tip_verdict::CONFIRMED : tip_verdict::CONTRADICTED;
   }
   if (static_cast<uint64_t>(latest.epoch_index) == static_cast<uint64_t>(tip.epoch_index) + 1) {
      const bool chained = latest.previous_envelope_hash.size() == sizeof(fc::sha256) &&
         fc::sha256(latest.previous_envelope_hash.data(), latest.previous_envelope_hash.size()) == tip.envelope_digest;
      return chained ? tip_verdict::CONFIRMED : tip_verdict::CONTRADICTED;
   }
   return tip_verdict::UNKNOWN;
}

/// Why an OPEN request is not bonded this pass.
enum class wait_reason : uint8_t {
   BAD_STATEMENT,    ///< the statement does not decode
   TOKEN_MISMATCH,   ///< the statement's token is not the request's bond token
   UNVERIFIED,       ///< the outpost has not confirmed the envelope yet
   NO_CAP,           ///< no exposure cap is configured for the token
   OVER_CAP,         ///< bonding it would exceed the token's exposure cap
   LOW_BALANCE       ///< the underwriter does not hold enough of the token
};

/// Bond `amount` on request `request_id`.
struct accept_action {
   uint64_t request_id = 0;
   uint64_t amount     = 0;
};

/// An OPEN request left for a later pass.
struct waiting_request {
   uint64_t    request_id = 0;
   wait_reason reason     = wait_reason::UNVERIFIED;
};

/// Everything one pass decides.
struct plan {
   std::vector<accept_action>   accepts;
   std::vector<uint64_t>        approves;    ///< BONDED by the underwriter with the window passed
   std::vector<uint64_t>        claims;      ///< APPROVED or VALID with the underwriter's bond not yet paid
   std::vector<uint64_t>        held;        ///< bonded by the underwriter and challenged: `sysio` must rule
   std::vector<uint64_t>        blocked;     ///< challenged before anyone bonded: stalls its pair until ruled
   std::vector<uint64_t>        forfeited;   ///< bonded by the underwriter and ruled INVALID (`forfeited_requests`)
   std::vector<waiting_request> waiting;
};

/// What one pass decides from.
struct plan_inputs {
   std::vector<request>              requests;   ///< `requests` rows, any issuer
   std::map<uint64_t, bond_position> bonds;      ///< the underwriter's bonds, by request id
   std::map<fc::slug_name, uint32_t> verified;   ///< outpost chain code -> last confirmed depot epoch
   std::map<fc::slug_name, uint64_t> caps;       ///< token code -> most the underwriter may have bonded at once
   std::map<fc::slug_name, uint64_t> balances;   ///< token code -> the underwriter's balance
   fc::time_point                    now{};
   /// The `sysio.andon` cord is pulled: nothing is bonded, approved or claimed until it clears.
   bool                              frozen = false;
   /// The underwriter stopped bonding after a forfeit: it still approves and claims what it bonded.
   bool                              halted = false;
};

/// Decide one pass. Only envelope requests are underwritten. OPEN requests are taken oldest first, so a pair's
/// envelopes are bonded in the order `sysio.synd` releases them; each bonds its whole remainder or waits. A bond counts
/// against its token's cap until `claim` pays it, unless it was forfeited.
inline plan plan_actions(const plan_inputs& in) {
   std::map<uint64_t, const request*> by_id;
   for (const auto& r : in.requests) by_id.emplace(r.id, &r);

   std::map<fc::slug_name, uint64_t> outstanding;   // token code -> unsettled bonds
   for (const auto& [id, b] : in.bonds) {
      const auto r = by_id.find(id);
      if (b.paid || r == by_id.end() || r->second->state == request_state::INVALID) continue;
      outstanding[r->second->token_code] += b.amount;
   }
   std::map<fc::slug_name, uint64_t> balances = in.balances;

   std::vector<const request*> ordered;
   for (const auto& r : in.requests) {
      if (is_envelope_request(r)) ordered.push_back(&r);
   }
   std::ranges::sort(ordered, {}, &request::id);

   plan out;
   out.forfeited = forfeited_requests(in.requests, in.bonds);
   for (const request* r : ordered) {
      const auto bond = in.bonds.find(r->id);
      const bool ours = bond != in.bonds.end();
      switch (r->state) {
      case request_state::OPEN: {
         if (in.frozen || in.halted || r->bonded >= r->covered) break;
         const uint64_t remainder = r->covered - r->bonded;
         auto wait = [&](wait_reason why) { out.waiting.push_back({r->id, why}); };
         const auto s = decode_statement(r->statement);
         if (!s) { wait(wait_reason::BAD_STATEMENT); break; }
         if (s->token_code != r->token_code) { wait(wait_reason::TOKEN_MISMATCH); break; }
         const auto verified = in.verified.find(s->chain_code);
         if (verified == in.verified.end() || s->epoch_index > verified->second) {
            wait(wait_reason::UNVERIFIED);
            break;
         }
         const auto cap = in.caps.find(r->token_code);
         if (cap == in.caps.end()) { wait(wait_reason::NO_CAP); break; }
         uint64_t& used = outstanding[r->token_code];
         if (used > cap->second || remainder > cap->second - used) { wait(wait_reason::OVER_CAP); break; }
         uint64_t& balance = balances[r->token_code];
         if (balance < remainder) { wait(wait_reason::LOW_BALANCE); break; }
         out.accepts.push_back({r->id, remainder});
         used += remainder;
         balance -= remainder;
         break;
      }
      case request_state::BONDED:
         if (!in.frozen && ours && in.now >= r->bonded_at + fc::seconds(r->window_sec)) out.approves.push_back(r->id);
         break;
      case request_state::APPROVED:
      case request_state::VALID:
         if (!in.frozen && ours && !bond->second.paid) out.claims.push_back(r->id);
         break;
      case request_state::INVALID:
         break;   // reported through `forfeited`, never claimed
      case request_state::HELD:
         if (ours) out.held.push_back(r->id);
         else out.blocked.push_back(r->id);
         break;
      }
   }
   return out;
}

} // namespace sysio::batch_operator_detail::underwriter
