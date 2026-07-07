#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio.opp.common/name_ops.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <sysio/permission.hpp>
#include <sysio/crypto.hpp>
#include <magic_enum/magic_enum.hpp>
#include <zpp_bits.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

namespace sysio {

using opp::types::AttestationType;
using opp::types::UnderwriteRequestStatus;
using opp::types::UnderwriteStatus;
using opp::types::OperatorStatus;
using opp::types::OperatorType;
using opp::types::ReserveStatus;
using opp::types::ChainKind;
using opp::attestations::SwapRequest;

namespace {

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

using sysio::slug_name_literals::operator""_s;

/// The WIRE token's slug — both the depot-native token code and (by
/// protocol convention) the depot chain's own registry code.
constexpr sysio::slug_name WIRE_TOKEN = "WIRE"_s;

/// WIRE token symbol (9 decimals) — the emissions/epoch denomination
/// (`sysio.system/src/emissions.cpp:42`). Never opreg's CORE_SYM (SYS, 4):
/// collateral and swap custody are different surfaces.
constexpr sysio::symbol WIRE_SYMBOL{"WIRE", 9};

/// High bit partitions depot-originated (swap-from-WIRE) uwreq ids from
/// inbound attestation ids — msgch's `mint_att_id` counts monotonically
/// from 1 and can never reach 2^63, so the two id spaces are disjoint.
constexpr uint64_t DEPOT_ORIGIN_ID_BASE = 0x8000000000000000ULL;

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

uint32_t get_current_epoch() {
   sysio::epoch::epochstate_t es(uwrit::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

/// Compose the `sha256(account || chain_code || token_code)` composite key.
/// Post v6 split-index design (§B.2): the rollup helpers (`opreg_pending_withdraws`,
/// `sum_locks_inline`) now scan the per-uint64 secondary indexes (`byaccount`,
/// `byuw`) and filter `(chain_code, token_code)` in memory instead of indexing
/// by a 24-byte composite. This helper is kept only for any caller that still
/// needs to derive the same key for cross-contract diagnostic comparison.
checksum256 compose_account_chain_token_ck(name account,
                                            sysio::slug_name chain_code,
                                            sysio::slug_name token_code) {
   std::array<uint8_t, 24> buf{};
   uint64_t acc_v = account.value;
   std::memcpy(buf.data() +  0, &acc_v,             8);
   std::memcpy(buf.data() +  8, &chain_code.value,  8);
   std::memcpy(buf.data() + 16, &token_code.value,  8);
   return sysio::sha256(reinterpret_cast<const char*>(buf.data()), buf.size());
}

/// Sum the underwriter's pending withdraws on opreg for the given
/// `(chain_code, token_code)`. Per v6 plan §B.2 (split-index design):
/// `opreg::wtdwqueue_t` exposes only uint64 secondary indexes. The `byaccount`
/// index keys on `account.value`; rows are filtered on `(chain_code,
/// token_code)` in memory. Per-account pending-withdraw counts are O(1)-ish
/// so the scan is cheap.
uint64_t opreg_pending_withdraws(name underwriter,
                                  sysio::slug_name chain_code,
                                  sysio::slug_name token_code) {
   opreg::wtdwqueue_t queue(uwrit::OPREG_ACCOUNT);
   auto idx = queue.template get_index<"byaccount"_n>();

   uint64_t total = 0;
   auto it  = idx.lower_bound(underwriter.value);
   auto end = idx.upper_bound(underwriter.value);
   for (; it != end; ++it) {
      if (it->chain_code != chain_code || it->token_code != token_code) continue;
      // Saturating: amounts are uncapped uint64 (external-chain values); a
      // wrapped subtotal would understate `reserved` and overstate availability.
      total = opp::safe::add_sat_u64(total, it->amount);
   }
   return total;
}

/// Sum this contract's active locks for the given
/// `(underwriter, chain_code, token_code)`. Per v6 plan §B.2 (split-index
/// design): `uwrit::locks_t` exposes only uint64 secondary indexes. The `byuw`
/// index keys on `underwriter.value`; rows are filtered on `(chain_code,
/// token_code)` in memory. Per-underwriter lock counts are O(1)-ish so the
/// scan is cheap.
uint64_t sum_locks_inline(name self,
                           name underwriter,
                           sysio::slug_name chain_code,
                           sysio::slug_name token_code) {
   uwrit::locks_t locks(self);
   auto idx = locks.template get_index<"byuw"_n>();

   uint64_t total = 0;
   auto it  = idx.lower_bound(underwriter.value);
   auto end = idx.upper_bound(underwriter.value);
   for (; it != end; ++it) {
      if (it->chain_code != chain_code || it->token_code != token_code) continue;
      // Saturating: amounts are uncapped uint64 (external-chain values); a
      // wrapped subtotal would understate `reserved` and overstate availability.
      total = opp::safe::add_sat_u64(total, it->amount);
   }
   return total;
}

/// Look up an underwriter's balance on opreg for the given
/// `(chain_code, token_code)`. Returns the raw stored balance — caller
/// subtracts active locks + pending withdraws to get the spendable amount.
uint64_t opreg_balance(name underwriter,
                        sysio::slug_name chain_code,
                        sysio::slug_name token_code,
                        OperatorStatus& out_status) {
   opreg::operators_t ops(uwrit::OPREG_ACCOUNT);
   opreg::operator_key op_pk{underwriter.value};
   if (!ops.contains(op_pk)) {
      out_status = OperatorStatus::OPERATOR_STATUS_UNKNOWN;
      return 0;
   }
   auto op = ops.get(op_pk);
   out_status = op.status;
   for (const auto& b : op.balances) {
      if (b.chain_code == chain_code && b.token_code == token_code) {
         return b.balance;
      }
   }
   return 0;
}

/// Compute the underwriter's spendable balance on
/// `(chain_code, token_code)`. Mirrors the sysio.opreg::available() formula:
///   balance - sum(active locks here in uwrit) - sum(pending withdraws on opreg)
/// gated by status (SLASHED / TERMINATED -> 0).
uint64_t available_via_mirrors(name self,
                                name underwriter,
                                sysio::slug_name chain_code,
                                sysio::slug_name token_code) {
   OperatorStatus status;
   uint64_t balance = opreg_balance(underwriter, chain_code, token_code, status);
   if (status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
       status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      return 0;
   }
   uint64_t locked  = sum_locks_inline(self, underwriter, chain_code, token_code);
   uint64_t pending = opreg_pending_withdraws(underwriter, chain_code, token_code);
   // Saturating: a wrap of locked+pending would understate `reserved` and
   // overstate availability — the exact direction that lets an overcommit
   // through. The cap is unreachable for real amounts.
   uint64_t reserved = opp::safe::add_sat_u64(locked, pending);
   return balance > reserved ? balance - reserved : 0;
}

/// True iff `candidate` is currently an ACTIVE underwriter on opreg: an
/// operator row that EXISTS, is typed `OPERATOR_TYPE_UNDERWRITER`, AND is in
/// `OPERATOR_STATUS_ACTIVE`. This is the authorization the race resolver
/// requires before a candidate may win and drive value settlement.
///
/// opreg's eligibility model registers every non-bootstrapped operator as
/// `UNKNOWN`, forbids bootstrapping an underwriter active at registration, and
/// only promotes an underwriter to `ACTIVE` through `opreg::processuw` once it
/// clears the role's `req_uw_collat` minimum. The balance mirror
/// (`available_via_mirrors`) zeroes only SLASHED / TERMINATED rows and ignores
/// `op.type`, so on its own it would admit a non-underwriter (PRODUCER / BATCH
/// / CHALLENGER) or a pre-activation underwriter (UNKNOWN / WARMUP / COOLDOWN)
/// that merely holds enough mirrored balance. Reads the opreg `operators` row
/// directly — the authoritative type/status source, mirrored nowhere else in
/// uwrit.
bool is_active_underwriter(name candidate) {
   opreg::operators_t ops(uwrit::OPREG_ACCOUNT);
   opreg::operator_key op_pk{candidate.value};
   if (!ops.contains(op_pk)) return false;
   const auto op = ops.get(op_pk);
   return op.type   == OperatorType::OPERATOR_TYPE_UNDERWRITER
       && op.status == OperatorStatus::OPERATOR_STATUS_ACTIVE;
}

/// Live per-spoke swap fee (basis points) from `uwconfig`, read fresh so the
/// ingestion variance check, the race-time recheck, and settlement all charge
/// one rate. `self` is the uwrit contract account (where the singleton lives).
uint32_t current_fee_bps(name self) {
   uwrit::uwconfig_t cfg(self);
   return cfg.get_or_default(uwrit::uw_config{}).fee_bps;
}

/// Find a reserve by its triple key, returning the row pointer-equivalent
/// optional. Mirrors sysio.reserv's primary-key access. Returns
/// `std::nullopt` when no such reserve exists (the variance check then
/// treats the quote as 0 — implicit skip).
std::optional<reserve::reserve_row> find_reserve(sysio::slug_name chain_code,
                                                  sysio::slug_name token_code,
                                                  sysio::slug_name reserve_code) {
   reserve::reserves_t reserves(uwrit::RESERVE_ACCOUNT);
   reserve::reserve_key pk{chain_code, token_code, reserve_code};
   if (!reserves.contains(pk)) return std::nullopt;
   return reserves.get(pk);
}

/// Quote `src_amount` of (src_chain, src_token, src_reserve) into
/// (dst_chain, dst_token, dst_reserve) along the depot's live curve. Mirrors
/// `sysio.reserv::swapquote` exactly — the shared weighted-Bancor kernel (each
/// reserve's `connector_weight_bps`) and the SAME post-fee reduction (`fee_bps`
/// out of the WIRE leg) — so the variance check at SWAP_REQUEST receipt time
/// matches what settlement will deliver, without an inline action call into
/// reserv. Returns 0 if any required reserve is missing or not ACTIVE (caller
/// treats 0 as "no quote available, skip variance check").
///
/// A WIRE endpoint skips that leg's reserve (the depot IS the WIRE side);
/// WIRE->WIRE is a plain `src_amount` passthrough.
uint64_t swap_quote(sysio::slug_name src_chain_code,
                    sysio::slug_name src_token_code,
                    sysio::slug_name src_reserve_code,
                    sysio::slug_name dst_chain_code,
                    sysio::slug_name dst_token_code,
                    sysio::slug_name dst_reserve_code,
                    uint64_t src_amount,
                    uint32_t fee_bps) {
   if (src_amount == 0) return 0;
   const bool src_is_wire = (src_token_code == WIRE_TOKEN);
   const bool dst_is_wire = (dst_token_code == WIRE_TOKEN);
   if (src_is_wire && dst_is_wire) return src_amount;

   auto active = [](std::optional<reserve::reserve_row>&& r)
                    -> std::optional<reserve::reserve_row> {
      if (!r || r->status != ReserveStatus::RESERVE_STATUS_ACTIVE) return std::nullopt;
      return r;
   };

   uint64_t sc = 0, sw = 0; uint32_t scw = 0;
   if (!src_is_wire) {
      auto r = active(find_reserve(src_chain_code, src_token_code, src_reserve_code));
      if (!r) return 0;
      sc = r->reserve_chain_amount; sw = r->reserve_wire_amount; scw = r->connector_weight_bps;
   }
   uint64_t dc = 0, dw = 0; uint32_t dcw = 0;
   if (!dst_is_wire) {
      auto r = active(find_reserve(dst_chain_code, dst_token_code, dst_reserve_code));
      if (!r) return 0;
      dc = r->reserve_chain_amount; dw = r->reserve_wire_amount; dcw = r->connector_weight_bps;
   }
   return opp::amm::quote_swap(src_is_wire, sc, sw, scw,
                               dst_is_wire, dc, dw, dcw,
                               src_amount, fee_bps);
}

/// True iff every NON-WIRE leg of a swap has an ACTIVE reserve row. Lets callers
/// distinguish a zero `swap_quote` caused by an unprovisioned / not-ACTIVE
/// reserve (dev & smoke clusters — the variance check is intentionally skipped)
/// from one caused by a *degenerate but ACTIVE* reserve: a side drained to zero,
/// extreme connector weights, or a WIRE leg too small for the weighted-Bancor
/// kernel to price (it floors the output to 0). In that case a zero quote is a
/// real pricing failure and the caller MUST fail closed — otherwise the
/// user-supplied `target_amount` bypasses the variance check and settlement
/// over-debits the reserve (WSA-041). A WIRE endpoint has no reserve and is not
/// required (the depot IS the WIRE side), mirroring `swap_quote`.
bool required_reserves_active(sysio::slug_name src_chain_code,
                             sysio::slug_name src_token_code,
                             sysio::slug_name src_reserve_code,
                             sysio::slug_name dst_chain_code,
                             sysio::slug_name dst_token_code,
                             sysio::slug_name dst_reserve_code) {
   auto leg_active = [](sysio::slug_name c, sysio::slug_name t, sysio::slug_name r) {
      auto row = find_reserve(c, t, r);
      return row && row->status == ReserveStatus::RESERVE_STATUS_ACTIVE;
   };
   if (src_token_code != WIRE_TOKEN
       && !leg_active(src_chain_code, src_token_code, src_reserve_code)) return false;
   if (dst_token_code != WIRE_TOKEN
       && !leg_active(dst_chain_code, dst_token_code, dst_reserve_code)) return false;
   return true;
}

/// Encode + queue a SWAP_REVERT attestation back to the source outpost when
/// the variance check fails. The outpost matches the original SWAP_REQUEST
/// via `original_swap_message_id` (low 8 bytes carry the depot's
/// attestation_id; see msgch's SWAP_REMIT dispatch for the matching decode
/// convention).
///
/// The slug_name pair `(source_chain_code, source_reserve_code)` is included
/// so the outpost can locate the matching local reserve when refunding.
void emit_swap_revert(name self,
                      uint64_t chain_code,
                      uint64_t attestation_id,
                      const opp::attestations::SwapRequest& sr,
                      sysio::slug_name source_chain_code,
                      sysio::slug_name source_reserve_code,
                      const std::string& reason) {
   opp::attestations::SwapRevert rev;
   rev.original_swap_message_id.assign(32, 0);
   for (size_t i = 0; i < 8; ++i) {
      rev.original_swap_message_id[i] = static_cast<char>((attestation_id >> (i * 8)) & 0xff);
   }
   rev.depositor           = sr.actor;
   rev.refund_amount       = sr.source_amount;
   rev.reason              = reason;
   rev.source_chain_code   = source_chain_code.value;
   rev.source_reserve_code = source_reserve_code.value;

   // `no_size{}` — raw protobuf bytes for the outpost decoder; the default
   // `zpp::bits::data_out` form prepends a 4-byte LE length prefix that
   // corrupts the first field tag on the receiving side.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(rev);

   action(
      permission_level{self, "active"_n},
      uwrit::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(chain_code,
         opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REVERT, encoded)
   ).send();
}

/// Resolve a `sysio::slug_name` chain identifier to its `ChainKind` by
/// reading the `sysio.chains::chains` registry row. Returns `std::nullopt`
/// when no chain row exists for the code — callers treat that as "no
/// outpost for this chain, skip the queueout".
std::optional<ChainKind> chain_kind_for_code(sysio::slug_name chain_code) {
   sysio::chains::chains_t tbl(uwrit::CHAINS_ACCOUNT);
   sysio::chains::chain_key pk{chain_code};
   if (!tbl.contains(pk)) return std::nullopt;
   return tbl.get(pk).kind;
}

/// Look up the depot's outpost id for `chain_code` via the chains registry.
/// Returns `std::nullopt` when no chain row is registered (per
/// `feedback_no_zero_sentinels` — outpost id 0 is a real id, so 0 must not
/// double as "missing").
///
/// Post v6 cross-contract realignment: chain rows live in
/// `sysio.chains::chains` keyed by `code` (slug_name); the legacy
/// `sysio.epoch::outposts` table is gone. The "outpost id" returned here is
/// the chain's `code.value` (uint64). The depot-self row is filtered out so
/// WIRE-direct flows skip queueouts cleanly.
std::optional<uint64_t> find_outpost_id_for_chain(sysio::slug_name chain_code) {
   sysio::chains::chains_t chains_tbl(uwrit::CHAINS_ACCOUNT);
   sysio::chains::chain_key pk{chain_code};
   if (!chains_tbl.contains(pk)) return std::nullopt;
   const auto row = chains_tbl.get(pk);
   if (row.is_depot) return std::nullopt;   // WIRE-direct flows don't queueout
   return chain_code.value;
}

/// True iff `chain_code` is a REGISTERED chain row flagged `is_depot`.
/// An unregistered chain is NOT the depot — `createuwreq` rejects swaps on
/// unregistered chains outright rather than mistaking them for WIRE legs
/// (which would silently waive that leg's signature/bond/lock).
bool leg_is_depot(sysio::slug_name chain_code) {
   sysio::chains::chains_t chains_tbl(uwrit::CHAINS_ACCOUNT);
   sysio::chains::chain_key pk{chain_code};
   if (!chains_tbl.contains(pk)) return false;
   return chains_tbl.get(pk).is_depot;
}

/// True iff the chain is registered AND active in sysio.chains.
bool chain_registered_active(sysio::slug_name chain_code) {
   sysio::chains::chains_t chains_tbl(uwrit::CHAINS_ACCOUNT);
   sysio::chains::chain_key pk{chain_code};
   if (!chains_tbl.contains(pk)) return false;
   return chains_tbl.get(pk).active;
}

/// The depot's own chain code (the singleton `is_depot` row in
/// sysio.chains — `code = "WIRE"` by protocol convention). Scans the
/// registry, which holds a handful of rows. Returns nullopt before the
/// depot row is registered (pre-bootstrap).
std::optional<sysio::slug_name> depot_chain_code() {
   sysio::chains::chains_t tbl(uwrit::CHAINS_ACCOUNT);
   for (auto it = tbl.begin(); it != tbl.end(); ++it) {
      if (it->is_depot) return it->code;
   }
   return std::nullopt;
}

/// Find a reserve and require ACTIVE status — the shape every settlement
/// pre-check needs. Returns nullopt when missing or not ACTIVE.
std::optional<reserve::reserve_row> find_active_reserve(sysio::slug_name chain_code,
                                                         sysio::slug_name token_code,
                                                         sysio::slug_name reserve_code) {
   auto row = find_reserve(chain_code, token_code, reserve_code);
   if (!row) return std::nullopt;
   if (row->status != ReserveStatus::RESERVE_STATUS_ACTIVE) return std::nullopt;
   return row;
}

/// Parse a WIRE account name from its string-spelling bytes (the canonical
/// `ChainAddress.address` encoding for CHAIN_KIND_WIRE). Delegates to the shared
/// `sysio::opp::safe::parse_wire_account_name`, which validates charset, length,
/// and final-symbol bounds BEFORE constructing the `name`, so this never throws
/// inside the evalcons dispatch chain. The full CDT name domain is accepted —
/// including a legitimate 13-byte name whose final symbol fits the 4-bit final
/// slot. An empty address is not a valid principal here.
std::optional<name> parse_wire_name(const std::vector<char>& bytes) {
   return sysio::opp::safe::parse_wire_account_name(std::string_view{bytes.data(), bytes.size()});
}

/// A WIRE account name as its string-spelling bytes — the inverse of
/// `parse_wire_name`, used to stamp WIRE-side principals into
/// ChainAddress-shaped fields (depositor, actor).
std::vector<char> wire_name_bytes(name n) {
   std::string s = n.to_string();
   return {s.begin(), s.end()};
}

/// Disposition of a pre-settlement attempt to build the outbound SWAP_REMIT
/// for a winning candidate. Lets `try_select_winner` unwind a race
/// non-throwing instead of letting a `check()` abort the evalcons dispatch
/// chain and stall OPP consensus chain-wide.
enum class swap_remit_disp {
   ok,            ///< envelope built — proceed to settle + `queue_swap_remit`
   terminal,      ///< no underwriter can EVER remit this uwreq — REJECT + refund
   disqualified,  ///< THIS candidate cannot remit — skip it, leave uwreq PENDING
};

/// Pre-validate + build (but do NOT send) the outbound SWAP_REMIT envelope
/// for `candidate` winning `req`, mutating no state.
///
/// **Non-throwing.** The former `emit_swap_remit` ran AFTER the caller's
/// `reserv::applyswap` / `applyfromwire` reserve mutation and resolved the
/// recipient / destination outpost / underwriter identity with `check()`.
/// Inside the synchronous evalcons → dispatch → rcrdcommit → try_select_winner
/// chain, any such throw aborts consensus application for the whole outpost
/// envelope and stalls OPP epoch advancement chain-wide. This function instead
/// reports a `swap_remit_disp` so the caller can disqualify the candidate (or
/// reject the uwreq) cleanly. It is called BEFORE any lock / CONFIRMED /
/// reserve write; `queue_swap_remit` (below) only ships the pre-built envelope,
/// AFTER the reserve books have moved (so every intervening quote prices the
/// post-swap books).
///
/// On `ok`, `dst_outpost_id` + `encoded` carry the ready-to-send envelope.
/// Failure classification:
///   * stored-request decode / dst outpost / dst chain-kind — uwreq-wide and
///     identical for every candidate ⇒ `terminal` (REJECT + refund/revert).
///   * winner's destination-chain authex link / pubkey — candidate-specific
///     (another underwriter may hold a valid link) ⇒ `disqualified`.
swap_remit_disp try_build_swap_remit(const uwrit::uw_request_t& req,
                                     name candidate,
                                     uint64_t& dst_outpost_id,
                                     std::vector<char>& encoded) {
   // Decode the stored SwapRequest for its `recipient` (the row keeps only
   // chain/kind/amount summaries). Same bytes for every candidate ⇒ terminal.
   opp::attestations::SwapRequest sr;
   {
      auto in = zpp::bits::in{
         std::span{req.attestation_inbound_data.data(),
                    req.attestation_inbound_data.size()},
         zpp::bits::no_size{}};
      if (in(sr) != zpp::bits::errc{}) return swap_remit_disp::terminal;
   }

   // Destination outpost id + ChainKind from the `sysio.chains` registry —
   // chain-level config; if unresolved, no winner can ever remit ⇒ terminal.
   auto dst_outpost_opt = find_outpost_id_for_chain(req.dst_chain_code);
   if (!dst_outpost_opt) return swap_remit_disp::terminal;
   auto dst_kind_opt = chain_kind_for_code(req.dst_chain_code);
   if (!dst_kind_opt) return swap_remit_disp::terminal;
   const ChainKind dst_kind = *dst_kind_opt;

   // Resolve the winning underwriter's destination-chain pubkey from
   // `sysio.authex::links` (`bynamechain`) so the SwapRemit carries the
   // underwriter's auditable identity — the destination outpost
   // cross-references it against the UNDERWRITE_INTENT_COMMIT it already saw.
   // A winner without a dst-chain link cannot ship a SwapRemit with a
   // populated underwriter, but this is candidate-specific: disqualify them
   // so the race can resolve for another underwriter (NOT terminal).
   sysio::authex::links_t links(uwrit::AUTHEX_ACCOUNT);
   auto idx = links.get_index<"bynamechain"_n>();
   auto it = idx.find(sysio::to_namechain_key(candidate, dst_kind));
   if (it == idx.end()) return swap_remit_disp::disqualified;
   std::vector<char> uw_addr = sysio::pubkey_to_bytes(it->pub_key);
   if (uw_addr.empty()) return swap_remit_disp::disqualified;

   // All identities resolved — build the envelope.
   opp::attestations::SwapRemit remit;
   remit.recipient = sr.recipient;
   // FORCE recipient.kind to the dst chain's actual ChainKind. The ETH
   // outpost ships SwapRequest with `recipient.kind = CHAIN_KIND_UNKNOWN`
   // ("depot routes by chain_code, outposts decode by their own kind"), but
   // the SOL off-chain cranker (`extract_inbound_recipient_pubkeys`) forwards
   // the recipient pubkey only when `kind == CHAIN_KIND_SVM`; UNKNOWN → dropped
   // → on-chain `handle_swap_remit` rejects "recipient not in
   // remaining_accounts". Per the project rule against 0-as-sentinel enums.
   remit.recipient.kind = dst_kind;
   remit.amount = opp::types::TokenAmount{
      .token_code = req.dst_token_code.value,
      .amount     = static_cast<int64_t>(req.dst_amount),
   };
   // `original_message_id` low 8 bytes encode uwreq_id; the reflected
   // SWAP_REMIT envelope back to msgch's dispatch uses this for the
   // release-trigger decode (see sysio.msgch.cpp's SWAP_REMIT case).
   remit.original_message_id.assign(32, 0);
   for (size_t i = 0; i < 8; ++i) {
      remit.original_message_id[i] =
         static_cast<char>((req.id >> (i * 8)) & 0xff);
   }
   remit.chain_code          = req.dst_chain_code.value;
   remit.reserve_code        = req.dst_reserve_code.value;
   remit.underwriter.kind    = dst_kind;
   remit.underwriter.address = std::move(uw_addr);
   remit.unlock_timestamp    = 0;

   // `no_size{}` — see emit_swap_revert for the rationale.
   encoded.clear();
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(remit);
   dst_outpost_id = *dst_outpost_opt;
   return swap_remit_disp::ok;
}

/// Queue a pre-built SWAP_REMIT envelope (from `try_build_swap_remit`) to the
/// destination outpost. Sent by `try_select_winner` AFTER the reserve mutation
/// in the same transaction, so every intervening quote prices the post-swap
/// books. The destination outpost's ReserveManager (ETH) / reserve PDA (SOL)
/// pays the recipient inline via `_handleSwapRemit` / `handle_swap_remit`.
/// Non-throwing.
void queue_swap_remit(name self, uint64_t dst_outpost_id,
                      const std::vector<char>& encoded) {
   action(
      permission_level{self, "active"_n},
      uwrit::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(dst_outpost_id,
         opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REMIT, encoded)
   ).send();
}

/// Verify the embedded signature in `uic_bytes` was produced by a key on
/// EITHER the `underwriter` account's `active` OR `owner` permission, over
/// the digest `sha256(serialize(uic_with_signature_blanked))`. Returns
/// true on first matching key. Returns false (never throws) when the bytes
/// are empty, the proto fails to decode, the embedded signature is missing,
/// or no `active`/`owner` key recovers to the signature. Other permissions
/// (custom permission names) are intentionally NOT checked: the
/// underwriter_plugin's signature_provider_manager_plugin config is pinned
/// to one of `active`/`owner`, so accepting a custom permission would let
/// an attacker bypass the configuration constraint.
///
/// **Per `feedback_opp_handlers_never_throw.md` — this MUST stay
/// non-throwing.** It's called from `try_select_winner`, which runs inside
/// the evalcons inline-action chain; a `check()`/throw here halts
/// consensus. The defensive size+tag bounds below reject the structurally
/// invalid signatures before recovery. Recovery itself uses
/// `sysio::recover_key`; converting its remaining contract-observable
/// throws (unactivated variant, recovery-math failure) to an rc = -1
/// sentinel — so this is non-throwing on every input — is tracked as
/// separate host+CDT work landing in its own PR. Until then a crafted but
/// size/tag-valid signature can still throw here.
bool verify_uic_signature(name underwriter,
                           const std::vector<char>& uic_bytes) {
   if (uic_bytes.empty()) return false;

   // Decode the UIC payload.
   opp::attestations::UnderwriteIntentCommit uic;
   {
      auto in = zpp::bits::in{
         std::span{uic_bytes.data(), uic_bytes.size()},
         zpp::bits::no_size{}};
      if (in(uic) != zpp::bits::errc{}) return false;
   }

   // Save the signature, blank it, recompute the digest.
   std::vector<char> sig_bytes_view{uic.signature.begin(), uic.signature.end()};
   if (sig_bytes_view.empty()) return false;
   uic.signature.clear();

   std::vector<char> blanked;
   auto out = zpp::bits::out{blanked, zpp::bits::no_size{}};
   if (out(uic) != zpp::bits::errc{}) return false;

   sysio::checksum256 digest =
      sysio::sha256(blanked.data(), blanked.size());

   // Defensive bounds before invoking the chain signature intrinsic. The
   // first byte of a packed `sysio::signature` is the variant tag
   // (0=K1, 1=R1, 2=WebAuthN, 3=EM, 4=ED25519, 5=BLS). Anything outside
   // that range or sized outside the smallest/largest legal variant is
   // tossed before the intrinsic gets a chance to attempt recovery.
   if (sig_bytes_view.size() < 2 || sig_bytes_view.size() > 1024) return false;
   const uint8_t tag = static_cast<uint8_t>(sig_bytes_view[0]);
   if (tag > 5) return false;

   // Unpack the underwriter's signature variant from the embedded bytes.
   sysio::signature parsed_sig;
   {
      sysio::datastream<const char*> ds{sig_bytes_view.data(),
                                          sig_bytes_view.size()};
      ds >> parsed_sig;
   }

   // Recover the public key — non-throwing variant. The host wraps the
   // throwing recovery path in try/catch and returns `std::nullopt` on
   // any failure (malformed bytes, unactivated sig type, recovery math
   // failure, subjective-size limit). Required because CDT compiles
   // with `-fno-exceptions` and `try_select_winner` cannot halt the
   // dispatch on attacker-controlled bytes (per
   // `feedback_opp_handlers_never_throw.md`).
   auto recovered_opt = sysio::try_recover_key(digest, parsed_sig);
   if (!recovered_opt) return false;
   const sysio::public_key& recovered = *recovered_opt;

   // Only `owner` and `active` permissions are considered. The
   // underwriter_plugin's signature_provider_manager_plugin config is
   // pinned to one of those two (see plugin docs); accepting a custom
   // permission would open a separate authorization surface that nothing
   // else on the platform validates.
   constexpr sysio::name OWNER_PERM  = "owner"_n;
   constexpr sysio::name ACTIVE_PERM = "active"_n;
   for (auto perm : { ACTIVE_PERM, OWNER_PERM }) {
      auto rec_opt = sysio::get_permission(underwriter, perm);
      if (!rec_opt) continue;
      for (const auto& kw : rec_opt->auth.keys) {
         if (kw.key == recovered) return true;
      }
   }
   return false;
}

/// Allocate a fresh `lock_id` from the uwcounters singleton.
uint64_t next_lock_id(name self) {
   uwrit::uwcounters_t ctr_tbl(self);
   auto ctr = ctr_tbl.get_or_default(uwrit::uw_counters{});
   uint64_t id = ctr.next_lock_id;
   ctr.next_lock_id = id + 1;
   ctr_tbl.set(ctr, ram_payer);
   return id;
}

/// Allocate a depot-origin (swap-from-WIRE) id: the high-bit-tagged
/// sequence that doubles as both the fwqueue row id and the eventual
/// uwreq id. Disjoint from msgch's inbound attestation-id space.
uint64_t next_fromwire_id(name self) {
   uwrit::uwcounters_t ctr_tbl(self);
   auto ctr = ctr_tbl.get_or_default(uwrit::uw_counters{});
   uint64_t id = DEPOT_ORIGIN_ID_BASE | ctr.next_fromwire_seq;
   ctr.next_fromwire_seq += 1;
   ctr_tbl.set(ctr, ram_payer);
   return id;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void uwrit::setconfig(uint32_t fee_bps,
                      uint64_t collateral_lock_duration_ms) {
   require_auth(get_self());
   // Reject a 100% (or higher) fee: it zeroes the post-fee WIRE leg
   // (`net == 0`), which let a swap debit destination reserve liquidity while
   // crediting zero WIRE (SEC-26 / WSA-042). MAX_FEE_BPS == 9999 keeps the
   // remainder positive for every positive input.
   check(fee_bps <= MAX_FEE_BPS,
         "fee_bps must be below 10000 (100%): a 100% fee zeroes the post-fee WIRE leg");
   check(collateral_lock_duration_ms > 0,
         "collateral_lock_duration_ms must be positive");
   // Reject a duration so large that `now_ms + collateral_lock_duration_ms`
   // would wrap past UINT64_MAX and yield a lock that expires in the past,
   // releasing collateral the instant it is placed.
   check(collateral_lock_duration_ms <= MAX_COLLATERAL_LOCK_DURATION_MS,
         "collateral_lock_duration_ms exceeds the one-year ceiling");

   uwconfig_t cfg_tbl(get_self());
   uw_config cfg = cfg_tbl.get_or_default(uw_config{});
   cfg.fee_bps                     = fee_bps;
   cfg.collateral_lock_duration_ms = collateral_lock_duration_ms;
   cfg_tbl.set(cfg, ram_payer);
}

// ---------------------------------------------------------------------------
//  createuwreq — called inline from sysio.msgch when SWAP arrives
// ---------------------------------------------------------------------------
void uwrit::createuwreq(uint64_t attestation_id,
                         opp::types::AttestationType type,
                         uint64_t chain_code,
                         std::vector<char> data) {
   require_auth(MSGCH_ACCOUNT);

   uwreqs_t reqs(get_self());
   auto pk = id_key{attestation_id};
   // Duplicate-delivery is the protocol's normal idempotency case — every
   // batch op re-relays the same envelope on each cron tick until the
   // depot advances the epoch, so the second, third, ... batch op's
   // `deliver → evalcons → dispatch → createuwreq` call lands on a row
   // that's already present. Per `feedback_opp_handlers_never_throw.md`
   // a `check()` here halts `evalcons` and stalls consensus across the
   // chain. Silently no-op the duplicate and let the relay continue.
   if (reqs.contains(pk)) {
      sysio::print("createuwreq: uwreq ", attestation_id,
                   " already exists, skipping idempotent re-delivery\n");
      return;
   }

   // Only SWAP_REQUEST attestations create UWREQs — msgch's dispatch routes
   // other types directly to their handlers, not through createuwreq. A
   // non-SWAP type can only arrive from a future/buggy dispatcher; per
   // `feedback_opp_handlers_never_throw.md` a check() here would halt evalcons
   // and stall consensus, so log + skip instead of asserting.
   if (type != AttestationType::ATTESTATION_TYPE_SWAP_REQUEST) {
      sysio::print("createuwreq: non-SWAP_REQUEST attestation ", attestation_id,
                   " routed to createuwreq, skipping\n");
      return;
   }

   // Decode the operator-supplied SwapRequest. A malformed payload cannot be
   // refunded (emit_swap_revert needs the decoded actor / source_amount to
   // tell the outpost which deposit to reverse — exactly what failed to
   // decode), and a check() here would abort the consensus-tipping delivery
   // before any uwreq row exists, stalling epoch advancement chain-wide. Log +
   // skip: no row is created, so no state is corrupted and the relay
   // continues. Per `feedback_opp_handlers_never_throw.md`.
   SwapRequest sr;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      if (in(sr) != zpp::bits::errc{}) {
         sysio::print("createuwreq: failed to decode SwapRequest for attestation ",
                      attestation_id, ", skipping (undecodable — cannot revert)\n");
         return;
      }
   }

   // Pull the slug_name triples + amounts out of the decoded SwapRequest.
   // The source token's code lives on the TokenAmount; the source chain
   // and source reserve are top-level fields. Destination has all three
   // top-level.
   const sysio::slug_name src_chain_code{sr.source_chain_code};
   const sysio::slug_name src_token_code{sr.source_amount.token_code};
   const sysio::slug_name src_reserve_code{sr.source_reserve_code};
   const sysio::slug_name dst_chain_code{sr.target_chain_code};
   const sysio::slug_name dst_token_code{sr.target_token_code};
   const sysio::slug_name dst_reserve_code{sr.target_reserve_code};
   // WSA-028: source_amount is signed on the wire. Gate it through the shared
   // fail-closed parser; a non-positive or out-of-range value yields nullopt and
   // is reverted (refund on the proven outpost) by the positivity guard below,
   // never wrapped into a huge src_amount that would corrupt the swap quote and
   // reserve settlement. Never-throw: revert, do not check().
   const std::optional<uint64_t> src_amount_opt =
      opp::safe::to_depot_amount(static_cast<int64_t>(sr.source_amount.amount));

   // WSA-005 source-chain binding. `chain_code` is the source outpost `sysio.msgch::dispatch`
   // proved via `deliver` (an active, non-depot `sysio.chains` row); `sr.source_chain_code` is the
   // payload's self-asserted source. They MUST be identical — a SwapRequest about a deposit on
   // chain X is always relayed by outpost X. A divergence means an envelope proven from outpost A is
   // claiming the swap was funded on a different chain B; settling it would lock/draw against B's
   // source reserve while the user's funds are escrowed on A — a cross-chain settlement forgery.
   // Refund on the PROVEN outpost (A, == `chain_code`, where the deposit actually sits) and create
   // no uwreq. Never throw: we are inside the evalcons dispatch chain, where a `check()` stalls
   // consensus chain-wide (`feedback_opp_handlers_never_throw`). Checked before the amount/registry
   // guards because provenance is the most fundamental precondition.
   if (sr.source_chain_code != chain_code) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       sysio::slug_name{chain_code}, src_reserve_code,
                       "SwapRequest rejected: source chain does not match the proven "
                       "delivering outpost (cross-chain provenance mismatch)");
      return;
   }

   // Structural guards — refund via SwapRevert, never throw (we are inside
   // the evalcons dispatch chain). Zero amounts are rejected up front so
   // every downstream lock/settlement amount is provably positive (a
   // zero-amount lock would trip `opreg::releaselock`'s amount check from
   // inside `chklocks` at epoch advance — a consensus stall).
   if (!src_amount_opt || sr.target_amount == 0) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: source and target amounts must be positive");
      return;
   }
   const uint64_t src_amount = *src_amount_opt;
   if (!chain_registered_active(src_chain_code) || !chain_registered_active(dst_chain_code)) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: unregistered or inactive chain");
      return;
   }
   const bool src_depot = leg_is_depot(src_chain_code);
   const bool dst_depot = leg_is_depot(dst_chain_code);
   if (src_depot && dst_depot) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: WIRE->WIRE swap is degenerate — "
                       "use sysio.token::transfer");
      return;
   }
   if (src_depot) {
      // Swap-from-WIRE originates ON the depot (swapfromwire -> drainfwq);
      // an outpost claiming WIRE as its source chain is bogus.
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: swap-from-WIRE cannot originate "
                       "from an outpost");
      return;
   }

   // Hard-fail any SwapRequest without a populated `source_tx_id`. The
   // off-chain underwriter verify path uses this id to confirm a real
   // on-chain deposit backs the swap before committing collateral; a
   // SwapRequest without it can't be verified, and an outpost is
   // required to populate the field at swap-emit time. Per
   // `feedback_opp_handlers_never_throw.md` we cannot `check()`/throw
   // here (we're inside the evalcons dispatch chain — a throw stalls
   // consensus); instead emit a SwapRevert back to the source outpost so
   // the user's deposit is refunded and the run continues.
   if (sr.source_tx_id.empty()) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: source_tx_id is required "
                       "(no SwapRequest may be emitted without a "
                       "populated source-chain transaction id)");
      return;
   }

   // Privacy gate — a private reserve only swaps against a counterpart
   // reserve owned by the same WIRE account (the authex-linked matcher
   // recorded at match time), and is excluded from WIRE-endpoint swaps
   // entirely. Ownership is immutable while a reserve is ACTIVE, so this
   // ingestion-time gate needs no race-time recheck.
   {
      const auto src_r = find_reserve(src_chain_code, src_token_code, src_reserve_code);
      if (dst_depot) {
         // Swap-to-WIRE: only the source reserve exists — it must be public.
         if (src_r && src_r->is_private) {
            emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                             src_chain_code, src_reserve_code,
                             "SwapRequest rejected: private reserves are "
                             "excluded from WIRE-endpoint swaps");
            return;
         }
      } else {
         const auto dst_r = find_reserve(dst_chain_code, dst_token_code, dst_reserve_code);
         const bool src_priv = src_r && src_r->is_private;
         const bool dst_priv = dst_r && dst_r->is_private;
         if (src_priv || dst_priv) {
            const bool same_owner = src_r && dst_r
                                    && src_r->owner != name{}
                                    && src_r->owner == dst_r->owner;
            if (!same_owner) {
               emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                                src_chain_code, src_reserve_code,
                                "SwapRequest rejected: private reserve pairing "
                                "violation — counterpart reserve must be owned "
                                "by the same WIRE account");
               return;
            }
         }
      }
   }

   // Variance-tolerance check via sysio.reserv mirror. If no matching
   // ACTIVE reserve exists for either leg the quote returns 0 and the
   // variance check is implicitly skipped — the swap proceeds to the
   // underwriter race. This lets dev / smoke clusters without provisioned
   // LPs continue to operate while still applying the check the moment
   // matching reserves are present.
   const uint64_t current_quote = swap_quote(src_chain_code, src_token_code, src_reserve_code,
                                              dst_chain_code, dst_token_code, dst_reserve_code,
                                              src_amount, current_fee_bps(get_self()));
   if (sr.target_amount != 0) {
      if (current_quote == 0) {
         // Zero quote: skip only when a required reserve is genuinely
         // unprovisioned / not ACTIVE (dev & smoke clusters). If every required
         // reserve IS ACTIVE, a zero quote is an unpriceable/degenerate reserve
         // — fail closed so target_amount cannot bypass variance and over-debit
         // at settlement (WSA-041).
         if (required_reserves_active(src_chain_code, src_token_code, src_reserve_code,
                                      dst_chain_code, dst_token_code, dst_reserve_code)) {
            emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                             src_chain_code, src_reserve_code,
                             "unpriceable reserve: zero quote from an ACTIVE reserve "
                             "(degenerate weights/balances) — fail closed");
            return;   // no UWREQ created
         }
      } else {
         uint64_t target   = sr.target_amount;
         uint64_t diff     = current_quote > target ? current_quote - target : target - current_quote;
         // tolerance_bps / 10000 of target; computed in uint128 to avoid overflow.
         uint128_t allowed = (static_cast<uint128_t>(target) * sr.target_tolerance_bps) / 10000u;
         if (static_cast<uint128_t>(diff) > allowed) {
            emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                             src_chain_code, src_reserve_code,
                             "variance exceeded tolerance: target=" + std::to_string(target)
                             + " current=" + std::to_string(current_quote)
                             + " tolerance_bps=" + std::to_string(sr.target_tolerance_bps));
            return;   // no UWREQ created
         }
      }
   }

   reqs.emplace(ram_payer, pk, uw_request_t{
      .id                        = attestation_id,
      .type                      = type,
      .status                    = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING,
      .src_chain_code            = src_chain_code,
      .src_token_code            = src_token_code,
      .src_reserve_code          = src_reserve_code,
      .src_amount                = src_amount,
      .dst_chain_code            = dst_chain_code,
      .dst_token_code            = dst_token_code,
      .dst_reserve_code          = dst_reserve_code,
      .dst_amount                = sr.target_amount,
      .variance_tolerance_bps    = sr.target_tolerance_bps,
      .source_tx_id              = sr.source_tx_id,
      .depositor                 = sr.actor.address,
      .commits_by                = {},
      .winner                    = name{},
      .committed_at_ms           = 0,
      .settled_at_ms             = 0,
      .expires_at_epoch          = 0,
      .attestation_inbound_data  = std::move(data),
      .attestation_outbound_data = {},
   });
}

