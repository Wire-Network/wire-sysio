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
#include <sysio/opp/uic_signature_canonical.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <sysio/permission.hpp>
#include <sysio/crypto.hpp>
#include <magic_enum/magic_enum.hpp>
#include <zpp_bits.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

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

/// `reserv::refundwire` fee argument for full refunds — refunds whose cause
/// the caller does not control (system state changed after enqueue, uwreq
/// rejected at resolution) forfeit nothing.
constexpr uint32_t REFUND_FEE_EXEMPT_BPS = 0;

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

/// Fixed-size, recoverable `sysio::signature` variants accepted at the UIC
/// protocol boundary. WebAuthn is deliberately excluded because its variable
/// fields require a hostile-byte parser; BLS is fixed-size but not recoverable.
/// The enum values are the packed variant tags and `sysio::signature` indices.
enum class uic_signature_variant : uint8_t {
   k1 = 0,
   r1 = 1,
   wa = 2,
   em = 3,
   ed = 4,
   bls = 5,
};

static_assert(std::variant_size_v<sysio::signature> == 6);
static_assert(std::is_same_v<std::variant_alternative_t<2, sysio::signature>,
                             sysio::webauthn_signature>);
static_assert(std::is_same_v<std::variant_alternative_t<4, sysio::signature>,
                             sysio::ed_signature>);
static_assert(std::is_same_v<std::variant_alternative_t<5, sysio::signature>,
                             sysio::bls_signature>);
// K1, R1, and EM intentionally share ecc_signature as their alternative type,
// so type-identity assertions cannot detect a permutation of those indices.
// sysio.dispatch_tests pins their concrete packed tags and exercises real
// contract recovery/permission authorization for all four accepted variants.

constexpr size_t K1_SIGNATURE_VARIANT_INDEX =
   magic_enum::enum_integer(uic_signature_variant::k1);
constexpr size_t R1_SIGNATURE_VARIANT_INDEX =
   magic_enum::enum_integer(uic_signature_variant::r1);
constexpr size_t EM_SIGNATURE_VARIANT_INDEX =
   magic_enum::enum_integer(uic_signature_variant::em);
constexpr size_t ED_SIGNATURE_VARIANT_INDEX =
   magic_enum::enum_integer(uic_signature_variant::ed);

/// Every accepted variant index is encoded as one varuint byte.
constexpr size_t PACKED_SIGNATURE_TAG_SIZE = 1;

/// Exact packed sizes of the one-byte variant tag plus each fixed signature
/// body. K1, R1, and EM share the 65-byte ECC representation; ED embeds its
/// public key in a 96-byte body.
constexpr size_t PACKED_ECC_SIGNATURE_SIZE =
   PACKED_SIGNATURE_TAG_SIZE + std::tuple_size_v<sysio::ecc_signature>;
constexpr size_t PACKED_ED_SIGNATURE_SIZE =
   PACKED_SIGNATURE_TAG_SIZE + std::tuple_size_v<sysio::ed_signature>;

/// Stable action-trace marker for malformed or unauthorized UIC signatures.
constexpr const char* UIC_SIGNATURE_REJECTED_LOG_PREFIX = "UIC_SIGNATURE_REJECTED";

/// Prefix used in the durable commit-entry rejection reason.
constexpr const char* UIC_SIGNATURE_REJECTED_REASON_PREFIX =
   "invalid underwrite-intent-commit ";

/// Separates the rejected leg from its typed verification result.
constexpr const char* UIC_SIGNATURE_REJECTED_REASON_SEPARATOR = " signature: ";

/// Fixed serialized-size allowance for a NEW `commit_entry`'s non-vector
/// fields (underwriter name, two timestamp/outpost-id pairs, status, empty
/// reason + the vectors' length prefixes) in rcrdcommit's projected-row-size
/// guard. The true packed size is ~50 bytes; 128 over-counts on purpose —
/// the guard may only ever be tighter than reality, never looser.
constexpr size_t COMMIT_ENTRY_PACK_ALLOWANCE_BYTES = 128;

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

/// This contract's active lock total for the given
/// `(underwriter, chain_code, token_code)` — an O(1) read of the `locksums`
/// rollup maintained by `add_locked_total` / `sub_locked_total`. An absent row
/// means the bucket holds no live locks.
///
/// This used to walk every lock row the underwriter held, on the assumption
/// that per-underwriter lock counts stay small. They do not: locks are held
/// for the whole wall-clock challenge window and are never released by
/// delivery, so the count is (settlement rate × lock duration). See the
/// `lock_sum` docs for why that mattered on this particular call path.
uint64_t sum_locks_inline(name self,
                           name underwriter,
                           sysio::slug_name chain_code,
                           sysio::slug_name token_code) {
   uwrit::locksums_t sums(self);
   uwrit::lock_sum_key pk{underwriter, chain_code, token_code};
   return sums.contains(pk) ? sums.get(pk).amount : 0;
}

/// Add `amount` to the `(underwriter, chain_code, token_code)` bucket,
/// creating the row when the bucket was empty. Called once per lock row
/// written by `try_select_winner`.
///
/// Saturating, matching the scan it replaced: amounts are uncapped uint64
/// external-chain values, and a wrapped total would UNDERSTATE what is
/// reserved and so OVERSTATE availability — the one direction that lets an
/// overcommit through. Saturation instead overstates `locked`, which fails
/// closed (a swap is refused, never over-collateralized).
void add_locked_total(name self, name underwriter, sysio::slug_name chain_code,
                      sysio::slug_name token_code, uint64_t amount) {
   uwrit::locksums_t sums(self);
   uwrit::lock_sum_key pk{underwriter, chain_code, token_code};
   if (sums.contains(pk)) {
      sums.modify(ram_payer, pk, [&](auto& row) {
         row.amount = opp::safe::add_sat_u64(row.amount, amount);
      });
      return;
   }
   sums.emplace(ram_payer, pk, uwrit::lock_sum{
      .underwriter = underwriter,
      .chain_code  = chain_code,
      .token_code  = token_code,
      .amount      = amount,
   });
}