// ---------------------------------------------------------------------------
//  Internal: try_select_winner — race resolver
// ---------------------------------------------------------------------------

namespace {

/// Helper: find or create the commit_entry for `underwriter` inside an
/// uw_request_t. Returns iterator-like reference into the in-place vector.
uwrit::commit_entry* find_or_create_commit(uwrit::uw_request_t& req, name underwriter) {
   for (auto& c : req.commits_by) {
      if (c.underwriter == underwriter) return &c;
   }
   req.commits_by.push_back(uwrit::commit_entry{
      .underwriter = underwriter,
   });
   return &req.commits_by.back();
}

/// Mark a single candidate's commit_entry as race-disqualified and leave the
/// uwreq PENDING so the race can still resolve for another valid underwriter.
/// Non-throwing.
///
/// Uses the dedicated `UNDERWRITE_STATUS_DISQUALIFIED` status — a
/// candidate-specific, pre-settlement invalidity (bad UIC signature,
/// insufficient bond, or a missing destination authex link). It is
/// deliberately distinct from `UNDERWRITE_STATUS_SLASHED` (an economic
/// punishment that actually burned collateral) and from
/// `UNDERWRITE_STATUS_RELEASED` (a clean race loser whose commit was valid):
/// conflating the three would mislead downstream indexing and audits. A
/// disqualified entry is reclaimable — a later `rcrdcommit` re-arms it to
/// `INTENT_SUBMITTED` for re-evaluation.
void disqualify_candidate(uwrit::uwreqs_t& reqs, const uwrit::id_key& pk,
                          name candidate, const std::string& reason) {
   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, candidate);
      c->status = UnderwriteStatus::UNDERWRITE_STATUS_DISQUALIFIED;
      c->reason = reason;
   });
}