/// Subtract `amount` from the bucket, erasing the row once it reaches zero so
/// the table holds only live buckets.
///
/// Clamped at zero rather than wrapping. A wrap here would leave a colossal
/// `locked` on the bucket and zero the underwriter's `available()` for the
/// rest of the chain's life — and this runs inside `chklocks`, which is inline
/// in `sysio.epoch::advance` and must never throw. Clamping keeps a
/// hypothetical accounting slip local and self-healing instead.
void sub_locked_total(name self, name underwriter, sysio::slug_name chain_code,
                      sysio::slug_name token_code, uint64_t amount) {
   uwrit::locksums_t sums(self);
   uwrit::lock_sum_key pk{underwriter, chain_code, token_code};
   if (!sums.contains(pk)) return;
   const uint64_t current = sums.get(pk).amount;
   const uint64_t next    = current > amount ? current - amount : 0;
   if (next == 0) {
      sums.erase(pk);
      return;
   }
   sums.modify(ram_payer, pk, [&](auto& row) { row.amount = next; });
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

/// Live `uwconfig` snapshot (defaults when `setconfig` has never run).
/// `self` is the uwrit contract account (where the singleton lives).
uwrit::uw_config read_config(name self) {
   uwrit::uwconfig_t cfg(self);
   return cfg.get_or_default(uwrit::uw_config{});
}

/// Live per-spoke swap fee (basis points) from `uwconfig`, read fresh so the
/// ingestion variance check, the race-time recheck, and settlement all charge
/// one rate.
uint32_t current_fee_bps(name self) {
   return read_config(self).fee_bps;
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
/// reserve's `connector_weight_bps`) and the SAME post-fee reduction: the
/// network `fee_bps` PLUS each participating reserve's own `owner_fee_bps`, all
/// out of the WIRE leg — so the variance check at SWAP_REQUEST receipt time
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

   uint64_t sc = 0, sw = 0; uint32_t scw = 0, sfee = 0;
   if (!src_is_wire) {
      auto r = active(find_reserve(src_chain_code, src_token_code, src_reserve_code));
      if (!r) return 0;
      sc = r->reserve_chain_amount; sw = r->reserve_wire_amount; scw = r->connector_weight_bps;
      sfee = r->owner_fee_bps;
   }
   uint64_t dc = 0, dw = 0; uint32_t dcw = 0, dfee = 0;
   if (!dst_is_wire) {
      auto r = active(find_reserve(dst_chain_code, dst_token_code, dst_reserve_code));
      if (!r) return 0;
      dc = r->reserve_chain_amount; dw = r->reserve_wire_amount; dcw = r->connector_weight_bps;
      dfee = r->owner_fee_bps;
   }
   return opp::amm::quote_swap(src_is_wire, sc, sw, scw,
                               dst_is_wire, dc, dw, dcw,
                               src_amount, fee_bps, sfee, dfee);
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

/// Slippage allowance for a variance check, in absolute destination units:
/// `tolerance_bps` of the **AMM quote**.
///
/// The reference is deliberately the quote and NEVER the caller-supplied
/// `target_amount` (WNS-02). A target crosses the OPP boundary unauthenticated,
/// so deriving the allowance from it made the bound scale with the very number
/// it was meant to constrain: `|quote - target| <= target * tolerance_bps / 10000`
/// holds for ANY target above the quote once `tolerance_bps` reaches 10000, which
/// is what let a caller name an arbitrary destination amount and have it settle.
/// Measured against the quote, the check means what it says — "the price I got is
/// within N% of the price I expected".
///
/// `tolerance_bps` is clamped to `BPS_TOTAL` (100%): a wider tolerance is
/// meaningless (100% already admits every target from zero to twice the quote)
/// and only inflates the allowance. Computed in `uint128_t` so the product cannot
/// overflow for any (quote, tolerance) pair.
uint128_t variance_allowance(uint64_t quote, uint32_t tolerance_bps) {
   const uint32_t bounded_bps = tolerance_bps > opp::amm::BPS_TOTAL ? opp::amm::BPS_TOTAL
                                                                    : tolerance_bps;
   return (static_cast<uint128_t>(quote) * bounded_bps) / opp::amm::BPS_TOTAL;
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
   // underwriter's auditable destination-chain settlement key. This is not
   // the UIC's signed transaction-signer metadata: on EVM the remit carries a
   // 33-byte compressed EM key while `uw_ext_chain_addr` carries the 20-byte
   // address, so the two fields are deliberately not byte-compared.
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

/// Typed outcome of verifying one required UIC leg.
enum class uic_signature_result {
   valid,
   empty_uic,
   malformed_uic,
   non_canonical_uic,
   missing_signature,
   invalid_signature_length,
   unsupported_signature_type,
   non_canonical_signature,
   digest_encoding_failed,
   recovery_failed,
   unauthorized_key,
};

/// Identifies which required UIC leg failed verification.
enum class uic_leg {
   source,
   destination,
};

/// True only when one incoming side already carries complete stored evidence.
bool required_uic_leg_populated(const uwrit::commit_entry& entry, uic_leg leg) {
   return leg == uic_leg::source
      ? !entry.source_uic_bytes.empty() && entry.source_received_at_ms != 0
      : !entry.dest_uic_bytes.empty() && entry.dest_received_at_ms != 0;
}

/// True when every outpost-backed side has complete stored UIC evidence.
bool required_uic_legs_complete(const uwrit::uw_request_t& request,
                                const uwrit::commit_entry& entry) {
   const bool source_complete = leg_is_depot(request.src_chain_code) ||
      required_uic_leg_populated(entry, uic_leg::source);
   const bool destination_complete = leg_is_depot(request.dst_chain_code) ||
      required_uic_leg_populated(entry, uic_leg::destination);
   return source_complete && destination_complete;
}

/**
 * @brief Validate and construct one fixed-size ECDSA UIC signature variant.
 * @tparam VariantIndex Exact `sysio::signature` alternative to construct.
 * @param packed Packed bytes including the one-byte variant tag.
 * @param curve Curve order governing `r` and `s`.
 * @param recovery_position Position of the compact recovery byte.
 * @param recovery_encoding Required numeric recovery-byte representation.
 * @param parsed_sig Destination signature variant.
 * @return Typed length/canonicality outcome, or `valid` after construction.
 */
template <size_t VariantIndex>
uic_signature_result parse_uic_ecc_signature(
   std::span<const char> packed,
   opp::uic_ecdsa_curve curve,
   opp::uic_ecdsa_recovery_position recovery_position,
   opp::uic_ecdsa_recovery_encoding recovery_encoding,
   sysio::signature& parsed_sig) {
   if (packed.size() != PACKED_ECC_SIGNATURE_SIZE) {
      return uic_signature_result::invalid_signature_length;
   }
   const auto body = packed.subspan(PACKED_SIGNATURE_TAG_SIZE);
   if (!opp::is_canonical_uic_ecdsa_signature(
          body, curve, recovery_position, recovery_encoding)) {
      return uic_signature_result::non_canonical_signature;
   }

   sysio::ecc_signature compact_sig{};
   std::copy_n(body.begin(), compact_sig.size(), compact_sig.begin());
   parsed_sig.emplace<VariantIndex>(std::move(compact_sig));
   return uic_signature_result::valid;
}

/// Verify the embedded signature in `uic_bytes` was produced by a key on
/// EITHER the `underwriter` account's `active` OR `owner` permission, over
/// the digest `sha256(serialize(uic_with_signature_blanked))`.
///
/// The accepted wire shapes are the fixed-size recoverable variants already
/// supported by WIRE providers: K1, R1, and EM (one tag byte plus a 65-byte ECC
/// body), and ED (one tag byte plus a 96-byte body). Each body is copied into
/// its known variant directly instead of routing attacker-controlled bytes
/// through the generic variant deserializer, whose fixed-array underrun calls
/// `check()` and would abort the enclosing evalcons transaction. Variable-size
/// WebAuthn and unrecoverable BLS signatures are rejected before construction.
///
/// Per `feedback_opp_handlers_never_throw.md`, every attacker-controlled
/// failure is returned as a typed result. Other permissions (custom names)
/// are intentionally not checked: the plugin is pinned to `active` or
/// `owner`, and accepting a custom permission would create an unconfigured
/// authorization surface.
uic_signature_result verify_uic_signature(name underwriter,
                                          const std::vector<char>& uic_bytes) {
   if (uic_bytes.empty()) return uic_signature_result::empty_uic;

   // Decode the UIC payload.
   opp::attestations::UnderwriteIntentCommit uic;
   {
      auto in = zpp::bits::in{
         std::span{uic_bytes.data(), uic_bytes.size()},
         zpp::bits::no_size{}};
      if (in(uic) != zpp::bits::errc{}) return uic_signature_result::malformed_uic;
   }

   // Require the exact canonical protobuf encoding before hashing or recovery.
   // zpp decode normalizes duplicate/default/unknown-field representations;
   // re-encoding the complete message (with its original signature intact)
   // makes every alternate byte sequence fail closed without throwing.
   std::vector<char> canonical_uic;
   auto canonical_out = zpp::bits::out{canonical_uic, zpp::bits::no_size{}};
   if (canonical_out(uic) != zpp::bits::errc{}) {
      return uic_signature_result::digest_encoding_failed;
   }
   if (canonical_uic != uic_bytes) {
      return uic_signature_result::non_canonical_uic;
   }

   // Validate the exact fixed-size shape before doing any hashing or
   // constructing the signature variant. This both bounds work on malformed
   // inputs and makes the former generic-deserializer underrun unreachable.
   const std::vector<char> sig_bytes_view{uic.signature.begin(), uic.signature.end()};
   if (sig_bytes_view.empty()) return uic_signature_result::missing_signature;
   const auto signature_variant = magic_enum::enum_cast<uic_signature_variant>(
      static_cast<uint8_t>(sig_bytes_view.front()));
   if (!signature_variant) {
      return uic_signature_result::unsupported_signature_type;
   }

   sysio::signature parsed_sig;
   switch (*signature_variant) {
      case uic_signature_variant::k1:
         if (const auto result = parse_uic_ecc_signature<K1_SIGNATURE_VARIANT_INDEX>(
                sig_bytes_view, opp::uic_ecdsa_curve::secp256k1,
                opp::uic_ecdsa_recovery_position::prefix,
                opp::uic_ecdsa_recovery_encoding::compact, parsed_sig);
             result != uic_signature_result::valid) return result;
         break;
      case uic_signature_variant::r1:
         if (const auto result = parse_uic_ecc_signature<R1_SIGNATURE_VARIANT_INDEX>(
                sig_bytes_view, opp::uic_ecdsa_curve::p256,
                opp::uic_ecdsa_recovery_position::prefix,
                opp::uic_ecdsa_recovery_encoding::compact, parsed_sig);
             result != uic_signature_result::valid) return result;
         break;
      case uic_signature_variant::em:
         if (const auto result = parse_uic_ecc_signature<EM_SIGNATURE_VARIANT_INDEX>(
                sig_bytes_view, opp::uic_ecdsa_curve::secp256k1,
                opp::uic_ecdsa_recovery_position::suffix,
                opp::uic_ecdsa_recovery_encoding::ethereum, parsed_sig);
             result != uic_signature_result::valid) return result;
         break;
      case uic_signature_variant::ed: {
         if (sig_bytes_view.size() != PACKED_ED_SIGNATURE_SIZE) {
            return uic_signature_result::invalid_signature_length;
         }
         sysio::ed_signature compact_sig{};
         std::copy_n(sig_bytes_view.begin() + PACKED_SIGNATURE_TAG_SIZE,
                     compact_sig.size(), compact_sig.begin());
         parsed_sig.emplace<ED_SIGNATURE_VARIANT_INDEX>(std::move(compact_sig));
         break;
      }
      case uic_signature_variant::wa:
      case uic_signature_variant::bls:
      default:
         return uic_signature_result::unsupported_signature_type;
   }

   // Blank the signature and recompute the signed digest.
   uic.signature.clear();

   std::vector<char> blanked;
   auto out = zpp::bits::out{blanked, zpp::bits::no_size{}};
   if (out(uic) != zpp::bits::errc{}) {
      return uic_signature_result::digest_encoding_failed;
   }

   sysio::checksum256 digest =
      sysio::sha256(blanked.data(), blanked.size());

   // Recover the public key through the rc-returning intrinsic. Exact
   // fixed-size construction above makes the subjective variable-size
   // signature guard unreachable, while bad recovery bytes or curve values
   // return nullopt.
   auto recovered_opt = sysio::try_recover_key(digest, parsed_sig);
   if (!recovered_opt) return uic_signature_result::recovery_failed;
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
         // A UIC carries exactly one signature. A direct key therefore
         // authorizes it only when that key's weight alone reaches this
         // permission's threshold; mere membership in a multisig authority
         // is not sufficient.
         if (kw.key == recovered && kw.weight >= rec_opt->auth.threshold) {
            return uic_signature_result::valid;
         }
      }
   }
   return uic_signature_result::unauthorized_key;
}

/// Create the durable rejection reason from typed leg and verification values.
std::string create_uic_signature_rejection_reason(uic_leg leg,
                                                  uic_signature_result result) {
   const auto leg_name = magic_enum::enum_name(leg);
   const auto result_name = magic_enum::enum_name(result);

   std::string reason{UIC_SIGNATURE_REJECTED_REASON_PREFIX};
   reason.append(leg_name.data(), leg_name.size());
   reason.append(UIC_SIGNATURE_REJECTED_REASON_SEPARATOR);
   reason.append(result_name.data(), result_name.size());
   return reason;
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
                      uint64_t collateral_lock_duration_ms,
                      uint64_t min_fromwire_amount,
                      uint32_t fromwire_revert_fee_bps,
                      uint32_t uwreq_pending_timeout_epochs,
                      uint32_t uwreq_retention_epochs) {
   require_auth(get_self());
   // Reject a 100% (or higher) fee: it zeroes the post-fee WIRE leg
   // (`net == 0`), which let a swap debit destination reserve liquidity while
   // crediting zero WIRE (SEC-26 / WSA-042). MAX_FEE_BPS == 9999 keeps the
   // remainder positive for every positive input — for THIS rate alone. Reserve
   // owner fees stack on the same leg under their own cap, so the total can
   // still reach 100%; `sysio.reserv`'s settlement `net > 0` checks reject that
   // configured combination.
   check(fee_bps <= MAX_FEE_BPS,
         "fee_bps must be below 10000 (100%): a 100% fee zeroes the post-fee WIRE leg");
   check(collateral_lock_duration_ms > 0,
         "collateral_lock_duration_ms must be positive");
   // Reject a duration so large that `now_ms + collateral_lock_duration_ms`
   // would wrap past UINT64_MAX and yield a lock that expires in the past,
   // releasing collateral the instant it is placed.
   check(collateral_lock_duration_ms <= MAX_COLLATERAL_LOCK_DURATION_MS,
         "collateral_lock_duration_ms exceeds the one-year ceiling");
   // A zero floor would reopen free dust rows in the swap-from-WIRE queue —
   // the floor is the queue-slot price (see DEFAULT_MIN_FROMWIRE_AMOUNT).
   check(min_fromwire_amount > 0, "min_fromwire_amount must be positive");
   // Same 100% rationale as fee_bps, and on this path it is also a liveness
   // rail: a 100% revert fee would zero the post-fee refund transfer inside
   // the never-throw drainfwq drain (`refundwire` rejects zero transfers).
   check(fromwire_revert_fee_bps <= MAX_FEE_BPS,
         "fromwire_revert_fee_bps must be below 10000 (100%): a 100% revert fee zeroes the refund");
   // A zero pending timeout would expire every uwreq the epoch it is created —
   // no race could ever resolve; a zero retention would erase terminal rows
   // before the audit window they exist for. The shared ceiling prevents a
   // near-UINT32_MAX knob from wrapping the `current_epoch + knob` deadline
   // stamp to a tiny epoch index (instant expiry).
   check(uwreq_pending_timeout_epochs > 0,
         "uwreq_pending_timeout_epochs must be positive");
   check(uwreq_pending_timeout_epochs <= MAX_UWREQ_LIFECYCLE_EPOCHS,
         "uwreq_pending_timeout_epochs exceeds the lifecycle ceiling");
   check(uwreq_retention_epochs > 0,
         "uwreq_retention_epochs must be positive");
   check(uwreq_retention_epochs <= MAX_UWREQ_LIFECYCLE_EPOCHS,
         "uwreq_retention_epochs exceeds the lifecycle ceiling");

   uwconfig_t cfg_tbl(get_self());
   uw_config cfg = cfg_tbl.get_or_default(uw_config{});
   cfg.fee_bps                       = fee_bps;
   cfg.collateral_lock_duration_ms   = collateral_lock_duration_ms;
   cfg.min_fromwire_amount           = min_fromwire_amount;
   cfg.fromwire_revert_fee_bps       = fromwire_revert_fee_bps;
   cfg.uwreq_pending_timeout_epochs  = uwreq_pending_timeout_epochs;
   cfg.uwreq_retention_epochs        = uwreq_retention_epochs;
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

   // One config snapshot for the whole ingestion: the variance quote and the
   // PENDING deadline stamp below must read one consistent config.
   const uw_config cfg = read_config(get_self());

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

   // An exact source/destination reserve identity has only one outpost leg.
   // Admitting it would leave the ordinary two-leg request waiting forever
   // for a second distinct commitment. Refund on the proven source outpost
   // and create no UWREQ; same-asset cross-chain swaps and different reserve
   // codes remain distinct triples and continue through the normal path.
   if (src_chain_code == dst_chain_code &&
       src_token_code == dst_token_code &&
       src_reserve_code == dst_reserve_code) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: source and destination reserve "
                       "identities must differ");
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

   // Row-growth rails (SEC-129 / WSA-223): the row stores these payloads
   // verbatim and every later `modify` re-serializes the whole row inside the
   // never-throw dispatch surfaces, so oversized inputs are refused at the
   // door. All caps sit far above legitimate traffic; a violation is a
   // malformed/hostile outpost payload — refund it like every other
   // structural rejection, never check().
   if (data.size() > MAX_ATTESTATION_INBOUND_DATA_BYTES ||
       sr.source_tx_id.size() > MAX_SOURCE_TX_ID_BYTES ||
       sr.actor.address.size() > MAX_DEPOSITOR_BYTES) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "SwapRequest rejected: payload exceeds a size cap "
                       "(attestation data / source_tx_id / depositor address)");
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

   // Price the swap on the depot's live curve. This quote — NOT the caller's
   // `target_amount` — is what the row carries as `dst_amount` (WNS-02).
   // `target_amount` is the caller's *expectation*, an unauthenticated number
   // that arrives over OPP, while `dst_amount` is paid out verbatim by
   // `sysio.reserv::applyswap` / `applyfromwire` / `paywire`. Settling on the
   // caller's figure let a small source deposit name an arbitrary destination
   // amount and drain the destination reserve.
   const uint64_t current_quote = swap_quote(src_chain_code, src_token_code, src_reserve_code,
                                              dst_chain_code, dst_token_code, dst_reserve_code,
                                              src_amount, cfg.fee_bps);
   // Zero quote with every required reserve ACTIVE = an unpriceable / degenerate
   // reserve (a side drained to zero, extreme connector weights, or a leg too
   // small for the weighted-Bancor kernel to price). Fail closed (WSA-041) —
   // otherwise the row would be created with `dst_amount == 0`, which
   // `applyswap` and its siblings assert against, aborting inside evalcons.
   // The gate is no longer nested under `target_amount != 0`: it protects
   // `dst_amount`, which has nothing to do with the caller's target, so it must
   // not depend on the positive-amounts gate above to be reached.
   //
   // A zero quote from a MISSING / not-ACTIVE reserve is the dev & smoke cluster
   // case: no LP is provisioned, so the row is created (the underwriter race
   // still runs) but cannot settle until a reserve exists — `try_select_winner`
   // re-quotes and only ever settles against ACTIVE reserves.
   if (current_quote == 0 &&
       required_reserves_active(src_chain_code, src_token_code, src_reserve_code,
                                dst_chain_code, dst_token_code, dst_reserve_code)) {
      emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                       src_chain_code, src_reserve_code,
                       "unpriceable reserve: zero quote from an ACTIVE reserve "
                       "(degenerate weights/balances) — fail closed");
      return;   // no UWREQ created
   }
   // Variance tolerance — the caller's slippage bound on the quote they will be
   // given, measured as a fraction of that quote (see `variance_allowance`).
   // A zero `target_amount` was already refused by the positive-amounts gate
   // above, so that term is defensive; the live skip is `current_quote == 0`,
   // the unprovisioned-LP case, where there is no price to compare against.
   if (sr.target_amount != 0 && current_quote != 0) {
      const uint64_t  target  = sr.target_amount;
      const uint64_t  diff    = current_quote > target ? current_quote - target
                                                       : target - current_quote;
      const uint128_t allowed = variance_allowance(current_quote, sr.target_tolerance_bps);
      if (static_cast<uint128_t>(diff) > allowed) {
         emit_swap_revert(get_self(), chain_code, attestation_id, sr,
                          src_chain_code, src_reserve_code,
                          "variance exceeded tolerance: target=" + std::to_string(target)
                          + " current=" + std::to_string(current_quote)
                          + " tolerance_bps=" + std::to_string(sr.target_tolerance_bps));
         return;   // no UWREQ created
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
      // The AMM quote, never `sr.target_amount` (WNS-02). Re-quoted and
      // overwritten with the live price at race resolution before anything
      // settles against it.
      .dst_amount                = current_quote,
      // The caller's expectation, retained as the FIXED reference the
      // race-time slippage check measures against. Never paid out.
      .target_amount             = sr.target_amount,
      .variance_tolerance_bps    = sr.target_tolerance_bps,
      .source_tx_id              = sr.source_tx_id,
      .depositor                 = sr.actor.address,
      .commits_by                = {},
      .winner                    = name{},
      .committed_at_ms           = 0,
      .settled_at_ms             = 0,
      // The PENDING deadline (SEC-129 / WSA-223): if no underwriter wins the
      // race within this many epochs, `pruneuwreqs` expires the row and
      // refunds the source side. No trigger ever announces "no winner is
      // coming" — commits are external and optional — so the deadline is what
      // converts a later epoch advance into the recovery trigger.
      .expires_at_epoch          = get_current_epoch() + cfg.uwreq_pending_timeout_epochs,
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
/// candidate-specific, durable pre-settlement invalidity (for example, an
/// older stored UIC invalidated by permission-key rotation or a missing
/// destination-authex identity link). It is
/// deliberately distinct from `UNDERWRITE_STATUS_SLASHED` (an economic
/// punishment that actually burned collateral) and from
/// `UNDERWRITE_STATUS_RELEASED` (a clean race loser whose commit was valid):
/// conflating the three would mislead downstream indexing and audits. A
/// disqualified entry is durable for this request: later records naming the
/// candidate cannot rewrite its evidence or re-arm it for winner selection.
/// Raw UIC payloads are cleared because terminal candidates have no remaining
/// signature-validation reader; timestamps, outpost ids, status, and reason
/// remain as the compact audit record.
void disqualify_candidate(uwrit::uwreqs_t& reqs, const uwrit::id_key& pk,
                          name candidate, const std::string& reason) {
   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, candidate);
      c->status = UnderwriteStatus::UNDERWRITE_STATUS_DISQUALIFIED;
      c->reason = reason;
      c->source_uic_bytes.clear();
      c->dest_uic_bytes.clear();
   });
}