/// Terminally reject a uwreq that can never settle: refund the source side
/// and mark the row REJECTED, releasing any in-flight commits. **Non-throwing**
/// — safe inside the evalcons dispatch chain (a `check()` here would stall OPP
/// consensus chain-wide). The refund routes by source-leg kind:
///   * outpost source (`src_needed`): best-effort SWAP_REVERT back to the
///     source outpost (requires the stored SwapRequest to decode; a depot
///     source has no outpost to route to, so `find_outpost_id_for_chain`
///     returns nullopt and the revert is skipped).
///   * depot source   (from-WIRE):    refund the escrowed WIRE to the
///     depositor via `reserv::refundwire`.
void reject_and_refund(name self, uwrit::uwreqs_t& reqs, const uwrit::id_key& pk,
                       const uwrit::uw_request_t& req, bool src_needed,
                       const std::string& revert_reason,
                       const std::string& commit_reason) {
   if (src_needed) {
      opp::attestations::SwapRequest sr;
      auto in = zpp::bits::in{
         std::span{req.attestation_inbound_data.data(),
                    req.attestation_inbound_data.size()},
         zpp::bits::no_size{}};
      if (in(sr) == zpp::bits::errc{}) {
         if (auto src_outpost_opt = find_outpost_id_for_chain(req.src_chain_code)) {
            emit_swap_revert(self, *src_outpost_opt, req.id, sr,
                             req.src_chain_code, req.src_reserve_code, revert_reason);
         }
      }
   } else if (auto user = parse_wire_name(req.depositor)) {
      // Swap-from-WIRE: the escrowed WIRE was never credited to a reserve —
      // refund it directly (there is no source outpost to route a revert to).
      action(
         permission_level{self, "active"_n},
         uwrit::RESERVE_ACCOUNT, "refundwire"_n,
         std::make_tuple(*user, req.src_amount)
      ).send();
   } else {
      sysio::print("reject_and_refund: cannot parse from-WIRE depositor for "
                   "refund on uwreq ", req.id, "\n");
   }
   reqs.modify(same_payer, pk, [&](auto& r) {
      r.status           = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_REJECTED;
      r.settled_at_ms    = current_time_ms();
      r.expires_at_epoch = get_current_epoch() + uwrit::UWREQ_RETENTION_EPOCHS;
      for (auto& c : r.commits_by) {
         if (c.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED) {
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
            c.reason = commit_reason;
         }
      }
   });
}