/// Emit the stable rejection marker for an invalid claimed candidate UIC.
/// `chain_code` is the provenance-bound slug supplied by msgch, not an outpost
/// table id. Logging is the only effect when pre-storage validation fails.
void log_uic_signature_rejection(uint64_t uwreq_id,
                                 name candidate,
                                 uic_leg leg,
                                 uint64_t chain_code,
                                 uic_signature_result result) {
   const std::string leg_name{magic_enum::enum_name(leg)};
   const std::string result_name{magic_enum::enum_name(result)};
   sysio::print(UIC_SIGNATURE_REJECTED_LOG_PREFIX,
                ": uwreq=", uwreq_id,
                ", claimed_underwriter=", candidate,
                ", leg=", leg_name,
                ", chain_code=", chain_code,
                ", reason=", result_name, "\n");
}

/// Revalidate an older stored leg when its matching leg arrives. This protects
/// the race from WIRE permission-key rotation between leg arrivals: a key that
/// authorized the first leg may no longer be authorized when the pair becomes
/// eligible to win. Returning false is a normal consensus-path outcome.
bool validate_candidate_uic(uwrit::uwreqs_t& reqs,
                            const uwrit::id_key& pk,
                            uint64_t uwreq_id,
                            name candidate,
                            uic_leg leg,
                            uint64_t chain_code,
                            const std::vector<char>& uic_bytes) {
   const auto result = verify_uic_signature(candidate, uic_bytes);
   if (result == uic_signature_result::valid) return true;

   const auto reason = create_uic_signature_rejection_reason(leg, result);
   log_uic_signature_rejection(uwreq_id, candidate, leg, chain_code, result);
   disqualify_candidate(reqs, pk, candidate, reason);
   return false;
}

/// Terminally close a uwreq that can never settle: refund the source side and
/// mark the row with `terminal_status` (REJECTED for active rejections — the
/// default; EXPIRED when `pruneuwreqs` abandons a PENDING race past its
/// deadline), releasing any in-flight commits. **Non-throwing** — safe inside
/// the evalcons dispatch chain (a `check()` here would stall OPP consensus
/// chain-wide). The refund routes by source-leg kind:
///   * outpost source (`src_needed`): best-effort SWAP_REVERT back to the
///     source outpost (requires the stored SwapRequest to decode; a depot
///     source has no outpost to route to, so `find_outpost_id_for_chain`
///     returns nullopt and the revert is skipped).
///   * depot source   (from-WIRE):    refund the escrowed WIRE to the
///     depositor via `reserv::refundwire`.
/// The terminal modify also stamps the retention deadline
/// (`uwconfig.uwreq_retention_epochs`) and clears the row's heavy payloads
/// (inbound attestation copy + every stored UIC byte blob) — they were only
/// needed to reach this decision; the compact audit metadata (ids, codes,
/// amounts, statuses, reasons, timestamps) is what retention keeps.
void reject_and_refund(name self, uwrit::uwreqs_t& reqs, const uwrit::id_key& pk,
                       const uwrit::uw_request_t& req, bool src_needed,
                       const std::string& revert_reason,
                       const std::string& commit_reason,
                       UnderwriteRequestStatus terminal_status =
                          UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_REJECTED) {
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
      // Full refund: uwreq-stage rejection (race loss, post-drain market
      // movement, expiry) is not a caller-controlled revert cause.
      action(
         permission_level{self, "active"_n},
         uwrit::RESERVE_ACCOUNT, "refundwire"_n,
         std::make_tuple(*user, req.src_amount, REFUND_FEE_EXEMPT_BPS)
      ).send();
   } else {
      sysio::print("reject_and_refund: cannot parse from-WIRE depositor for "
                   "refund on uwreq ", req.id, "\n");
   }
   reqs.modify(same_payer, pk, [&](auto& r) {
      r.status           = terminal_status;
      r.settled_at_ms    = current_time_ms();
      r.expires_at_epoch = get_current_epoch() + read_config(self).uwreq_retention_epochs;
      for (auto& c : r.commits_by) {
         if (c.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED) {
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
            c.reason = commit_reason;
         }
         // Terminal compaction (SEC-129 / WSA-223): the raw UIC blobs were
         // only needed to resolve the race; retention keeps the compact
         // audit metadata, not the payload bytes.
         c.source_uic_bytes.clear();
         c.dest_uic_bytes.clear();
      }
      // The refund above already decoded the snapshot's copy; the stored
      // bytes have no remaining reader on a terminal row.
      r.attestation_inbound_data.clear();
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
/// Stale/invalid evidence disqualifies a candidate durably. Candidate-local
/// eligibility, collateral, and identity-link gaps, plus mutable reserve
/// availability, remain PENDING until an outpost replays an exact stored UIC.
/// Every failure remains non-throwing inside the enclosing consensus dispatch.
void try_select_winner(name self, uint64_t uwreq_id, name candidate,
                       std::optional<uic_leg> just_verified_leg) {
   uwrit::uwreqs_t reqs(self);
   auto pk = uwrit::id_key{uwreq_id};
   if (!reqs.contains(pk)) return;
   auto req = reqs.get(pk);
   if (req.status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING) return;

   const bool src_needed = !leg_is_depot(req.src_chain_code);
   const bool dst_needed = !leg_is_depot(req.dst_chain_code);

   // The resolver only operates on one complete, live candidate. Keeping this
   // invariant here prevents consensus dispatch from reaching costly
   // permission recovery with incomplete or terminal evidence.
   const uwrit::commit_entry* ce_ptr = nullptr;
   for (const auto& c : req.commits_by) {
      if (c.underwriter == candidate) { ce_ptr = &c; break; }
   }
   if (!ce_ptr ||
       ce_ptr->status != UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED ||
       !required_uic_legs_complete(req, *ce_ptr)) {
      return;
   }

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
   // CONFIRMED write, or reserve mutation. This status can change through the
   // ordinary opreg lifecycle, so retain valid UIC evidence and let an exact
   // external replay retry after it becomes eligible rather than permanently
   // disqualifying it. This resolver runs inside the evalcons dispatch chain,
   // so it must remain non-throwing (`feedback_opp_handlers_never_throw`).
   if (!is_active_underwriter(candidate)) {
      sysio::print("try_select_winner: candidate ", candidate,
                   " is not an ACTIVE underwriter for uwreq ", uwreq_id,
                   ", leaving valid commit retryable\n");
      return;
   }

   // ── T7: permission freshness ──────────────────────────────────────────
   // `rcrdcommit` passes its incoming leg after verifying it immediately, so
   // only the older stored opposite leg needs another recovery against current
   // owner/active keys before locks or settlement.
   if (src_needed && just_verified_leg != std::optional{uic_leg::source} &&
       !validate_candidate_uic(reqs, pk, uwreq_id, candidate, uic_leg::source,
                               ce_ptr->source_outpost_id,
                               ce_ptr->source_uic_bytes)) {
      return;
   }
   if (dst_needed && just_verified_leg != std::optional{uic_leg::destination} &&
       !validate_candidate_uic(reqs, pk, uwreq_id, candidate,
                               uic_leg::destination, ce_ptr->dest_outpost_id,
                               ce_ptr->dest_uic_bytes)) {
      return;
   }

   // ── Settlement quote — re-price on the live curve ────────────────────
   // The books move in this transaction, so the price they move at is the price
   // computed HERE, not the one quoted at ingestion. Settling on a stale quote
   // would over-debit the destination reserve by exactly the drift the LP took
   // between ingestion and the race.
   //
   // Only the quote is computed at this point — every decision it feeds is made
   // below, AFTER the candidate has proved its bond. The quote must come first
   // regardless, because it is what sizes the obligation the bond has to cover.
   const uint64_t settle_quote = swap_quote(
      req.src_chain_code, req.src_token_code, req.src_reserve_code,
      req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
      req.src_amount, current_fee_bps(self));
   if (settle_quote != 0) {
      // Every downstream read of `req.dst_amount` — bond coverage below, the
      // reserve pre-checks, the lock amount, the SWAP_REMIT payload, and the
      // reserv settlement actions — now uses the live price, and the CONFIRMED
      // write persists it as the amount the winner owes. When the curve cannot
      // price the swap the stored ingestion quote stands in for the bond check;
      // the request is terminated (or skipped) below before it could settle.
      req.dst_amount = settle_quote;
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
   // passing value. Collateral can be replenished or freed by lock expiry, so
   // insufficient coverage keeps this valid candidate replayable while the
   // uwreq remains PENDING. This resolver is non-throwing
   // (`feedback_opp_handlers_never_throw`).
   const bool same_bucket = src_needed && dst_needed
                            && req.src_chain_code == req.dst_chain_code
                            && req.src_token_code == req.dst_token_code;
   if (same_bucket) {
      // One opreg balance funds both legs — it must cover their sum. uint128
      // keeps the comparison honest when src+dst exceeds uint64 (no uint64
      // balance can cover it, so availability is insufficient by construction).
      const uint64_t  avail = available_via_mirrors(self, candidate,
                                                    req.src_chain_code, req.src_token_code);
      const uint128_t need  = static_cast<uint128_t>(req.src_amount) + req.dst_amount;
      if (static_cast<uint128_t>(avail) < need) {
         const std::string required = need > std::numeric_limits<uint64_t>::max()
            ? "greater than uint64 maximum"
            : std::to_string(static_cast<uint64_t>(need));
         sysio::print("try_select_winner: insufficient available collateral for uwreq ",
                      uwreq_id, " candidate ", candidate, " in bucket (",
                      req.src_chain_code.to_string(), ", ", req.src_token_code.to_string(),
                      "): required=", required, ", available=", avail,
                      ", leaving valid commit retryable\n");
         return;
      }
   } else {
      // Distinct buckets (or a single required leg) — check each independently.
      const uint64_t src_avail = src_needed
         ? available_via_mirrors(self, candidate, req.src_chain_code, req.src_token_code) : 0;
      const uint64_t dst_avail = dst_needed
         ? available_via_mirrors(self, candidate, req.dst_chain_code, req.dst_token_code) : 0;
      if (src_needed && src_avail < req.src_amount) {
         sysio::print("try_select_winner: insufficient source collateral for uwreq ",
                      uwreq_id, " candidate ", candidate,
                      ", leaving valid commit retryable\n");
         return;
      }
      if (dst_needed && dst_avail < req.dst_amount) {
         sysio::print("try_select_winner: insufficient destination collateral for uwreq ",
                      uwreq_id, " candidate ", candidate,
                      ", leaving valid commit retryable\n");
         return;
      }
   }

   // ── Candidate-specific remit eligibility ─────────────────────────────
   // Build the outbound remit before any request-global verdict. A missing
   // destination-chain authex link belongs to this candidate. It can be added
   // without changing the request or UIC evidence, so an exact external replay
   // can retry after the link is provisioned rather than permanently
   // disqualifying the candidate.
   // Request-wide build failures remain terminal, and every outcome here is
   // still after the authoritative bond gate above.
   uint64_t remit_dst_outpost_id = 0;
   std::vector<char> remit_encoded;
   if (dst_needed) {
      switch (try_build_swap_remit(req, candidate, remit_dst_outpost_id, remit_encoded)) {
         case swap_remit_disp::ok:
            break;
         case swap_remit_disp::disqualified:
            sysio::print("try_select_winner: candidate ", candidate,
                         " has no destination authex link for uwreq ", uwreq_id,
                         ", leaving valid commit retryable\n");
            return;
         case swap_remit_disp::terminal:
            reject_and_refund(self, reqs, pk, req, src_needed,
               "swap unremittable at race resolution: destination outpost / "
               "chain-kind unresolved or stored request undecodable",
               "uwreq rejected: destination unremittable at race resolution");
            return;
      }
   }

   // ── Request-level terminal decisions ─────────────────────────────────
   // Deliberately placed AFTER every candidate-specific gate (eligibility,
   // signature, bond, destination identity). These outcomes REFUND the user
   // and close the request for good, so they must not be reachable by a
   // candidate that was never going to win: an ACTIVE underwriter can clear
   // the role minimum yet be under-bonded for this particular swap, and
   // letting it terminally close a healthy request during transient drift
   // would hand any such operator a denial-of-service on other people's swaps.
   // A candidate that reaches this point is eligible, remittable, and bonded
   // for the currently known obligation, so every remaining verdict belongs
   // to the request itself.
   if (settle_quote == 0) {
      if (required_reserves_active(req.src_chain_code, req.src_token_code, req.src_reserve_code,
                                   req.dst_chain_code, req.dst_token_code, req.dst_reserve_code)) {
         // Every required reserve is ACTIVE yet the curve cannot price the swap
         // — a drained side, extreme connector weights, or a leg below the
         // kernel's pricing floor. Terminal: fail closed rather than settle at a
         // price the AMM does not have (WSA-041).
         reject_and_refund(self, reqs, pk, req, src_needed,
            "unpriceable reserve: zero quote from an ACTIVE reserve at race resolution",
            "uwreq reverted at race resolution (unpriceable reserve)");
         return;
      }
      // A required reserve may be provisioned or activated while the UWREQ is
      // still PENDING. Keep the candidate's valid evidence so a later exact
      // UIC replay can re-evaluate the live route.
      sysio::print("try_select_winner: required reserve missing or not ACTIVE for uwreq ",
                   uwreq_id, ", leaving request PENDING\n");
      return;
   }
   // Slippage — the live settlement quote against the user's ORIGINAL
   // `target_amount`, never against the previous quote. Measuring drift from
   // `dst_amount` (the ingestion quote) would compound the tolerance across the
   // two checkpoints: a 10% bound would accept a 91 quote at ingestion and then
   // an 83 quote at settlement, delivering 17% below a target of 100. Anchoring
   // on the fixed target means the bound the user agreed to is the bound they
   // get, whatever path the price took. `target_amount` is guaranteed positive
   // by `createuwreq` / `swapfromwire`, so the guard is defensive.
   // Non-throwing throughout (`feedback_opp_handlers_never_throw`).
   if (req.target_amount != 0) {
      const uint64_t  diff    = settle_quote > req.target_amount
                                   ? settle_quote - req.target_amount
                                   : req.target_amount - settle_quote;
      const uint128_t allowed = variance_allowance(settle_quote, req.variance_tolerance_bps);
      if (static_cast<uint128_t>(diff) > allowed) {
         reject_and_refund(self, reqs, pk, req, src_needed,
            "variance exceeded tolerance at race resolution: "
            "target=" + std::to_string(req.target_amount)
            + " current=" + std::to_string(settle_quote)
            + " tolerance_bps=" + std::to_string(req.variance_tolerance_bps),
            "uwreq reverted at race resolution (variance drift)");
         return;
      }
   }

   // ── Settlement pre-checks against the local reserve mirror ───────────
   // The reserv-side settlement actions (`applyswap` / `applyfromwire` /
   // `paywire`) `check()`-abort on violation; an abort inside this
   // dispatch chain stalls consensus, so every condition is pre-validated
   // here — making the inline actions unreachable-failure by construction
   // (no other reserve mutation can interleave within this transaction).
   // A live reserve shortfall is shared mutable state. It may recover through
   // a reserve top-up or a different settlement, so keep the request PENDING
   // rather than letting a momentary shortfall permanently refund it.
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
      // reserv::paywire, and an amount past asset::max_amount (2^62-1) has no
      // WIRE-asset representation — it would (a) wrap the `dst_amount +
      // to_wire_fee` sufficiency guard below and (b) abort paywire's asset()
      // constructor mid-evalcons, stalling consensus. Since `dst_amount` is the
      // AMM quote (WNS-02), it is bounded by the source reserve's WIRE side,
      // itself an asset-bounded balance — so this is unreachable
      // defense-in-depth rather than a live path. Kept because the consequence
      // of it ever becoming reachable is a chain-wide consensus stall; it must
      // stay AHEAD of any CONFIRMED / lock / reserve write.
      if (req.dst_amount > static_cast<uint64_t>(asset::max_amount)) {
         reject_and_refund(self, reqs, pk, req, src_needed,
            "swap-to-WIRE rejected: quoted amount exceeds the maximum "
            "representable WIRE asset",
            "uwreq rejected: to-WIRE quote exceeds asset max_amount");
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
      // The TOTAL fee `paywire` will charge on this leg: the network fee plus
      // the source reserve's own owner fee (swap-to-WIRE has no destination
      // reserve). Only `.fee` is read, and the total is independent of how the
      // network fee sub-divides, so the emissions share is irrelevant here.
      const uint64_t to_wire_fee = opp::amm::split_wire_fee(
         w_gross, current_fee_bps(self), reserve::FEE_UNDERWRITER_SHARE_BPS,
         /*emissions_share_bps*/ 0,
         src_r ? src_r->owner_fee_bps : 0, /*dst_reserve_fee_bps*/ 0).fee;
      const uint64_t wire_needed = opp::safe::add_sat_u64(req.dst_amount, to_wire_fee);
      if (!src_r || w_gross == 0 || src_r->reserve_wire_amount < wire_needed) {
         sysio::print("try_select_winner: insufficient source-reserve WIRE for uwreq ",
                      uwreq_id, ", leaving request PENDING\n");
         return;
      }
   } else if (!src_needed) {
      // Swap-from-WIRE. drainfwq validated at queue-drain; re-validate the
      // live state (privacy is immutable, but liquidity can drift).
      auto dst_r = find_active_reserve(req.dst_chain_code, req.dst_token_code, req.dst_reserve_code);
      if (!dst_r || dst_r->is_private || dst_r->reserve_chain_amount < req.dst_amount) {
         sysio::print("try_select_winner: destination reserve cannot settle uwreq ",
                      uwreq_id, ", leaving request PENDING\n");
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
         sysio::print("try_select_winner: insufficient reserve liquidity for uwreq ",
                      uwreq_id, ", leaving request PENDING\n");
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
      add_locked_total(self, candidate, req.src_chain_code, req.src_token_code,
                       req.src_amount);
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
      add_locked_total(self, candidate, req.dst_chain_code, req.dst_token_code,
                       req.dst_amount);
   }

   reqs.modify(same_payer, pk, [&](auto& r) {
      r.status          = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED;
      r.winner          = candidate;
      r.committed_at_ms = current_time_ms();
      // The live quote this race settled at — the amount the winner owes on the
      // destination chain, and the amount the locks and the SWAP_REMIT carry.
      // Persisting it keeps the row's record of the swap equal to what the
      // reserve books actually moved.
      r.dst_amount      = req.dst_amount;
      // Deadline cleared (SEC-129 / WSA-223): a CONFIRMED row is owned by the
      // wall-clock lock window — `chklocks` is guaranteed to terminalize it
      // once the locks expire, so a second (epoch-count) deadline would just
      // race the first. 0 excludes the row from the `pruneuwreqs` sweep.
      r.expires_at_epoch = 0;
      // Mark the winner's commit_entry CONFIRMED, others RELEASED (loser).
      for (auto& c : r.commits_by) {
         if (c.underwriter == candidate) {
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_CONFIRMED;
         } else if (c.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED) {
            // Eligible loser — promote to RELEASED for retention/debugging.
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
            c.reason = "lost the COMMIT race";
         }
         // Race resolved — non-winner UIC blobs have no remaining reader.
         // The winner's bytes stay through the lock window as
         // the challenge-evidence trail; `chklocks` clears them at COMPLETED.
         if (c.underwriter != candidate) {
            c.source_uic_bytes.clear();
            c.dest_uic_bytes.clear();
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
                          req.src_amount, towire_recipient, req.dst_amount, candidate)
      ).send();
   } else if (!src_needed) {
      // Swap-from-WIRE: the escrowed WIRE becomes dst-reserve liquidity,
      // then the normal remit tail to the destination outpost.
      action(
         permission_level{self, "active"_n},
         uwrit::RESERVE_ACCOUNT, "applyfromwire"_n,
         std::make_tuple(req.dst_chain_code, req.dst_token_code, req.dst_reserve_code,
                          req.src_amount, req.dst_amount, candidate)
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
                          req.dst_amount, candidate)
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
   // gate, so an entry for
   // any other account is pure dead weight. Refuse to create one: otherwise a matched-leg UIC could
   // append one commit_entry per attacker-chosen valid-but-unregistered account name and bloat the
   // row. Fail closed, no mutation, never check().
   if (!is_active_underwriter(underwriter)) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " underwriter ", underwriter,
                   " is not an ACTIVE underwriter, skipping\n");
      return;
   }

   // Reject a new name at the roster cap before protobuf decoding, hashing, or
   // recovery. Existing candidates can still provide their missing required leg.
   const auto existing_it =
      std::find_if(req_snapshot.commits_by.begin(), req_snapshot.commits_by.end(),
                   [&](const commit_entry& c) { return c.underwriter == underwriter; });
   const bool existing_entry = existing_it != req_snapshot.commits_by.end();
   if (!existing_entry && req_snapshot.commits_by.size() >= MAX_UWREQ_CANDIDATES) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " already carries ",
                   req_snapshot.commits_by.size(),
                   " candidates (cap reached), skipping new candidate\n");
      return;
   }

   const auto incoming_leg = is_source ? uic_leg::source : uic_leg::destination;

   // Disqualification is durable for this request. Once winner-time checks
   // reject a candidate, no replay, malformed record, unauthorized record, or
   // even newly valid replacement may rewrite the evidence/timestamps or
   // re-arm that candidate. Another underwriter can still win the open race.
   if (existing_entry &&
       existing_it->status == UnderwriteStatus::UNDERWRITE_STATUS_DISQUALIFIED) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " candidate ", underwriter,
                   " is already DISQUALIFIED, skipping\n");
      return;
   }

   // A candidate leg is write-once. Outpost delivery is at-least-once, so a
   // conflicting duplicate is never replacement evidence. A byte-identical
   // replay of a complete candidate is the explicit external trigger for a
   // fresh winner-selection pass: it does not mutate evidence or timestamps,
   // and `std::nullopt` makes the resolver revalidate every stored leg against
   // current permissions.
   if (existing_entry && required_uic_leg_populated(*existing_it, incoming_leg)) {
      const auto& stored_bytes = is_source
         ? existing_it->source_uic_bytes
         : existing_it->dest_uic_bytes;
      if (stored_bytes != uic_bytes) {
         sysio::print("rcrdcommit: uwreq ", uwreq_id, " candidate ", underwriter,
                      " received conflicting duplicate ",
                      is_source ? "source" : "destination", " UIC, skipping\n");
         return;
      }
      if (required_uic_legs_complete(req_snapshot, *existing_it)) {
         sysio::print("rcrdcommit: uwreq ", uwreq_id, " candidate ", underwriter,
                      " received exact stored UIC replay, re-evaluating\n");
         try_select_winner(get_self(), uwreq_id, underwriter, std::nullopt);
         return;
      }
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " candidate ", underwriter,
                   " already carries a ",
                   is_source ? "source" : "destination",
                   " UIC, skipping duplicate\n");
      return;
   }

   // Reject oversized blobs before protobuf decoding, hashing, or recovery.
   // Candidate evidence/status/reason/timestamps remain untouched.
   if (uic_bytes.size() > MAX_UIC_LEG_BYTES) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " UIC payload of ",
                   uic_bytes.size(), " bytes exceeds the per-leg cap, skipping\n");
      return;
   }

   // Validate the claimed underwriter's signature before changing candidate
   // evidence/status/reason/timestamps.
   // Current outposts bind the canonical UIC's claimed identity and external
   // address to their authenticated caller, while the depot remains
   // authoritative for binding `uw_account` to current WIRE permission keys.
   // An invalid claim must never overwrite an honest
   // candidate's previously stored leg, re-arm its status, or create a
   // competitor-visible entry. Log and ignore it; later
   // records in the same envelope continue dispatching.
   const auto signature_result = verify_uic_signature(underwriter, uic_bytes);
   if (signature_result != uic_signature_result::valid) {
      log_uic_signature_rejection(uwreq_id, underwriter, incoming_leg,
                                  chain_code, signature_result);
      return;
   }

   // Remaining row-growth rails (SEC-129 / WSA-223) — every rail fails closed
   // with no mutation, never check() (we are inside the evalcons dispatch
   // chain). The candidate roster is capped so the ACTIVE-underwriter population
   // bounds scan cost but not row size…
   // …and the projected whole-row size is guarded so THIS modify — and every
   // later consensus-dispatched one — stays clear of chain KV value limits.
   // Conservative projection: current packed row + the incoming leg bytes +
   // a fixed allowance for a new entry's non-vector fields.
   const size_t projected_row_bytes = pack_size(req_snapshot)
                                    + uic_bytes.size()
                                    + (existing_entry ? 0 : COMMIT_ENTRY_PACK_ALLOWANCE_BYTES);
   if (projected_row_bytes > MAX_UWREQ_ROW_BYTES) {
      sysio::print("rcrdcommit: uwreq ", uwreq_id, " projected row size ",
                   projected_row_bytes, " bytes exceeds the row cap, skipping\n");
      return;
   }

   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, underwriter);
      const uint64_t admitted_at_ms = current_time_ms();
      if (is_source) {
         c->source_received_at_ms = admitted_at_ms;
         c->source_outpost_id     = chain_code;
         c->source_uic_bytes      = uic_bytes;
      } else if (is_dest) {
         c->dest_received_at_ms = admitted_at_ms;
         c->dest_outpost_id     = chain_code;
         c->dest_uic_bytes      = uic_bytes;
      }
   });

   // Re-read after modify — try_select_winner needs the latest commit_entry.
   // A leg is required iff its chain is an outpost; a depot (WIRE) leg is
   // satisfied by construction (no outpost exists to send a UIC for it),
   // so single-leg swaps arm the race on their one real commit.
   auto refreshed = reqs.get(pk);
   for (const auto& c : refreshed.commits_by) {
      if (c.underwriter != underwriter) continue;
      if (required_uic_legs_complete(refreshed, c)) {
         try_select_winner(get_self(), uwreq_id, underwriter, incoming_leg);
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
   // Queue-slot price floor (default 5 WIRE, retunable via setconfig): the
   // escrow of a failed row is refunded, so without a floor dust rows could
   // hold drain slots while locking no meaningful capital — and sit below the
   // Bancor kernel's pricing floor, guaranteeing zero-quote reverts.
   check(wire_amount >= read_config(get_self()).min_fromwire_amount,
         "swapfromwire: wire_amount below the configured minimum");
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
// account existed at escrow time and accounts are not deletable; the
// post-fee refund stays positive because setconfig caps the revert fee
// below 100%). Validation failures refund + drop; only validated rows
// become PENDING uwreqs.
void uwrit::drainfwq() {
   check(has_auth(EPOCH_ACCOUNT) || has_auth(get_self()),
         "drainfwq requires sysio.epoch or sysio.uwrit authority");

   fwqueue_t q(get_self());
   uwreqs_t reqs(get_self());
   const auto depot_code = depot_chain_code();
   // One config snapshot for the whole drain: quotes and revert fees inside a
   // single action must charge one rate.
   const uw_config cfg = read_config(get_self());

   // Bounded FIFO drain (SEC-77): process at most MAX_FWQ_DRAIN_PER_EPOCH rows
   // per advance, oldest first (primary-key = id order). Every branch of the
   // loop body erases the front row, so re-reading `q.begin()` each iteration
   // walks the queue forward and the next advance resumes where this one
   // stopped; undrained rows keep their WIRE escrow in reserve custody and
   // drain a later epoch. Bounding this loop is what keeps `advance` inside its
   // hard, uncatchable CPU deadline: an unbounded drain over an attacker-
   // inflated queue would abort every advance and stall epoch progress chain-
   // wide. `drained` is incremented before any of the per-row `continue`
   // branches, so it counts every attempt (refund-and-drop included). Reading
   // only the front row (not a full-table pre-copy) also bounds the scan cost.
   uint32_t drained = 0;
   while (drained < MAX_FWQ_DRAIN_PER_EPOCH) {
      auto head = q.begin();
      if (head == q.end()) break;
      const auto row = *head;
      ++drained;

      // Fault taxonomy for the revert fee: system-caused failures (the chain
      // registry or reserve state changed AFTER swapfromwire validated the
      // row) refund in full via REFUND_FEE_EXEMPT_BPS; failures the caller's
      // own parameters produce (unpriceable escrow, missed variance
      // tolerance) forfeit `fromwire_revert_fee_bps` so revert churn cannot
      // recycle for free.
      auto refund_and_drop = [&](const char* why, uint32_t revert_fee_bps) {
         sysio::print("drainfwq: ", why, " — refunding queued swap ", row.id, "\n");
         action(
            permission_level{get_self(), "active"_n},
            RESERVE_ACCOUNT, "refundwire"_n,
            std::make_tuple(row.user, row.wire_amount, revert_fee_bps)
         ).send();
         q.erase(fw_key{row.id});
      };

      if (!depot_code) {
         refund_and_drop("depot chain not registered", REFUND_FEE_EXEMPT_BPS);
         continue;
      }
      auto r = find_reserve(row.dst_chain_code, row.dst_token_code, row.dst_reserve_code);
      if (!r || r->status != ReserveStatus::RESERVE_STATUS_ACTIVE) {
         refund_and_drop("target reserve missing or not ACTIVE", REFUND_FEE_EXEMPT_BPS);
         continue;
      }
      if (r->is_private) {
         refund_and_drop("target reserve is private (excluded from WIRE-endpoint swaps)",
                         REFUND_FEE_EXEMPT_BPS);
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
                                         cfg.fee_bps);
      if (quote == 0) {
         refund_and_drop("unpriceable target reserve: zero quote from an ACTIVE reserve",
                         cfg.fromwire_revert_fee_bps);
         continue;
      }
      {
         // `target_amount > 0` is guaranteed by swapfromwire. The allowance is a
         // fraction of the AMM `quote`, never of the user's target (WNS-02) —
         // see `variance_allowance`.
         const uint64_t diff = quote > row.target_amount
                                  ? quote - row.target_amount
                                  : row.target_amount - quote;
         const uint128_t allowed = variance_allowance(quote, row.variance_tolerance_bps);
         if (static_cast<uint128_t>(diff) > allowed) {
            refund_and_drop("variance exceeded tolerance at drain",
                            cfg.fromwire_revert_fee_bps);
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
         // Internal encode failure — not a caller-controlled revert cause.
         refund_and_drop("failed to encode synthetic SwapRequest", REFUND_FEE_EXEMPT_BPS);
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
         // The AMM quote, never the user's `target_amount` (WNS-02) — same
         // contract as `createuwreq`; re-quoted at race resolution.
         .dst_amount                = quote,
         // The user's expectation, retained as the FIXED slippage reference.
         .target_amount             = row.target_amount,
         .variance_tolerance_bps    = row.variance_tolerance_bps,
         .source_tx_id              = std::move(stx),
         .depositor                 = wire_name_bytes(row.user),
         .commits_by                = {},
         .winner                    = name{},
         .committed_at_ms           = 0,
         .settled_at_ms             = 0,
         // Same PENDING deadline as createuwreq (SEC-129 / WSA-223): a
         // from-WIRE race that never resolves is expired by `pruneuwreqs`,
         // which refunds the escrow in full via `refundwire`.
         .expires_at_epoch          = get_current_epoch() + cfg.uwreq_pending_timeout_epochs,
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
void uwrit::chklocks(uint32_t max_rows) {
   // Two valid callers, mirroring pruneuwreqs / drainfwq:
   //   * sysio.epoch::advance — inlined at every epoch boundary with
   //     MAX_LOCK_RELEASE_PER_EPOCH.
   //   * sysio.uwrit — manual cleanup invocation with a caller-chosen budget,
   //     e.g. from a migration.
   check(has_auth(EPOCH_ACCOUNT) || has_auth(get_self()),
         "chklocks requires sysio.epoch or sysio.uwrit authority");
   if (max_rows == 0) return;

   const uint64_t now_ms = current_time_ms();
   locks_t locks(get_self());
   auto idx = locks.get_index<"byexpire"_n>();

   // Walk in ascending `expires_at_ms` and collect full copies while
   // expired — we erase in a second pass (an erase invalidates the index
   // cursor) and need every field for the releaselock fan-out.
   //
   // Bounded to `max_rows`, for the same reason as MAX_UWREQ_PRUNE_PER_EPOCH
   // and MAX_FWQ_DRAIN_PER_EPOCH: the per-lock work here is an inline
   // `opreg::releaselock` dispatch plus an erase, and it runs inside
   // advance's hard, uncatchable transaction CPU deadline. Expiries arrive
   // in BURSTS — every lock is stamped `now + collateral_lock_duration_ms`
   // at creation, so one epoch's settlements all fall due in one epoch a
   // lock-duration later — and an unbounded sweep of such a burst would
   // abort `advance`. Those locks would still be expired at the next
   // advance, so it would abort identically every epoch thereafter: a
   // permanent chain-wide epoch stall. Ascending expiry order makes the
   // bound a FIFO drain; an oversized burst simply spreads across
   // subsequent epochs.
   std::vector<lock_entry> expired;
   for (auto it = idx.begin();
        it != idx.end() && it->expires_at_ms <= now_ms && expired.size() < max_rows;
        ++it) {
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
      sub_locked_total(get_self(), l.underwriter, l.chain_code, l.token_code, l.amount);
      if (std::find(affected.begin(), affected.end(), l.uwreq_id) == affected.end()) {
         affected.push_back(l.uwreq_id);
      }
   }

   // COMPLETED flip — a CONFIRMED uwreq whose final lock just swept has
   // exited its challenge window. The flip stamps the retention deadline
   // (`pruneuwreqs` erases the row once it elapses) and clears the remaining
   // heavy payloads: with the challenge window closed, the winner's UIC
   // evidence and the inbound attestation copy have no remaining reader —
   // retention keeps the compact audit metadata only (SEC-129 / WSA-223).
   uwreqs_t reqs(get_self());
   auto byuwreq = locks.get_index<"byuwreq"_n>();
   const uint32_t now_ep = get_current_epoch();
   const uint32_t retention_epochs = read_config(get_self()).uwreq_retention_epochs;
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
         row.expires_at_epoch = now_ep + retention_epochs;
         row.attestation_inbound_data.clear();
         for (auto& c : row.commits_by) {
            c.source_uic_bytes.clear();
            c.dest_uic_bytes.clear();
         }
      });
   }
}

// ---------------------------------------------------------------------------
//  pruneuwreqs — bounded UWREQ lifecycle sweep (SEC-129 / WSA-223)
// ---------------------------------------------------------------------------
//
// The enforcement half of `uw_request_t.expires_at_epoch`: every deadline is
// stamped by the lifecycle writes (createuwreq / drainfwq / reject_and_refund
// / chklocks) and evaluated only HERE, lazily, when the epoch machinery
// triggers this action. Nothing is scheduled; a row past its deadline is
// inert data until the sweep fires. NEVER throws past the auth gate: it runs
// inline inside `sysio.epoch::advance`, where an abort stalls epoch progress
// chain-wide. The per-call `max_rows` budget keeps the sweep inside advance's
// transaction CPU deadline (same rationale as MAX_FWQ_DRAIN_PER_EPOCH); a
// backlog drains across subsequent epochs.
void uwrit::pruneuwreqs(uint32_t max_rows) {
   // Two valid callers, mirroring chklocks / drainfwq:
   //   * sysio.epoch::advance — inlined each epoch with MAX_UWREQ_PRUNE_PER_EPOCH.
   //   * sysio.uwrit — manual backlog drain with a caller-chosen budget.
   check(has_auth(EPOCH_ACCOUNT) || has_auth(get_self()),
         "pruneuwreqs requires sysio.epoch or sysio.uwrit authority");
   if (max_rows == 0) return;

   const uint32_t now_ep = get_current_epoch();
   uwreqs_t reqs(get_self());
   auto idx = reqs.get_index<"byexpire"_n>();

   // Collect-then-act, bounded to `max_rows` copies: a modify/erase
   // invalidates the index cursor (same two-pass shape as chklocks), and the
   // bounded copy keeps the scan cost budgeted. `lower_bound(1)` skips the
   // `expires_at_epoch == 0` population — rows with no deadline (CONFIRMED,
   // owned by the wall-clock lock window) are never swept.
   std::vector<uw_request_t> due;
   for (auto it = idx.lower_bound(uint64_t{1});
        it != idx.end() && it->expires_at_epoch <= now_ep && due.size() < max_rows;
        ++it) {
      due.push_back(*it);
   }
   if (due.empty()) return;

   for (const auto& req : due) {
      auto pk = id_key{req.id};
      switch (req.status) {
         case UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING: {
            // The race never resolved inside the pending timeout — abandon
            // it: refund the source side (SWAP_REVERT to the source outpost,
            // or the full from-WIRE escrow via refundwire — expiry is not a
            // caller-controlled revert cause), flip to EXPIRED, and start the
            // terminal retention window. PENDING rows hold no locks (locks
            // are first written in the same modify that flips CONFIRMED), so
            // there is nothing to release.
            const bool src_needed = !leg_is_depot(req.src_chain_code);
            reject_and_refund(get_self(), reqs, pk, req, src_needed,
               "underwrite request expired: no underwriter resolved the race "
               "within the pending timeout",
               "uwreq expired: pending deadline elapsed before the race resolved",
               UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_EXPIRED);
            break;
         }
         case UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_REJECTED:
         case UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_COMPLETED:
         case UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_EXPIRED:
            // Terminal row past its retention window — remove it.
            reqs.erase(pk);
            break;
         default:
            // CONFIRMED (or a future status) with a live deadline should be
            // unreachable — winner selection zeroes the deadline. Defensive
            // skip + log; never throw inside advance.
            sysio::print("pruneuwreqs: uwreq ", req.id, " in status ",
                         magic_enum::enum_integer(req.status),
                         " unexpectedly carries an elapsed deadline, skipping\n");
            break;
      }
   }
}

} // namespace sysio