/// Resolve the race once every REQUIRED leg of a (uwreq, underwriter) pair
/// has arrived. A leg is required iff its chain is an outpost; a depot
/// (WIRE) leg needs no UIC, no bond, and no lock — single-leg swaps
/// (to/from WIRE) therefore resolve on their one outpost commit. On a win:
/// verify the required legs' signatures + bond, pre-validate reserve
/// liquidity against the local mirror AND pre-build the outbound SWAP_REMIT
/// envelope (so both the inline reserv settlement actions and the remit are
/// unreachable-failure by construction — nothing past the CONFIRMED write can
/// `check()`-abort and stall evalcons), push one lock per required leg (a 12h
/// wall-clock challenge window — released only by `chklocks`, never by
/// delivery), mark CONFIRMED, then settle:
///   * normal     — reserv::applyswap  + SWAP_REMIT to the dst outpost
///   * from-WIRE  — reserv::applyfromwire + SWAP_REMIT to the dst outpost
///   * to-WIRE    — reserv::paywire (REAL WIRE to the recipient; no remit)
/// Disqualified candidates get their commit_entry marked
/// (status=DISQUALIFIED); transient liquidity shortfalls log + skip, leaving
/// the uwreq PENDING.
void try_select_winner(name self, uint64_t uwreq_id, name candidate) {
   uwrit::uwreqs_t reqs(self);
   auto pk = uwrit::id_key{uwreq_id};
   if (!reqs.contains(pk)) return;
   auto req = reqs.get(pk);
   if (req.status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING) return;

   const bool src_needed = !leg_is_depot(req.src_chain_code);
   const bool dst_needed = !leg_is_depot(req.dst_chain_code);

   // ── Underwriter eligibility — role + activation gate ─────────────────
   // Mirrored bond availability and UIC signature recovery alone do NOT
   // establish that `candidate` is an authorized underwriter. opreg registers
   // every non-bootstrapped operator as UNKNOWN, forbids bootstrapping an
   // underwriter active, and only flips an underwriter to ACTIVE through
   // `opreg::processuw` once it clears `req_uw_collat`. The balance mirror
   // (`available_via_mirrors`) zeroes only SLASHED / TERMINATED and ignores
   // `op.type`, so without this gate a non-underwriter (PRODUCER / BATCH /
   // CHALLENGER) or a not-yet-active underwriter (UNKNOWN / WARMUP / COOLDOWN)
   // carrying enough mirrored balance and a valid UIC signature would be
   // selected as the winner, consume lock capacity, and settle against the
   // reserves. Require an ACTIVE UNDERWRITER before any signature work, lock,
   // CONFIRMED write, or reserve mutation. Disqualify (reclaimable — a later
   // rcrdcommit re-arms the entry once the operator activates) rather than
   // throw: this resolver runs inside the evalcons dispatch chain, and a
   // check() here would stall OPP consensus (`feedback_opp_handlers_never_throw`).
   if (!is_active_underwriter(candidate)) {
      disqualify_candidate(reqs, pk, candidate,
                           "candidate is not an ACTIVE underwriter on opreg "
                           "(role/activation eligibility not satisfied)");
      return;
   }

   // ── T7: signature verification — required (outpost) legs only ────────
   // Look up the candidate's commit_entry to get the per-leg UIC bytes.
   const uwrit::commit_entry* ce_ptr = nullptr;
   for (const auto& c : req.commits_by) {
      if (c.underwriter == candidate) { ce_ptr = &c; break; }
   }
   if (!ce_ptr) return;
   if ((src_needed && !verify_uic_signature(candidate, ce_ptr->source_uic_bytes)) ||
       (dst_needed && !verify_uic_signature(candidate, ce_ptr->dest_uic_bytes))) {
      // A candidate whose UIC signature does not recover to its active/owner
      // key can never win — disqualify it (non-throwing) so the race state
      // converges, instead of leaving a stale INTENT_SUBMITTED that keeps the
      // uwreq pending/noisy until another underwriter wins. The race stays
      // open for the remaining valid commits, and the disqualified candidate
      // may re-arm via a later rcrdcommit. Per
      // `feedback_opp_handlers_never_throw.md` a check() here would halt
      // evalcons and stall consensus, so this path stays non-throwing.
      disqualify_candidate(reqs, pk, candidate,
                           "invalid underwrite-intent-commit signature on one "
                           "or both required legs");
      return;
   }

   // ── Bond availability — required legs, aggregated per collateral bucket ─
   // Underwriter collateral is held and rolled up by (underwriter,
   // chain_code, token_code) — NOT by reserve_code (see `available_via_mirrors`
   // / `opreg::available`). A same-(chain, token) swap between two reserves
   // (distinct `reserve_code`, a shape `rcrdcommit` explicitly routes) draws
   // BOTH required legs against the SAME balance bucket. Checking each leg
   // independently would let an underwriter whose balance covers each single
   // leg but not their sum win and write two locks that overcommit the one
   // balance: balance 150 wins `src_amount=100` + `dst_amount=100` because
   // 150>=100 holds per leg, yet the aggregate 200 is uncovered, and
   // the deferred slash/remit cleanup (`opreg::releaselock` inside `chklocks`)
   // then has to draw 200 from a 150 balance. Require availability to cover
   // the AGGREGATE required amount of every leg that shares a collateral
   // bucket. The sum is computed in uint128: `src + dst` can exceed uint64,
   // and a balance is itself uint64, so an overflowing aggregate must read as
   // genuinely insufficient — NOT saturate to UINT64_MAX (which a UINT64_MAX
   // available balance would spuriously satisfy) and NOT wrap to a small
   // passing value. This resolver is non-throwing
   // (`feedback_opp_handlers_never_throw`), so it disqualifies the candidate
   // rather than asserting on an over-large request.
   const bool same_bucket = src_needed && dst_needed
                            && req.src_chain_code == req.dst_chain_code
                            && req.src_token_code == req.dst_token_code;
   bool insufficient_bond = false;
   if (same_bucket) {
      // One opreg balance funds both legs — it must cover their sum. uint128
      // keeps the comparison honest when src+dst exceeds uint64 (no uint64
      // balance can cover it, so availability is insufficient by construction).
      const uint64_t  avail = available_via_mirrors(self, candidate,
                                                    req.src_chain_code, req.src_token_code);
      const uint128_t need  = static_cast<uint128_t>(req.src_amount) + req.dst_amount;
      insufficient_bond = static_cast<uint128_t>(avail) < need;
   } else {
      // Distinct buckets (or a single required leg) — check each independently.
      const uint64_t src_avail = src_needed
         ? available_via_mirrors(self, candidate, req.src_chain_code, req.src_token_code) : 0;
      const uint64_t dst_avail = dst_needed
         ? available_via_mirrors(self, candidate, req.dst_chain_code, req.dst_token_code) : 0;
      insufficient_bond = (src_needed && src_avail < req.src_amount)
                       || (dst_needed && dst_avail < req.dst_amount);
   }
   if (insufficient_bond) {
      // Insufficient bond to cover the aggregate required collateral —
      // disqualify this candidate, leave the race open for another.
      disqualify_candidate(reqs, pk, candidate,
                           "insufficient bond to cover the aggregate required "
                           "collateral on one or both legs");
      return;
   }

   // ── T6: race-time variance recheck ────────────────────────────────────
   // The createuwreq path validated the LP quote at ingestion. Between
   // then and now the LP may have drifted; if the drift now exceeds the
   // user's tolerance, emit SWAP_REVERT instead of locking. Skipped when
   // the local mirror returns 0 (no LP provisioned) — same convention as
   // createuwreq.
   {
      const uint64_t current_quote = swap_quote(
         req.src_chain_code, req.src_token_code, req.src_reserve_code,
         req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
         req.src_amount, current_fee_bps(self));
      const uint64_t quoted = req.dst_amount;
      if (quoted != 0) {
         if (current_quote == 0) {
            // Zero quote with every required reserve ACTIVE = unpriceable /
            // degenerate reserve (drained side, extreme weights, leg too small
            // to price). Fail closed so a stale target cannot settle against a
            // reserve the AMM can no longer price (WSA-041). Skip only when a
            // required reserve is genuinely unprovisioned (dev & smoke clusters).
            if (required_reserves_active(req.src_chain_code, req.src_token_code, req.src_reserve_code,
                                         req.dst_chain_code, req.dst_token_code, req.dst_reserve_code)) {
               reject_and_refund(self, reqs, pk, req, src_needed,
                  "unpriceable reserve: zero quote from an ACTIVE reserve at race resolution",
                  "uwreq reverted at race resolution (unpriceable reserve)");
               return;
            }
         } else {
            const uint64_t diff = current_quote > quoted
                                     ? current_quote - quoted
                                     : quoted - current_quote;
            const uint128_t allowed = (static_cast<uint128_t>(quoted)
                                          * req.variance_tolerance_bps) / 10000u;
            if (static_cast<uint128_t>(diff) > allowed) {
               // Drift now exceeds the user's tolerance — refund the source
               // side and REJECT rather than lock a stale quote. Non-throwing.
               reject_and_refund(self, reqs, pk, req, src_needed,
                  "variance exceeded tolerance at race resolution: "
                  "quoted=" + std::to_string(quoted)
                  + " current=" + std::to_string(current_quote)
                  + " tolerance_bps=" + std::to_string(req.variance_tolerance_bps),
                  "uwreq reverted at race resolution (variance drift)");
               return;
            }
         }
      }
   }

   // ── Settlement pre-checks against the local reserve mirror ───────────
   // The reserv-side settlement actions (`applyswap` / `applyfromwire` /
   // `paywire`) `check()`-abort on violation; an abort inside this
   // dispatch chain stalls consensus, so every condition is pre-validated
   // here — making the inline actions unreachable-failure by construction
   // (no other reserve mutation can interleave within this transaction).
   // Transient shortfalls (another swap drained the reserve between quote
   // and win) log + skip, leaving the uwreq PENDING for later resolution.
   name towire_recipient{};
   if (!dst_needed) {
      // Swap-to-WIRE. Terminal first: a malformed recipient can never
      // become valid — revert to the source outpost + REJECT.
      opp::attestations::SwapRequest sr0;
      {
         auto in = zpp::bits::in{
            std::span{req.attestation_inbound_data.data(),
                       req.attestation_inbound_data.size()},
            zpp::bits::no_size{}};
         if (in(sr0) != zpp::bits::errc{}) {
            sysio::print("try_select_winner: cannot decode stored SwapRequest "
                         "for uwreq ", uwreq_id, "\n");
            return;
         }
      }
      std::vector<char> recipient_bytes{sr0.recipient.address.begin(),
                                         sr0.recipient.address.end()};
      auto rcpt = parse_wire_name(recipient_bytes);
      if (!rcpt || !is_account(*rcpt)) {
         // A malformed recipient can never become valid (terminal). to-WIRE
         // always has an outpost source leg, so this routes a SWAP_REVERT
         // back to it.
         reject_and_refund(self, reqs, pk, req, src_needed,
            "swap-to-WIRE rejected: recipient is not a valid WIRE account",
            "uwreq rejected: invalid WIRE recipient");
         return;
      }
      towire_recipient = *rcpt;
      // Terminal: the to-WIRE payout is sent as `asset(dst_amount, WIRE)` by
      // reserv::paywire. `dst_amount` is the UNBOUNDED cross-chain
      // `SwapRequest.target_amount`; a target exceeding asset::max_amount
      // (2^62-1) can never be represented as a WIRE asset — for ANY winner or
      // liquidity. Left unchecked it would (a) wrap the `dst_amount +
      // to_wire_fee` sufficiency guard below for a target near UINT64_MAX,
      // letting the swap slip into settlement, then (b) abort paywire's
      // asset() constructor mid-evalcons and stall consensus. Reject + refund
      // the source side here, BEFORE any CONFIRMED / lock / reserve write.
      if (req.dst_amount > static_cast<uint64_t>(asset::max_amount)) {
         reject_and_refund(self, reqs, pk, req, src_needed,
            "swap-to-WIRE rejected: target amount exceeds the maximum "
            "representable WIRE asset",
            "uwreq rejected: to-WIRE target exceeds asset max_amount");
         return;
      }
      auto src_r = find_active_reserve(req.src_chain_code, req.src_token_code, req.src_reserve_code);
      // paywire gives up `dst_amount` (to the recipient) + the fee (on the gross
      // WIRE leg) out of the source reserve's WIRE; pre-validate that exact sum.
      // `dst_amount` is bounded <= asset::max_amount above and `to_wire_fee`
      // derives from the (asset-bounded) reserve WIRE, so the sum cannot wrap
      // uint64 — but add saturating so the guard is provably overflow-free at
      // the settlement boundary regardless of inputs.
      const uint64_t w_gross = src_r
         ? opp::amm::token_to_wire(src_r->reserve_chain_amount, src_r->reserve_wire_amount,
                                   src_r->connector_weight_bps, req.src_amount)
         : 0;
      const uint64_t to_wire_fee = opp::amm::split_wire_fee(
         w_gross, current_fee_bps(self), reserve::FEE_REWARD_SHARE_BPS).fee;
      const uint64_t wire_needed = opp::safe::add_sat_u64(req.dst_amount, to_wire_fee);
      if (!src_r || w_gross == 0 || src_r->reserve_wire_amount < wire_needed) {
         sysio::print("try_select_winner: insufficient source-reserve WIRE for "
                      "to-WIRE payout on uwreq ", uwreq_id, ", skipping\n");
         return;
      }
   } else if (!src_needed) {
      // Swap-from-WIRE. drainfwq validated at queue-drain; re-validate the
      // live state (privacy is immutable, but liquidity can drift).
      auto dst_r = find_active_reserve(req.dst_chain_code, req.dst_token_code, req.dst_reserve_code);
      if (!dst_r || dst_r->is_private || dst_r->reserve_chain_amount < req.dst_amount) {
         sysio::print("try_select_winner: destination reserve unavailable for "
                      "from-WIRE swap on uwreq ", uwreq_id, ", skipping\n");
         return;
      }
   } else {
      // Normal outpost↔outpost swap — both rows must cover the four-leg
      // apply (the WIRE intermediate is derived from the same pre-mutation
      // source row `applyswap` will read).
      auto src_r = find_active_reserve(req.src_chain_code, req.src_token_code, req.src_reserve_code);
      auto dst_r = find_active_reserve(req.dst_chain_code, req.dst_token_code, req.dst_reserve_code);
      // applyswap debits the gross weighted WIRE intermediate from the source
      // (the fee is taken from it before the net reaches dst); pre-validate the
      // same conditions applyswap will check.
      const uint64_t w_gross = src_r
         ? opp::amm::token_to_wire(src_r->reserve_chain_amount, src_r->reserve_wire_amount,
                                   src_r->connector_weight_bps, req.src_amount)
         : 0;
      if (!src_r || !dst_r || w_gross == 0 ||
          src_r->reserve_wire_amount < w_gross ||
          dst_r->reserve_chain_amount < req.dst_amount) {
         sysio::print("try_select_winner: insufficient reserve liquidity for "
                      "uwreq ", uwreq_id, ", skipping\n");
         return;
      }
   }

   // ── Pre-build the outbound SWAP_REMIT (dst-outpost paths only) ───────
   // The remit's identity resolution — stored-request decode, destination
   // outpost / chain-kind, and the winner's destination authex link — used to
   // run inside the former `emit_swap_remit` AFTER the
   // reserve mutation, with `check()` aborts that would halt evalcons and
   // stall OPP consensus chain-wide. Build + validate the envelope HERE,
   // before any lock / CONFIRMED / applyswap write, so a failure unwinds the
   // race non-throwing. `queue_swap_remit` (in the settlement tail) only
   // ships the pre-built bytes, after the reserve books have moved.
   uint64_t remit_dst_outpost_id = 0;
   std::vector<char> remit_encoded;
   if (dst_needed) {
      switch (try_build_swap_remit(req, candidate, remit_dst_outpost_id, remit_encoded)) {
         case swap_remit_disp::ok:
            break;
         case swap_remit_disp::disqualified:
            // This winner has no destination-chain authex link / usable
            // pubkey — disqualify and let the race resolve for another.
            disqualify_candidate(reqs, pk, candidate,
               "winning candidate has no authex link for the destination "
               "chain — cannot emit an auditable SwapRemit");
            return;
         case swap_remit_disp::terminal:
            // No underwriter can ever remit this uwreq (stored request
            // undecodable, or destination outpost / chain-kind unresolved) —
            // refund the source side and REJECT.
            reject_and_refund(self, reqs, pk, req, src_needed,
               "swap unremittable at race resolution: destination outpost / "
               "chain-kind unresolved or stored request undecodable",
               "uwreq rejected: destination unremittable at race resolution");
            return;
      }
   }

   // Winner — push one lock per REQUIRED leg + mark uwreq CONFIRMED.
   // Each lock_entry carries the matching leg's full slug_name triple
   // (`chain_code, token_code, reserve_code`) so a future slash routes
   // unambiguously back to the originating reserve. Locks are a
   // wall-clock challenge window (12h default): they are NEVER released
   // by delivery — only `chklocks` (epoch advance) sweeps them after
   // `expires_at_ms`.
   const uint64_t now_ms_v = current_time_ms();
   uwrit::uwconfig_t uwcfg_tbl(self);
   auto uwcfg = uwcfg_tbl.get_or_default(uwrit::uw_config{});
   const uint64_t expires_ms = now_ms_v + uwcfg.collateral_lock_duration_ms;

   uwrit::locks_t locks(self);

   if (src_needed) {
      uint64_t src_lock_id = next_lock_id(self);
      locks.emplace(ram_payer, uwrit::lock_key{src_lock_id}, uwrit::lock_entry{
         .lock_id       = src_lock_id,
         .uwreq_id      = uwreq_id,
         .underwriter   = candidate,
         .chain_code    = req.src_chain_code,
         .token_code    = req.src_token_code,
         .reserve_code  = req.src_reserve_code,
         .amount        = req.src_amount,
         .created_at_ms = now_ms_v,
         .expires_at_ms = expires_ms,
      });
   }

   if (dst_needed) {
      uint64_t dst_lock_id = next_lock_id(self);
      locks.emplace(ram_payer, uwrit::lock_key{dst_lock_id}, uwrit::lock_entry{
         .lock_id       = dst_lock_id,
         .uwreq_id      = uwreq_id,
         .underwriter   = candidate,
         .chain_code    = req.dst_chain_code,
         .token_code    = req.dst_token_code,
         .reserve_code  = req.dst_reserve_code,
         .amount        = req.dst_amount,
         .created_at_ms = now_ms_v,
         .expires_at_ms = expires_ms,
      });
   }

   reqs.modify(same_payer, pk, [&](auto& r) {
      r.status          = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED;
      r.winner          = candidate;
      r.committed_at_ms = current_time_ms();
      // Mark the winner's commit_entry CONFIRMED, others RELEASED (loser).
      for (auto& c : r.commits_by) {
         if (c.underwriter == candidate) {
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_CONFIRMED;
         } else if (c.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED) {
            // Eligible loser — promote to RELEASED for retention/debugging.
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
            c.reason = "lost the COMMIT race";
         }
      }
   });

   // ── Settlement — reserve books move NOW, before any remit leaves ─────
   // All reserve mutations are queued ahead of the SWAP_REMIT queueout in
   // this same transaction, so every quote between emit and delivery
   // prices the post-swap books. There is no failure path past this
   // point: everything was verified above, and transport is
   // consensus-gated (delivery either reaches consensus or nothing
   // happens — the envelope dispute process covers divergence). Success
   // is implicit; locks expire on their own 12h window via `chklocks`.
   if (!dst_needed) {
      // Swap-to-WIRE: pay the recipient REAL WIRE from reserve custody.
      // The depot itself is the payer — no outbound remit, no ack.
      action(
         permission_level{self, "active"_n},
         uwrit::RESERVE_ACCOUNT, "paywire"_n,
         std::make_tuple(req.src_chain_code, req.src_token_code, req.src_reserve_code,
                          req.src_amount, towire_recipient, req.dst_amount)
      ).send();
   } else if (!src_needed) {
      // Swap-from-WIRE: the escrowed WIRE becomes dst-reserve liquidity,
      // then the normal remit tail to the destination outpost.
      action(
         permission_level{self, "active"_n},
         uwrit::RESERVE_ACCOUNT, "applyfromwire"_n,
         std::make_tuple(req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
                          req.src_amount, req.dst_amount)
      ).send();
      queue_swap_remit(self, remit_dst_outpost_id, remit_encoded);
   } else {
      // Normal swap: emit-time four-leg apply, then the remit tail.
      action(
         permission_level{self, "active"_n},
         uwrit::RESERVE_ACCOUNT, "applyswap"_n,
         std::make_tuple(req.src_chain_code, req.src_token_code, req.src_reserve_code,
                          req.src_amount,
                          req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
                          req.dst_amount)
      ).send();
      queue_swap_remit(self, remit_dst_outpost_id, remit_encoded);
   }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  rcrdcommit — record a per-leg COMMIT arrival
// ---------------------------------------------------------------------------
//
// The leg-classification logic now uses the slug_name triple
// `(from_chain_code, from_token_code, reserve_code)` to disambiguate src
// vs dst. The triple is required because two reserves of the same token
// can coexist on the same chain — same-(chain, token) swaps fall back to
// reserve_code as the tiebreaker.
void uwrit::rcrdcommit(uint64_t uwreq_id,
                       name underwriter,
                       uint64_t chain_code,
                       sysio::slug_name from_chain_code,
                       sysio::slug_name from_token_code,
                       sysio::slug_name reserve_code,
                       std::vector<char> uic_bytes) {
   require_auth(MSGCH_ACCOUNT);

   uwreqs_t reqs(get_self());
   auto pk = id_key{uwreq_id};
   // Dispatched-from-msgch handlers MUST NOT throw — a check() halts
   // evalcons (`feedback_opp_handlers_never_throw.md`). Silently no-op
   // on unknown uwreq_id or wrong status.
   if (!reqs.contains(pk)) {
      sysio::print("rcrdcommit: unknown uwreq ", uwreq_id, ", skipping\n");
      return;
   }
   auto req_snapshot = reqs.get(pk);
   if (req_snapshot.status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id,
                   " not in PENDING (status=",
                   magic_enum::enum_integer(req_snapshot.status), "), skipping\n");
      return;
   }

   // Classify the UIC's leg BEFORE touching commits_by. `src_*`/`dst_*` are set once at
   // createuwreq and never change here, so the snapshot's values are authoritative. Route by the
   // full `(chain_code, token_code, reserve_code)` triple so same-chain swaps with multiple
   // reserves on the same (chain, token) pair land in the correct per-leg slot.
   const bool is_source = (from_chain_code   == req_snapshot.src_chain_code
                           && from_token_code == req_snapshot.src_token_code
                           && reserve_code    == req_snapshot.src_reserve_code);
   const bool is_dest   = (from_chain_code   == req_snapshot.dst_chain_code
                           && from_token_code == req_snapshot.dst_token_code
                           && reserve_code    == req_snapshot.dst_reserve_code);
   // A UIC whose leg matches neither the source nor the destination is not bound to this request.
   // Drop it with no mutation so a stream of unmatched commits cannot grow commits_by or re-arm a
   // disqualified entry. Fail closed, never check().
   if (!is_source && !is_dest) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id,
                   " UIC leg matches neither source nor destination, skipping\n");
      return;
   }

   // Only a registered ACTIVE underwriter can win this race — try_select_winner enforces the same
   // gate, and the DISQUALIFIED re-arm below already requires a fresh rcrdcommit, so an entry for
   // any other account is pure dead weight. Refuse to create one: otherwise a matched-leg UIC could
   // append one commit_entry per attacker-chosen valid-but-unregistered account name and bloat the
   // row. Fail closed, no mutation, never check().
   if (!is_active_underwriter(underwriter)) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " underwriter ", underwriter,
                   " is not an ACTIVE underwriter, skipping\n");
      return;
   }

   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, underwriter);
      uint64_t now_ms = current_time_ms();
      if (is_source) {
         c->source_received_at_ms = now_ms;
         c->source_outpost_id     = chain_code;
         c->source_uic_bytes      = uic_bytes;
      } else if (is_dest) {
         c->dest_received_at_ms = now_ms;
         c->dest_outpost_id     = chain_code;
         c->dest_uic_bytes      = uic_bytes;
      }
      // Re-arm a previously-disqualified entry to INTENT_SUBMITTED if the
      // underwriter re-commits (e.g. they topped up bond, or added the missing
      // destination authex link, and want back in the race). The next
      // try_select_winner call re-evaluates. An economic SLASH is recorded on
      // the lock, never on a commit_entry, so DISQUALIFIED is the only
      // reclaimable terminal state here.
      if (c->status == UnderwriteStatus::UNDERWRITE_STATUS_DISQUALIFIED) {
         c->status = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED;
         c->reason.clear();
      }
   });

   // Re-read after modify — try_select_winner needs the latest commit_entry.
   // A leg is required iff its chain is an outpost; a depot (WIRE) leg is
   // satisfied by construction (no outpost exists to send a UIC for it),
   // so single-leg swaps arm the race on their one real commit.
   auto refreshed = reqs.get(pk);
   const bool src_needed = !leg_is_depot(refreshed.src_chain_code);
   const bool dst_needed = !leg_is_depot(refreshed.dst_chain_code);
   for (const auto& c : refreshed.commits_by) {
      if (c.underwriter != underwriter) continue;
      const bool src_ok = !src_needed || c.source_received_at_ms != 0;
      const bool dst_ok = !dst_needed || c.dest_received_at_ms != 0;
      if (src_ok && dst_ok) {
         try_select_winner(get_self(), uwreq_id, underwriter);
      }
      break;
   }
}

// ---------------------------------------------------------------------------
//  swapfromwire — queue a depot-originated swap (WIRE is the source)
// ---------------------------------------------------------------------------
//
// User transaction (require_auth(user)) — reverting on validation failure
// is the correct mode here, exactly like opreg::deposit. The WIRE escrow
// and the queue row land atomically; the uwreq itself is NOT created in
// this transaction (that would short-circuit ledger consistency) — the
// next sysio.epoch::advance drains the queue via drainfwq.
void uwrit::swapfromwire(name                  user,
                          uint64_t              wire_amount,
                          sysio::slug_name       dst_chain_code,
                          sysio::slug_name       dst_token_code,
                          sysio::slug_name       dst_reserve_code,
                          uint64_t              target_amount,
                          uint32_t              target_tolerance_bps,
                          opp::types::ChainKind recipient_kind,
                          std::vector<char>     recipient_addr) {
   require_auth(user);
   check(wire_amount > 0, "swapfromwire: wire_amount must be positive");
   check(target_amount > 0, "swapfromwire: target_amount must be positive");
   check(!recipient_addr.empty() && recipient_addr.size() <= 64,
         "swapfromwire: recipient address must be 1..64 bytes");
   check(chain_registered_active(dst_chain_code),
         "swapfromwire: target chain not registered or not active");
   check(!leg_is_depot(dst_chain_code),
         "swapfromwire: target must be an outpost chain (WIRE->WIRE is just "
         "a token transfer)");
   {
      auto kind_opt = chain_kind_for_code(dst_chain_code);
      check(kind_opt.has_value() && *kind_opt == recipient_kind,
            "swapfromwire: recipient_kind does not match the target chain's kind");
   }

   // Advisory reserve checks for UX — drainfwq re-validates authoritatively
   // at the epoch boundary (state may change in between; drain refunds).
   {
      auto r = find_reserve(dst_chain_code, dst_token_code, dst_reserve_code);
      check(r.has_value(), "swapfromwire: target reserve not found");
      check(r->status == ReserveStatus::RESERVE_STATUS_ACTIVE,
            "swapfromwire: target reserve not ACTIVE");
      check(!r->is_private,
            "swapfromwire: private reserves are excluded from WIRE-endpoint swaps");
   }

   // Escrow REAL WIRE into reserve custody NOW. The queue row records the
   // claim; until drainfwq either creates the uwreq (escrow becomes
   // reserve liquidity at race win) or refunds, this sits as the
   // in-flight-escrow term of the custody invariant.
   action(
      permission_level{user, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(user, RESERVE_ACCOUNT,
         asset(static_cast<int64_t>(wire_amount), WIRE_SYMBOL),
         std::string("sysio.uwrit::swapfromwire escrow"))
   ).send();

   uint64_t qid = next_fromwire_id(get_self());
   fwqueue_t q(get_self());
   q.emplace(ram_payer, fw_key{qid}, fromwire_q{
      .id                     = qid,
      .user                   = user,
      .wire_amount            = wire_amount,
      .dst_chain_code         = dst_chain_code,
      .dst_token_code         = dst_token_code,
      .dst_reserve_code       = dst_reserve_code,
      .target_amount          = target_amount,
      .variance_tolerance_bps = target_tolerance_bps,
      .recipient_kind         = recipient_kind,
      .recipient_addr         = std::move(recipient_addr),
      .created_at_epoch       = get_current_epoch(),
   });
}

// ---------------------------------------------------------------------------
//  drainfwq — epoch-boundary drain of the swap-from-WIRE queue
// ---------------------------------------------------------------------------
//
// Inlined from sysio.epoch::advance. NEVER throws: every reachable
// `check()` in the reserv actions it inlines (`refundwire`) is
// pre-guaranteed (wire_amount > 0 enforced at swapfromwire; the user
// account existed at escrow time and accounts are not deletable).
// Validation failures refund + drop; only validated rows become PENDING
// uwreqs.
void uwrit::drainfwq() {
   check(has_auth(EPOCH_ACCOUNT) || has_auth(get_self()),
         "drainfwq requires sysio.epoch or sysio.uwrit authority");

   fwqueue_t q(get_self());
   std::vector<fromwire_q> rows;
   for (auto it = q.begin(); it != q.end(); ++it) {
      rows.push_back(*it);
   }
   if (rows.empty()) return;

   uwreqs_t reqs(get_self());
   const auto depot_code = depot_chain_code();

   for (const auto& row : rows) {
      auto refund_and_drop = [&](const char* why) {
         sysio::print("drainfwq: ", why, " — refunding queued swap ", row.id, "\n");
         action(
            permission_level{get_self(), "active"_n},
            RESERVE_ACCOUNT, "refundwire"_n,
            std::make_tuple(row.user, row.wire_amount)
         ).send();
         q.erase(fw_key{row.id});
      };

      if (!depot_code) {
         refund_and_drop("depot chain not registered");
         continue;
      }
      auto r = find_reserve(row.dst_chain_code, row.dst_token_code, row.dst_reserve_code);
      if (!r || r->status != ReserveStatus::RESERVE_STATUS_ACTIVE) {
         refund_and_drop("target reserve missing or not ACTIVE");
         continue;
      }
      if (r->is_private) {
         refund_and_drop("target reserve is private (excluded from WIRE-endpoint swaps)");
         continue;
      }
      // Authoritative variance check. The target reserve is confirmed ACTIVE
      // above, so a zero quote here is NOT an unprovisioned-LP skip — it is a
      // degenerate / unpriceable ACTIVE reserve (token side drained to zero,
      // extreme connector weights, or a WIRE escrow too small for the
      // weighted-Bancor kernel to price). Fail closed and refund: otherwise a
      // caller-chosen target_amount bypasses variance and drains the reserve's
      // token side for a negligible WIRE escrow (WSA-041).
      const uint64_t quote = swap_quote(*depot_code, WIRE_TOKEN, WIRE_TOKEN,
                                         row.dst_chain_code, row.dst_token_code,
                                         row.dst_reserve_code, row.wire_amount,
                                         current_fee_bps(get_self()));
      if (quote == 0) {
         refund_and_drop("unpriceable target reserve: zero quote from an ACTIVE reserve");
         continue;
      }
      {
         // `target_amount > 0` is guaranteed by swapfromwire.
         const uint64_t diff = quote > row.target_amount
                                  ? quote - row.target_amount
                                  : row.target_amount - quote;
         const uint128_t allowed = (static_cast<uint128_t>(row.target_amount)
                                       * row.variance_tolerance_bps) / 10000u;
         if (static_cast<uint128_t>(diff) > allowed) {
            refund_and_drop("variance exceeded tolerance at drain");
            continue;
         }
      }

      // Synthetic SwapRequest payload — the settlement tail
      // (`try_build_swap_remit`) decodes `recipient` from the stored bytes
      // exactly as it does for outpost-originated swaps.
      opp::attestations::SwapRequest sr;
      sr.actor.kind = ChainKind::CHAIN_KIND_WIRE;
      {
         auto user_bytes = wire_name_bytes(row.user);
         sr.actor.address.assign(user_bytes.begin(), user_bytes.end());
      }
      sr.source_amount.token_code = WIRE_TOKEN.value;
      sr.source_amount.amount     = static_cast<int64_t>(row.wire_amount);
      sr.source_chain_code        = depot_code->value;
      sr.source_reserve_code      = WIRE_TOKEN.value;   // sentinel — no WIRE-side reserve
      sr.target_chain_code        = row.dst_chain_code.value;
      sr.target_token_code        = row.dst_token_code.value;
      sr.target_reserve_code      = row.dst_reserve_code.value;
      sr.recipient.kind           = row.recipient_kind;
      sr.recipient.address.assign(row.recipient_addr.begin(), row.recipient_addr.end());
      sr.target_amount            = row.target_amount;
      sr.target_tolerance_bps     = row.variance_tolerance_bps;
      sr.target_timestamp_ms      = 0;
      // Synthetic but non-empty source_tx_id (8-byte LE row id) so the
      // uwreq row shape matches outpost-originated swaps.
      std::vector<char> stx(8);
      for (size_t i = 0; i < 8; ++i) {
         stx[i] = static_cast<char>((row.id >> (i * 8)) & 0xff);
      }
      sr.source_tx_id.assign(stx.begin(), stx.end());

      std::vector<char> encoded;
      auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
      if (out(sr) != zpp::bits::errc{}) {
         refund_and_drop("failed to encode synthetic SwapRequest");
         continue;
      }

      auto pk = id_key{row.id};
      if (reqs.contains(pk)) {
         // Idempotency backstop — the id space makes this unreachable.
         q.erase(fw_key{row.id});
         continue;
      }
      reqs.emplace(ram_payer, pk, uw_request_t{
         .id                        = row.id,
         .type                      = AttestationType::ATTESTATION_TYPE_SWAP_REQUEST,
         .status                    = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING,
         .src_chain_code            = *depot_code,
         .src_token_code            = WIRE_TOKEN,
         .src_reserve_code          = WIRE_TOKEN,
         .src_amount                = row.wire_amount,
         .dst_chain_code            = row.dst_chain_code,
         .dst_token_code            = row.dst_token_code,
         .dst_reserve_code          = row.dst_reserve_code,
         .dst_amount                = row.target_amount,
         .variance_tolerance_bps    = row.variance_tolerance_bps,
         .source_tx_id              = std::move(stx),
         .depositor                 = wire_name_bytes(row.user),
         .commits_by                = {},
         .winner                    = name{},
         .committed_at_ms           = 0,
         .settled_at_ms             = 0,
         .expires_at_epoch          = 0,
         .attestation_inbound_data  = std::move(encoded),
         .attestation_outbound_data = {},
      });
      q.erase(fw_key{row.id});
   }
}

// ---------------------------------------------------------------------------
//  sumlocks — read-only helper
// ---------------------------------------------------------------------------
uint64_t uwrit::sumlocks(name underwriter,
                         sysio::slug_name chain_code,
                         sysio::slug_name token_code) {
   return sum_locks_inline(get_self(), underwriter, chain_code, token_code);
}

// ---------------------------------------------------------------------------
//  chklocks — epoch-boundary sweep of expired locks (the ONLY release path)
// ---------------------------------------------------------------------------
//
// Locks are a wall-clock challenge window (12h default via uwconfig) and
// are never released by delivery. This sweep:
//   1. erases every lock whose `expires_at_ms` has elapsed,
//   2. inlines `opreg::releaselock` per lock — the deferred-slash
//      (SLASHED) / deferred-remit (TERMINATED) / no-op (healthy) hop that
//      settles the underwriter's bond as the challenge window closes,
//   3. flips a CONFIRMED uwreq to COMPLETED once its last lock is swept
//      (delivery success is implicit — there is no SWAP_REMIT ack).
//
// Runs as one of the FIRST steps of sysio.epoch::advance so freshly freed
// collateral is visible to the same advance's withdraw flushing. NEVER
// throws on reachable state: lock amounts are provably positive (enforced
// at uwreq/queue creation), so opreg::releaselock's amount>0 check cannot
// fire; and releaselock clamps its balance subtraction to the live bucket,
// so even a residual over-committed lock set cannot underflow + abort the
// advance (defence-in-depth — the winner-selection aggregate bond check
// already prevents the over-commit at lock-creation time).
void uwrit::chklocks() {
   // Two valid callers:
   //   * sysio.epoch::advance — inlined at every epoch boundary.
   //   * sysio.uwrit — manual cleanup invocation, e.g. from a migration.
   check(has_auth(EPOCH_ACCOUNT) || has_auth(get_self()),
         "chklocks requires sysio.epoch or sysio.uwrit authority");

   const uint64_t now_ms = current_time_ms();
   locks_t locks(get_self());
   auto idx = locks.get_index<"byexpire"_n>();

   // Walk in ascending `expires_at_ms` and collect full copies while
   // expired — we erase in a second pass (an erase invalidates the index
   // cursor) and need every field for the releaselock fan-out.
   std::vector<lock_entry> expired;
   for (auto it = idx.begin();
        it != idx.end() && it->expires_at_ms <= now_ms; ++it) {
      expired.push_back(*it);
   }
   if (expired.empty()) return;

   std::vector<uint64_t> affected;
   for (const auto& l : expired) {
      action(
         permission_level{get_self(), "active"_n},
         OPREG_ACCOUNT, "releaselock"_n,
         std::make_tuple(l.underwriter, l.chain_code, l.token_code, l.amount)
      ).send();
      locks.erase(lock_key{l.lock_id});
      if (std::find(affected.begin(), affected.end(), l.uwreq_id) == affected.end()) {
         affected.push_back(l.uwreq_id);
      }
   }

   // COMPLETED flip — a CONFIRMED uwreq whose final lock just swept has
   // exited its challenge window.
   uwreqs_t reqs(get_self());
   auto byuwreq = locks.get_index<"byuwreq"_n>();
   const uint32_t now_ep = get_current_epoch();
   for (uint64_t id : affected) {
      auto lit = byuwreq.lower_bound(id);
      if (lit != byuwreq.end() && lit->uwreq_id == id) continue;   // locks remain
      auto pk = id_key{id};
      if (!reqs.contains(pk)) continue;
      auto r = reqs.get(pk);
      if (r.status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED) continue;
      reqs.modify(same_payer, pk, [&](auto& row) {
         row.status           = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_COMPLETED;
         row.settled_at_ms    = current_time_ms();
         row.expires_at_epoch = now_ep + UWREQ_RETENTION_EPOCHS;
      });
   }
}

} // namespace sysio
