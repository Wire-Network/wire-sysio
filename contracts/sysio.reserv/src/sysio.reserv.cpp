#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio.opp.common/registry_metadata.hpp>

#include <zpp_bits.h>

#include <cstring>
#include <limits>
#include <optional>

namespace sysio {

namespace {

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

uint32_t get_current_epoch_index() {
   sysio::epoch::epochstate_t es(reserve::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

bool is_bootstrap_window() {
   return get_current_epoch_index() == 0;
}

void require_priv_caller() {
   require_auth(current_receiver());
   sysio::check(sysio::is_privileged(current_receiver()),
                "sysio.reserv: privileged account required");
}

using sysio::slug_name_literals::operator""_s;

/// The WIRE token / depot-chain slug. A reserve leg whose token code is WIRE is
/// a depot (WIRE) endpoint with no token/WIRE pool of its own.
constexpr sysio::slug_name WIRE_TOKEN = "WIRE"_s;

/// Saturating uint64 credit for reserve balances / rewards-bucket counters. These accumulate from
/// operator-relayed external-chain amounts (no on-chain supply cap), and the credit sites run
/// inside the consensus dispatch chain (applyswap / applyfromwire / paywire inline from
/// uwrit::try_select_winner). A raw `+=` could wrap the uint64 and corrupt the
/// weighted-AMM curve and the `>=` sufficiency checks; cap at UINT64_MAX instead — never wrap,
/// never throw on the consensus path. Delegates to the shared
/// `sysio::opp::safe::add_sat_u64` so the never-wrap rule lives in one place.
///
/// The cap is unreachable for any real BALANCE — a balance is bounded by what is
/// actually in custody. That reasoning does NOT carry to the monotonic
/// `lifetime_*` audit counters this helper also credits: those are unbounded
/// running totals and CAN saturate, which is documented at each field
/// (`reserve_row::owner_fee_lifetime`, `uw_fee_row::lifetime_*`). Saturation
/// there truncates audit history only — never a balance, never a payout.
inline void add_capped_u64(uint64_t& balance, uint64_t amt) {
   balance = sysio::opp::safe::add_sat_u64(balance, amt);
}

/// Live per-spoke swap fee (basis points) — owned by `sysio.uwrit`. Read fresh
/// so the read-only quote and settlement charge one and the same rate. Falls
/// back to the `uw_config` default when uwrit has not been configured yet.
uint32_t uwrit_fee_bps() {
   sysio::uwrit::uwconfig_t cfg(reserve::UWRIT_ACCOUNT);
   return cfg.get_or_default(sysio::uwrit::uw_config{}).fee_bps;
}

/// Stage-2 governance dial: the share of each fee's REWARDS POOL routed to the
/// `sysio` emissions treasury. Read fresh per settlement so a `setconfig` takes
/// effect immediately; defaults to 0 (the whole pool is allocated to the
/// batch-operator distribution and no fee leaves custody) until configured.
uint32_t fee_emissions_share_bps(name self) {
   reserve::reservcfg_t cfg(self);
   return cfg.get_or_default(reserve::reserve_config{}).fee_emissions_share_bps;
}

/// Credit a reserve's owner-fee revenue — the claimable balance AND the
/// monotonic audit total — from inside an open `modify` lambda. No-op at zero,
/// so a fee-free reserve costs nothing.
void accrue_owner_fee(reserve::reserve_row& row, uint64_t amount) {
   if (amount == 0) return;
   add_capped_u64(row.owner_fee_accrued,  amount);
   add_capped_u64(row.owner_fee_lifetime, amount);
}

reserve::reserve_key make_key(sysio::slug_name chain_code,
                              sysio::slug_name token_code,
                              sysio::slug_name reserve_code) {
   return reserve::reserve_key{chain_code, token_code, reserve_code};
}

/// Reserve custody is denominated in WIRE (9 decimals) — the emissions
/// treasury symbol (`sysio.system/src/emissions.cpp:42`,
/// `sysio.epoch.cpp:38`). Deliberately NOT opreg's `CORE_SYM` (SYS, 4):
/// operator collateral and reserve custody are independent surfaces with
/// different symbols; mixing them would silently corrupt custody.
constexpr sysio::symbol WIRE_SYMBOL{"WIRE", 9};

/// Resolve a `chain_code` to its `ChainKind` via the `sysio.chains`
/// registry (mirrors `sysio.uwrit`'s helper of the same name). Returns
/// `std::nullopt` when the chain is unregistered.
std::optional<opp::types::ChainKind> chain_kind_for_code(sysio::slug_name chain_code) {
   sysio::chains::chains_t tbl(reserve::CHAINS_ACCOUNT);
   sysio::chains::chain_key pk{chain_code};
   if (!tbl.contains(pk)) return std::nullopt;
   return tbl.get(pk).kind;
}

/// Soft-gate never-throw msgch handlers before they can emit a queueout to an
/// unregistered destination chain. `sysio.msgch::queueout` fails loudly for
/// direct callers, but dispatch callbacks must log-and-skip instead of
/// aborting the consensus-tipping delivery transaction.
bool registered_chain_or_skip(sysio::slug_name chain_code, const char* handler) {
   if (chain_kind_for_code(chain_code).has_value()) return true;
   sysio::print(handler, ": chain_code is not registered; skipping\n");
   return false;
}

/// Reconstruct a `sysio::public_key` variant from the raw creator-key
/// bytes carried on a ReserveCreate attestation. EVM chains require the
/// 33-byte compressed secp256k1 key in `creator_pub_key` (the EM variant,
/// index 3); SVM chains use the 32-byte ed25519 key (ED variant, index 4),
/// falling back to `creator_addr.address` (which on Solana IS the signer
/// pubkey) when `creator_pub_key` was left empty. Returns `std::nullopt`
/// on any size/kind mismatch — callers treat that as "creator not linkable"
/// (never throw; this runs inside the msgch dispatch chain).
std::optional<sysio::public_key> pubkey_from_raw(opp::types::ChainKind   kind,
                                                  const std::vector<char>& pub_bytes,
                                                  const std::vector<char>& addr_bytes) {
   using opp::types::ChainKind;
   if (kind == ChainKind::CHAIN_KIND_EVM) {
      if (pub_bytes.size() != 33) return std::nullopt;
      using em_t = std::variant_alternative_t<3, sysio::public_key>;
      em_t em{};
      std::memcpy(em.data(), pub_bytes.data(), pub_bytes.size());
      return sysio::public_key{std::in_place_index<3>, em};
   }
   if (kind == ChainKind::CHAIN_KIND_SVM) {
      const std::vector<char>& src = pub_bytes.size() == 32 ? pub_bytes : addr_bytes;
      if (src.size() != 32) return std::nullopt;
      using ed_t = std::variant_alternative_t<4, sysio::public_key>;
      ed_t ed{};
      std::memcpy(ed.data(), src.data(), src.size());
      return sysio::public_key{std::in_place_index<4>, ed};
   }
   return std::nullopt;
}

/// Encode + queue a depot→outpost attestation back to the reserve's owning
/// chain. Mirrors `sysio.uwrit::emit_swap_remit` / `emit_swap_revert`:
///
///   * `zpp::bits::no_size{}` — raw protobuf bytes for the outpost decoder
///     (the default `zpp::bits::data_out` form prepends a 4-byte LE length
///     prefix that corrupts the first field tag on the receiving side).
///   * The destination `chain_code` is the reserve's `chain_code.value`
///     itself (per the v6 convention recorded in `sysio.msgch.hpp`:
///     "the outpost id IS the chain's slug_name value").
template <typename ProtoMessage>
void queue_attestation_out(name self,
                           sysio::slug_name owning_chain,
                           opp::types::AttestationType attest_type,
                           const ProtoMessage& message) {
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(message);

   action(
      permission_level{self, "active"_n},
      reserve::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(owning_chain.value, attest_type, encoded)
   ).send();
}

/// Route the NETWORK COMPONENT of a collected WIRE swap fee to its three
/// destinations. The reserve-owner shares carried in `fee` are NOT routed here —
/// the settlement actions accrue those to their own reserve rows before calling
/// this, so this function only ever moves the network fee's own split:
///   * the underwriter share accrues to `underwriter`'s `uwfees` row, payable to
///     that account on its own `claimuwfee` call — stays in custody;
///   * the rewards share accrues to the singleton `rewards_bucket`, swept by
///     `drainrewards` into `sysio.system::payepoch` and paid to batch operators
///     — stays in custody;
///   * the emissions share (zero unless `reserve_config` sets one) is
///     TRANSFERRED to the `sysio` treasury — the only part that leaves custody.
/// No-op when there is no fee.
///
/// At the default config the emissions share is 0, so everything THIS function
/// routes moves into two earmarked accumulators in the SAME custody and the call
/// never changes `token_balance`. With a non-zero share, custody drops by exactly
/// that share. Note the total `wire_fee::fee` is larger than what is routed here
/// whenever a participating reserve charges an owner fee: at the default dial the
/// whole fee ends up spread across the owner accrual(s) the caller already made,
/// `uwfees`, and `rewards_bucket` — not wholly into the two accumulators below.
///
/// A fee with no winning underwriter (a revert refund) passes an unset
/// `underwriter` together with a zero underwriter share. Should a caller ever
/// pair an unset account with a non-zero share, the share falls through to the
/// rewards bucket rather than stranding WIRE in custody with no claimant.
void route_wire_fee(name self, const opp::amm::wire_fee& fee, name underwriter) {
   uint64_t reward_share = fee.reward_share;

   if (fee.underwriter_share > 0 && underwriter.value != 0) {
      reserve::uwfees_t uwf(self);
      reserve::uw_fee_key key{underwriter};
      auto it = uwf.find(key);
      if (it == uwf.end()) {
         uwf.emplace(ram_payer, key, reserve::uw_fee_row{
            .underwriter      = underwriter,
            .balance          = fee.underwriter_share,
            .lifetime_accrued = fee.underwriter_share,
            .lifetime_claimed = 0,
         });
      } else {
         uwf.modify(ram_payer, key, [&](auto& row) {
            add_capped_u64(row.balance,          fee.underwriter_share);
            add_capped_u64(row.lifetime_accrued, fee.underwriter_share);
         });
      }
   } else {
      reward_share += fee.underwriter_share;
   }

   if (reward_share > 0) {
      reserve::rewardbkt_t bkt(self);
      auto rb = bkt.get_or_default(reserve::rewards_bucket{});
      add_capped_u64(rb.balance,          reward_share);
      add_capped_u64(rb.lifetime_accrued, reward_share);
      bkt.set(rb, ram_payer);
   }

   // The ONLY part of a fee that leaves this contract's custody, and only when
   // governance has configured a non-zero emissions share.
   if (fee.emissions_share > 0) {
      action(
         permission_level{self, "active"_n},
         reserve::TOKEN_ACCOUNT, "transfer"_n,
         std::make_tuple(self, reserve::TREASURY_ACCOUNT,
            asset(static_cast<int64_t>(fee.emissions_share), WIRE_SYMBOL),
            std::string("sysio.reserv::swap fee -> emissions"))
      ).send();
   }
}

} // namespace

void reserve::regreserve(sysio::slug_name chain_code,
                          sysio::slug_name token_code,
                          sysio::slug_name reserve_code,
                          std::string     name,
                          std::string     description,
                          uint64_t        initial_chain_amount,
                          uint64_t        initial_wire_amount,
                          uint32_t        source_token_precision,
                          uint32_t        connector_weight_bps,
                          bool            is_private,
                          sysio::name     owner) {
   require_priv_caller();
   sysio::check(is_bootstrap_window(),
                "regreserve is bootstrap-window only; post-bootstrap reserves go through create_reserve");
   sysio::check(source_token_precision <= WIRE_PRECISION,
                "source_token_precision exceeds the depot frame (9) — the outpost must downscale to min(native, 9)");
   sysio::check(connector_weight_bps > 0 && connector_weight_bps <= MAX_CONNECTOR_WEIGHT_BPS,
                "connector_weight_bps must be in (0, 10000) — both pool-side weights must be positive");
   sysio::check(initial_chain_amount > 0 && initial_wire_amount > 0,
                "bootstrap reserve must seed both chain_amount and wire_amount > 0");
   sysio::check(!is_private || owner != sysio::name{},
                "a private bootstrap reserve must name an owner");
   // Both strings persist into a `sysio`-billed row -- bound them before emplace.
   opp::registry::check_metadata(name, description, "sysio.reserv");

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   sysio::check(tbl.find(pk) == tbl.end(), "reserve already registered");

   // Real-WIRE backing: drain the seed from the `sysio` emissions treasury
   // into this contract's custody so `reserve_wire_amount` is never an
   // unbacked accounting number. This contract is privileged, so the
   // inline action may carry the treasury's authorization.
   action(
      permission_level{TREASURY_ACCOUNT, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(TREASURY_ACCOUNT, get_self(),
         asset(static_cast<int64_t>(initial_wire_amount), WIRE_SYMBOL),
         std::string("sysio.reserv::regreserve bootstrap WIRE backing"))
   ).send();

   const auto now = current_time_ms();
   tbl.emplace(ram_payer, pk, reserve_row{
      .chain_code             = chain_code,
      .token_code             = token_code,
      .reserve_code           = reserve_code,
      .name                   = std::move(name),
      .description            = std::move(description),
      .status                 = opp::types::RESERVE_STATUS_ACTIVE,
      .reserve_chain_amount   = initial_chain_amount,
      .reserve_wire_amount    = initial_wire_amount,
      .source_token_precision = source_token_precision,
      .connector_weight_bps   = connector_weight_bps,
      .creator_addr           = {},
      .requested_wire_amount  = initial_wire_amount,
      .external_token_amount  = initial_chain_amount,
      .registered_at_ms       = now,
      .activated_at_ms        = now,
      .cancelled_at_ms        = 0,
      .is_private             = is_private,
      .owner                  = owner,
      .creator_pub_key        = {},
   });
}

void reserve::oncrtreserve(sysio::slug_name       chain_code,
                            sysio::slug_name       token_code,
                            sysio::slug_name       reserve_code,
                            std::string           name,
                            std::string           description,
                            uint64_t              external_token_amount,
                            uint64_t              requested_wire_amount,
                            uint32_t              source_token_precision,
                            uint32_t              connector_weight_bps,
                            opp::types::ChainKind creator_chain_kind,
                            std::vector<char>     creator_chain_addr,
                            bool                  is_private,
                            std::vector<char>     creator_pub_key) {
   require_auth(MSGCH_ACCOUNT);
   if (!registered_chain_or_skip(chain_code, "oncrtreserve")) return;

   // Soft-validate; silent skip per feedback_opp_handlers_never_throw.
   if (connector_weight_bps == 0 || connector_weight_bps > MAX_CONNECTOR_WEIGHT_BPS) {
      sysio::print("oncrtreserve: bad connector_weight_bps; skipping\n");
      return;
   }
   // An invalid amount — a zero deposit / requested amount, OR an inbound
   // TokenAmount that sysio.msgch clamped to 0 because it was negative or out of
   // asset range (WSA-028) — cannot create a reserve. It must NOT be silently
   // dropped: the creator's outpost escrow still has to be released. Route it into
   // the SAME cancel/refund path as an unlinked creator below (insert a CANCELLED
   // row + queue RESERVE_CREATE_CANCELLED), idempotently.
   const bool invalid_amount = (external_token_amount == 0 || requested_wire_amount == 0);
   // Same `sysio`-billed metadata bound the privileged registrations enforce with
   // `check_metadata`, asked the non-throwing way: this handler must never abort, so an
   // over-bound string joins the reject/refund path below rather than reverting dispatch.
   const bool oversized_metadata = opp::registry::metadata_exceeds_bounds(name, description);
   // The outpost downscales to min(native, 9) at its boundary, so a
   // source_token_precision above the depot frame means a malformed attestation.
   if (source_token_precision > WIRE_PRECISION) {
      sysio::print("oncrtreserve: source_token_precision exceeds depot frame (9); skipping\n");
      return;
   }

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   // Existence guard, status-aware. A live row (PENDING/ACTIVE) — or any
   // non-CANCELLED row — is immutable here; skip idempotently. A CANCELLED
   // row (a prior no-link rejection or an outpost-side cancel) is *reclaimable*
   // by a later, properly-linked creator: fall through so the link probe below
   // decides whether to overwrite it. Without this carve-out a CANCELLED row
   // would permanently burn the (chain, token, reserve_code) identity for its
   // rightful owner (namespace squatting), since no path ever erases a row.
   auto existing = tbl.find(pk);
   const bool reclaimable_cancelled =
      existing != tbl.end() &&
      existing->status == opp::types::RESERVE_STATUS_CANCELLED;
   if (existing != tbl.end() && !reclaimable_cancelled) {
      sysio::print("oncrtreserve: reserve already exists; skipping\n");
      return;
   }

   opp::types::ChainAddress creator;
   creator.kind    = creator_chain_kind;
   creator.address = std::move(creator_chain_addr);

   // Create gating: the creator must already be authex-linked to a WIRE
   // account ("the only requirement to create a reserve"). Reconstruct the
   // creator's key variant and probe `sysio.authex::links.bypubkey`. On
   // any failure — malformed key bytes, no link, an invalid amount, OR
   // over-bound metadata — reject by inserting a
   // CANCELLED row (for refund idempotency) and queueing
   // RESERVE_CREATE_CANCELLED so the outpost refunds the creator's escrow.
   // The CANCELLED row does NOT permanently burn the identity: a later,
   // properly-linked creator reclaims it via the reclaim branch below
   // (prevents namespace squatting). Never throws.
   std::vector<char> canonical_creator_key;
   {
      auto pk_variant = pubkey_from_raw(creator_chain_kind, creator_pub_key, creator.address);
      bool linked = false;
      if (pk_variant) {
         sysio::authex::links_t links(AUTHEX_ACCOUNT);
         auto idx = links.get_index<"bypubkey"_n>();
         linked = idx.find(sysio::pubkey_to_checksum256(*pk_variant)) != idx.end();
         if (linked) {
            canonical_creator_key = sysio::pubkey_to_bytes(*pk_variant);
         }
      }
      if (!linked || invalid_amount || oversized_metadata) {
         // A CANCELLED row already standing means this is a re-relay of the same
         // rejected create (an unlinked squatter OR an invalid amount). Leave it
         // and do NOT queue a second refund — the refund was queued when the row
         // was first inserted, and the outpost refunds per (chain,token,reserve_code).
         // The row stays reclaimable by a future linked creator with a valid amount.
         if (reclaimable_cancelled) {
            sysio::print("oncrtreserve: re-relay of an already-CANCELLED reserve; "
                         "leaving it (no double refund)\n");
            return;
         }
         sysio::print("oncrtreserve: rejecting with RESERVE_CREATE_CANCELLED "
                      "(invalid amount, over-bound metadata, or unlinked / malformed creator key)\n");
         const auto now = current_time_ms();
         tbl.emplace(ram_payer, pk, reserve_row{
            .chain_code             = chain_code,
            .token_code             = token_code,
            .reserve_code           = reserve_code,
            // The tombstone is itself a `sysio`-billed row, so over-bound strings are NOT
            // carried onto it — storing them verbatim would persist exactly the state the
            // bound exists to prevent. Only THAT rejection reason substitutes the marker; a
            // row rejected for an unlinked creator or an invalid amount keeps its (already
            // in-bounds) metadata. The creator's originals stay in the inbound OPP envelope
            // artifact either way.
            .name                   = oversized_metadata
                                       ? std::string(opp::registry::rejected_label)
                                       : std::move(name),
            .description            = oversized_metadata ? std::string{} : std::move(description),
            .status                 = opp::types::RESERVE_STATUS_CANCELLED,
            .reserve_chain_amount   = 0,
            .reserve_wire_amount    = 0,
            .source_token_precision = source_token_precision,
            .connector_weight_bps   = connector_weight_bps,
            .creator_addr           = std::move(creator),
            .requested_wire_amount  = requested_wire_amount,
            .external_token_amount  = external_token_amount,
            .registered_at_ms       = now,
            .activated_at_ms        = 0,
            .cancelled_at_ms        = now,
            .is_private             = is_private,
            .owner                  = {},
            .creator_pub_key        = {},
         });
         opp::attestations::ReserveCreateCancelled cancelled;
         cancelled.chain_code   = chain_code.value;
         cancelled.token_code   = token_code.value;
         cancelled.reserve_code = reserve_code.value;
         queue_attestation_out(get_self(), chain_code,
                               opp::types::AttestationType::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED,
                               cancelled);
         return;
      }
   }

   const auto now = current_time_ms();
   reserve_row fresh{
      .chain_code             = chain_code,
      .token_code             = token_code,
      .reserve_code           = reserve_code,
      .name                   = std::move(name),
      .description            = std::move(description),
      .status                 = opp::types::RESERVE_STATUS_PENDING,
      .reserve_chain_amount   = external_token_amount,
      .reserve_wire_amount    = 0,
      .source_token_precision = source_token_precision,
      .connector_weight_bps   = connector_weight_bps,
      .creator_addr           = std::move(creator),
      .requested_wire_amount  = requested_wire_amount,
      .external_token_amount  = external_token_amount,
      .registered_at_ms       = now,
      .activated_at_ms        = 0,
      .cancelled_at_ms        = 0,
      .is_private             = is_private,
      .owner                  = {},
      .creator_pub_key        = std::move(canonical_creator_key),
   };
   if (reclaimable_cancelled) {
      // A properly-linked creator reclaims a previously-CANCELLED identity:
      // overwrite the dead row in place (re-indexing `bystatus`
      // CANCELLED → PENDING). Every field is reset to this create — this is
      // what makes the slot non-squattable.
      tbl.modify(ram_payer, pk, [&](auto& row) { row = std::move(fresh); });
   } else {
      tbl.emplace(ram_payer, pk, std::move(fresh));
   }
}

void reserve::matchreserve(sysio::slug_name chain_code,
                            sysio::slug_name token_code,
                            sysio::slug_name reserve_code,
                            name            matcher,
                            uint64_t        wire_amount) {
   require_auth(matcher);

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "matchreserve: reserve not found");
   sysio::check(it->status == opp::types::RESERVE_STATUS_PENDING,
                "matchreserve: reserve is not PENDING");
   sysio::check(wire_amount == it->requested_wire_amount,
                "matchreserve: wire_amount must equal requested_wire_amount exactly");

   // Match gating: the matcher MUST be the WIRE account authex-linked to
   // the reserve's creator. Resolve the matcher's link for the reserve
   // chain's kind and compare its canonical key bytes against the creator
   // pubkey normalized at create time. (recordlink permits one key →
   // many accounts; ANY account linked to the creator's key may match —
   // that is "the linked WIRE account".)
   sysio::check(!it->creator_pub_key.empty(),
                "matchreserve: reserve carries no creator pubkey (not matchable)");
   auto kind_opt = chain_kind_for_code(chain_code);
   sysio::check(kind_opt.has_value(),
                "matchreserve: reserve chain not registered in sysio.chains");
   {
      sysio::authex::links_t links(AUTHEX_ACCOUNT);
      auto idx = links.get_index<"bynamechain"_n>();
      auto lit = idx.find(sysio::to_namechain_key(matcher, *kind_opt));
      sysio::check(lit != idx.end(),
                   "matchreserve: matcher has no authex link for the reserve's chain");
      sysio::check(sysio::pubkey_to_bytes(lit->pub_key) == it->creator_pub_key,
                   "matchreserve: matcher is not the authex-linked account of the reserve's creator");
   }

   // The match IS a WIRE deposit: take REAL custody of the WIRE side so
   // `reserve_wire_amount` is backed the moment the reserve activates.
   action(
      permission_level{matcher, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(matcher, get_self(),
         asset(static_cast<int64_t>(wire_amount), WIRE_SYMBOL),
         std::string("sysio.reserv::matchreserve WIRE escrow"))
   ).send();

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.status              = opp::types::RESERVE_STATUS_ACTIVE;
      row.reserve_wire_amount = wire_amount;
      row.activated_at_ms     = current_time_ms();
      row.owner               = matcher;
   });

   // Reserve is now ACTIVE on the depot. Notify the owning outpost so its
   // local reserve record can flip to ACTIVE and become usable for swap
   // routing. The destination `chain_code` is the reserve's `chain_code`
   // (per the v6 `sysio.msgch::queueout` convention — the outpost id is
   // the chain slug_name's packed uint64 value).
   opp::attestations::ReserveReady ready;
   ready.chain_code   = chain_code.value;
   ready.token_code   = token_code.value;
   ready.reserve_code = reserve_code.value;
   queue_attestation_out(get_self(), chain_code,
                         opp::types::AttestationType::ATTESTATION_TYPE_RESERVE_READY,
                         ready);
}

void reserve::oncnclrsv(sysio::slug_name       chain_code,
                         sysio::slug_name       token_code,
                         sysio::slug_name       reserve_code,
                         opp::types::ChainKind creator_chain_kind,
                         std::vector<char>     creator_chain_addr) {
   require_auth(MSGCH_ACCOUNT);
   if (!registered_chain_or_skip(chain_code, "oncnclrsv")) return;

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   if (it == tbl.end()) {
      sysio::print("oncnclrsv: reserve not found; silently skipping\n");
      return;
   }

   if (it->status != opp::types::RESERVE_STATUS_PENDING) {
      sysio::print("oncnclrsv: status != PENDING; race lost, silent no-op\n");
      return;
   }

   const bool addr_matches =
      it->creator_addr.kind    == creator_chain_kind &&
      it->creator_addr.address == creator_chain_addr;
   if (!addr_matches) {
      sysio::print("oncnclrsv: creator_addr mismatch; silently skipping\n");
      return;
   }

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.status          = opp::types::RESERVE_STATUS_CANCELLED;
      row.cancelled_at_ms = current_time_ms();
   });

   // Race won — depot accepted the cancel before any matchreserve. Notify
   // the outpost so it refunds the creator's `external_token_amount`. The
   // destination `chain_code` is the reserve's owning `chain_code`. Per
   // `feedback_opp_handlers_never_throw.md` this handler still cannot
   // throw; the early chain-registration guard and the remaining
   // soft-validation checks must all pass before queueout is sent.
   opp::attestations::ReserveCreateCancelled cancelled;
   cancelled.chain_code   = chain_code.value;
   cancelled.token_code   = token_code.value;
   cancelled.reserve_code = reserve_code.value;
   queue_attestation_out(get_self(), chain_code,
                         opp::types::AttestationType::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED,
                         cancelled);
}

uint64_t reserve::swapquote(sysio::slug_name from_chain_code,
                             sysio::slug_name from_token_code,
                             sysio::slug_name from_reserve_code,
                             uint64_t        from_amount,
                             sysio::slug_name to_chain_code,
                             sysio::slug_name to_token_code,
                             sysio::slug_name to_reserve_code) {
   if (from_amount == 0) return 0;

   const bool src_is_wire = (from_token_code == WIRE_TOKEN);
   const bool dst_is_wire = (to_token_code   == WIRE_TOKEN);
   if (src_is_wire && dst_is_wire) return from_amount; // WIRE->WIRE is a plain transfer

   reserves_t tbl(get_self());

   // Resolve only the non-WIRE side(s); a WIRE endpoint has no token/WIRE pool
   // (the depot IS the WIRE side). Any required reserve missing or not ACTIVE
   // yields a 0 quote.
   uint64_t src_chain = 0, src_wire = 0; uint32_t src_cw = 0, src_fee_bps = 0;
   if (!src_is_wire) {
      auto it = tbl.find(make_key(from_chain_code, from_token_code, from_reserve_code));
      if (it == tbl.end() || it->status != opp::types::RESERVE_STATUS_ACTIVE) return 0;
      src_chain   = it->reserve_chain_amount;
      src_wire    = it->reserve_wire_amount;
      src_cw      = it->connector_weight_bps;
      src_fee_bps = it->owner_fee_bps;
   }
   uint64_t dst_chain = 0, dst_wire = 0; uint32_t dst_cw = 0, dst_fee_bps = 0;
   if (!dst_is_wire) {
      auto it = tbl.find(make_key(to_chain_code, to_token_code, to_reserve_code));
      if (it == tbl.end() || it->status != opp::types::RESERVE_STATUS_ACTIVE) return 0;
      dst_chain   = it->reserve_chain_amount;
      dst_wire    = it->reserve_wire_amount;
      dst_cw      = it->connector_weight_bps;
      dst_fee_bps = it->owner_fee_bps;
   }

   // Each participating reserve's owner fee rides the quote alongside the
   // network fee, so the quote prices exactly what settlement will charge.
   return opp::amm::quote_swap(src_is_wire, src_chain, src_wire, src_cw,
                               dst_is_wire, dst_chain, dst_wire, dst_cw,
                               from_amount, uwrit_fee_bps(),
                               src_fee_bps, dst_fee_bps);
}

uint64_t reserve::rewardbal() {
   rewardbkt_t bkt(get_self());
   return bkt.get_or_default(rewards_bucket{}).balance;
}

void reserve::drainrewards(int64_t amount) {
   // Only the system treasury (where sysio.system::payepoch runs) may sweep the
   // rewards bucket. The swept WIRE is allocated exclusively to the
   // batch-operator distribution at the next pay-epoch — producers are not paid
   // out of swap fees. payepoch pays only eligible shares; what it skips stays in
   // the treasury.
   require_auth(TREASURY_ACCOUNT);

   // Internal treasury sweep: a non-positive amount means the caller's
   // integration logic is wrong (payepoch only calls with a positive amount),
   // so fail loudly rather than silently no-op.
   sysio::check(amount > 0, "drainrewards: amount must be positive");

   const uint64_t req = static_cast<uint64_t>(amount);

   rewardbkt_t bkt(get_self());
   auto rb = bkt.get_or_default(rewards_bucket{});
   sysio::check(req <= rb.balance, "drainrewards: amount exceeds rewards bucket balance");

   // Drop the accounting balance first, then move the backing WIRE out of
   // custody. lifetime_accrued is a cumulative audit total — never decremented.
   rb.balance -= req;
   bkt.set(rb, ram_payer);

   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), TREASURY_ACCOUNT,
         asset(static_cast<int64_t>(req), WIRE_SYMBOL),
         std::string("sysio.reserv::swap-fee rewards -> emissions payepoch"))
   ).send();
}

void reserve::debit(sysio::slug_name chain_code,
                     sysio::slug_name token_code,
                     sysio::slug_name reserve_code,
                     uint64_t        amount) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(amount > 0, "amount must be positive");

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "debit: reserve not found");
   sysio::check(it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "debit: reserve not ACTIVE");
   sysio::check(it->reserve_chain_amount >= amount,
                "insufficient reserve_chain_amount for SWAP_REMIT debit");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.reserve_chain_amount -= amount;
   });
}

// onreject was removed — no SwapRejected attestation exists (every depot-initiated
// REMIT is paid by the destination outpost; reserves need no rejection reconciliation).

// onreward was removed: the v6 STAKING_REWARD path credits the per-staker reward to
// sysio.dclaim directly (already WIRE-denominated), so there is no reserve leg.

// ---------------------------------------------------------------------------
//  Emit-time swap settlement (auth = sysio.uwrit)
//
//  All `check()`s below are defense-in-depth: `sysio.uwrit::try_select_winner`
//  pre-validates the same conditions against its reserve mirror BEFORE
//  sending these inline actions, so within the single race-resolution
//  transaction they are unreachable. A check firing here therefore signals
//  a depot bug (mirror drift), not a runtime condition — halting that
//  transaction is the correct response.
// ---------------------------------------------------------------------------

void reserve::applyswap(sysio::slug_name src_chain_code,
                         sysio::slug_name src_token_code,
                         sysio::slug_name src_reserve_code,
                         uint64_t        src_amount,
                         sysio::slug_name dst_chain_code,
                         sysio::slug_name dst_token_code,
                         sysio::slug_name dst_reserve_code,
                         uint64_t        dst_amount,
                         sysio::name     underwriter) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(src_amount > 0 && dst_amount > 0, "applyswap: amounts must be positive");

   reserves_t tbl(get_self());
   auto src_pk = make_key(src_chain_code, src_token_code, src_reserve_code);
   auto dst_pk = make_key(dst_chain_code, dst_token_code, dst_reserve_code);
   auto src_it = tbl.find(src_pk);
   sysio::check(src_it != tbl.end(), "applyswap: source reserve not found");
   sysio::check(src_it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "applyswap: source reserve not ACTIVE");
   auto dst_it = tbl.find(dst_pk);
   sysio::check(dst_it != tbl.end(), "applyswap: destination reserve not found");
   sysio::check(dst_it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "applyswap: destination reserve not ACTIVE");

   // The gross WIRE intermediate is derived from the PRE-mutation source row on
   // the weighted curve (the source reserve's own `connector_weight_bps`) — the
   // same definition `sysio.uwrit::swap_quote` uses, so the depot's books and
   // its quotes share one curve. The fee is then taken OUT of this WIRE leg: the
   // source side gives up the full gross WIRE and only `net` continues to the
   // destination side. The TOTAL `fee` spreads across BOTH reserves' owner
   // accruals (made in the modifies below), the winning underwriter's accrual,
   // the batch-operator rewards bucket, and — only when
   // `fee_emissions_share_bps` is set — a transfer of that share to `sysio`.
   const uint64_t w_gross = opp::amm::token_to_wire(src_it->reserve_chain_amount,
                                                    src_it->reserve_wire_amount,
                                                    src_it->connector_weight_bps,
                                                    src_amount);
   sysio::check(w_gross > 0, "applyswap: WIRE intermediate is zero");
   // BOTH reserves supply liquidity for this swap, so both charge their own
   // owner fee on the same WIRE leg — on top of the network fee (WIRE-281).
   const auto fee = opp::amm::split_wire_fee(w_gross, uwrit_fee_bps(), FEE_UNDERWRITER_SHARE_BPS,
                                                fee_emissions_share_bps(get_self()),
                                                src_it->owner_fee_bps, dst_it->owner_fee_bps);
   // SEC-26 / WSA-042 settlement backstop: a zero post-fee WIRE leg credits no
   // WIRE to the destination reserve below while still debiting its chain side
   // — draining it at an arbitrary price. This is a LIVE path, not unreachable
   // defense-in-depth: the caps bound each rate INDEPENDENTLY, so a network fee
   // at `sysio.uwrit::MAX_FEE_BPS` (9999) plus either reserve's owner fee at
   // MIN_OWNER_FEE_BPS (1) already totals 100%. Such a combination is an
   // intentionally rejected configuration — refuse the swap here rather than
   // settle it at an arbitrary price.
   sysio::check(fee.net > 0, "applyswap: zero post-fee WIRE would credit no destination liquidity");
   sysio::check(src_it->reserve_wire_amount >= w_gross,
                "applyswap: insufficient source reserve WIRE for intermediate");
   // WNS-02 settlement bound: the destination side may never give up more token
   // than its own curve produces for the post-fee WIRE it is about to receive.
   // `sysio.uwrit` derives `dst_amount` from exactly this expression (via
   // `swap_quote` -> `opp::amm::quote_swap`) against the same pre-mutation rows
   // in the same transaction, so equality holds and this cannot fire on the
   // live path. It exists so the reserve is self-defending: `dst_amount` arrives
   // as a caller-supplied parameter, and the vulnerability this replaces was
   // precisely a caller-chosen amount being paid out verbatim. A reserve that
   // trusts its caller for a payout size has no floor of its own.
   const uint64_t curve_out = opp::amm::wire_to_token(dst_it->reserve_wire_amount,
                                                      dst_it->reserve_chain_amount,
                                                      dst_it->connector_weight_bps,
                                                      fee.net);
   sysio::check(dst_amount <= curve_out,
                "applyswap: destination amount exceeds the curve output for the post-fee WIRE");
   sysio::check(dst_it->reserve_chain_amount >= dst_amount,
                "applyswap: insufficient destination reserve balance");

   tbl.modify(ram_payer, src_pk, [&](auto& row) {
      add_capped_u64(row.reserve_chain_amount, src_amount);
      row.reserve_wire_amount  -= w_gross;
      accrue_owner_fee(row, fee.src_reserve_share);
   });
   // Same-row swaps (identical triples) compose correctly: the second
   // modify reads the post-first-modify state. The destination receives only
   // the post-fee WIRE.
   tbl.modify(ram_payer, dst_pk, [&](auto& row) {
      add_capped_u64(row.reserve_wire_amount, fee.net);
      row.reserve_chain_amount -= dst_amount;
      accrue_owner_fee(row, fee.dst_reserve_share);
   });

   // Route the NETWORK component (the two owner shares already accrued to their
   // reserve rows above): underwriter half to `uwfees`, rewards half to
   // `rewards_bucket` — both custody-internal — except any configured
   // `fee_emissions_share_bps`, the only part that leaves for the treasury.
   route_wire_fee(get_self(), fee, underwriter);
}

void reserve::applyfromwire(sysio::slug_name dst_chain_code,
                             sysio::slug_name dst_token_code,
                             sysio::slug_name dst_reserve_code,
                             uint64_t        wire_in,
                             uint64_t        dst_amount,
                             sysio::name     underwriter) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(wire_in > 0 && dst_amount > 0, "applyfromwire: amounts must be positive");

   reserves_t tbl(get_self());
   auto pk = make_key(dst_chain_code, dst_token_code, dst_reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "applyfromwire: destination reserve not found");
   sysio::check(it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "applyfromwire: destination reserve not ACTIVE");
   sysio::check(it->reserve_chain_amount >= dst_amount,
                "applyfromwire: insufficient destination reserve balance");

   // Fee out of the user's escrowed input WIRE: only the post-fee remainder
   // becomes destination-reserve liquidity. The TOTAL fee spreads across the
   // DESTINATION reserve's owner accrual, the winning underwriter's accrual, and
   // the rewards bucket. The full `wire_in` was escrowed in this contract at
   // `swapfromwire` time; the ONLY part that leaves here is a configured
   // `fee_emissions_share_bps` of the rewards pool. At the default-zero dial
   // nothing leaves and custody balances as net -> Σwire, fee -> the accruals.
   // The source is the depot's own WIRE — there is no source reserve — so only
   // the DESTINATION reserve charges an owner fee here.
   const auto fee = opp::amm::split_wire_fee(wire_in, uwrit_fee_bps(), FEE_UNDERWRITER_SHARE_BPS,
                                             fee_emissions_share_bps(get_self()),
                                             /*src_reserve_fee_bps*/ 0, it->owner_fee_bps);
   // SEC-26 / WSA-042 settlement backstop — see applyswap. A zero post-fee WIRE
   // leg would debit the destination reserve below while crediting zero WIRE.
   // Reachable under valid configuration — the network fee at MAX_FEE_BPS (9999)
   // plus this reserve's owner fee at MIN_OWNER_FEE_BPS (1) totals 100% — so
   // this rejects a configured combination, not an impossible one.
   sysio::check(fee.net > 0, "applyfromwire: zero post-fee WIRE would credit no destination liquidity");
   // WNS-02 settlement bound — see applyswap. The from-WIRE shape feeds the
   // user's escrowed WIRE straight into the WIRE leg, so the curve output is
   // `wire_to_token` of the post-fee remainder.
   const uint64_t curve_out = opp::amm::wire_to_token(it->reserve_wire_amount,
                                                      it->reserve_chain_amount,
                                                      it->connector_weight_bps,
                                                      fee.net);
   sysio::check(dst_amount <= curve_out,
                "applyfromwire: destination amount exceeds the curve output for the post-fee WIRE");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      add_capped_u64(row.reserve_wire_amount, fee.net);
      row.reserve_chain_amount -= dst_amount;
      accrue_owner_fee(row, fee.dst_reserve_share);
   });

   route_wire_fee(get_self(), fee, underwriter);
}

void reserve::paywire(sysio::slug_name src_chain_code,
                       sysio::slug_name src_token_code,
                       sysio::slug_name src_reserve_code,
                       uint64_t        src_amount,
                       sysio::name     recipient,
                       uint64_t        wire_out,
                       sysio::name     underwriter) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(src_amount > 0 && wire_out > 0, "paywire: amounts must be positive");
   sysio::check(is_account(recipient), "paywire: recipient account does not exist");

   reserves_t tbl(get_self());
   auto pk = make_key(src_chain_code, src_token_code, src_reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "paywire: source reserve not found");
   sysio::check(it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "paywire: source reserve not ACTIVE");

   // Swap-to-WIRE: the recipient receives `wire_out`, and the fee is charged on
   // the gross WIRE the source side produces — the same WIRE leg the quote
   // priced. The source reserve gives up `wire_out + fee`.
   const uint64_t w_gross = opp::amm::token_to_wire(it->reserve_chain_amount,
                                                    it->reserve_wire_amount,
                                                    it->connector_weight_bps,
                                                    src_amount);
   sysio::check(w_gross > 0, "paywire: WIRE leg is zero");
   // The recipient is paid in WIRE — there is no destination reserve — so only
   // the SOURCE reserve charges an owner fee here.
   const auto fee = opp::amm::split_wire_fee(w_gross, uwrit_fee_bps(), FEE_UNDERWRITER_SHARE_BPS,
                                                fee_emissions_share_bps(get_self()),
                                                it->owner_fee_bps, /*dst_reserve_fee_bps*/ 0);
   // WNS-02 settlement bound — see applyswap. For a WIRE destination the curve
   // output IS the post-fee WIRE leg, so the payout may never exceed `fee.net`.
   // `sysio.uwrit` passes exactly `fee.net` (its `swap_quote` returns the
   // post-fee WIRE for a WIRE endpoint), so equality holds on the live path.
   // `fee.net` now also nets out the source reserve owner's share, and
   // `quote_swap` charges that share through this same `split_wire_fee`, so the
   // quote and this bound still agree by construction.
   sysio::check(wire_out <= fee.net,
                "paywire: payout exceeds the post-fee WIRE the source leg produced");
   const uint64_t wire_leaving = wire_out + fee.fee;
   sysio::check(it->reserve_wire_amount >= wire_leaving,
                "paywire: insufficient source reserve WIRE for payout + fee");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      add_capped_u64(row.reserve_chain_amount, src_amount);
      row.reserve_wire_amount  -= wire_leaving;
      accrue_owner_fee(row, fee.src_reserve_share);
   });

   // `wire_out` goes to the recipient. The fee stays behind as three accruals —
   // the source reserve's owner accrual (above), the underwriter accrual, and
   // the rewards bucket — except any configured `fee_emissions_share_bps`, which
   // `route_wire_fee` transfers to the treasury. `Σ reserve_wire_amount` drops by
   // `wire_out + fee` while the token balance drops by `wire_out` plus that
   // emissions share; the difference is exactly the accruals, preserving the
   // invariant.
   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), recipient,
         asset(static_cast<int64_t>(wire_out), WIRE_SYMBOL),
         std::string("sysio.reserv::paywire swap-to-WIRE payout"))
   ).send();
   route_wire_fee(get_self(), fee, underwriter);
}

void reserve::refundwire(sysio::name recipient,
                          uint64_t   wire_amount,
                          uint32_t   revert_fee_bps) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(wire_amount > 0, "refundwire: amount must be positive");
   sysio::check(is_account(recipient), "refundwire: recipient account does not exist");

   // Caller-fault revert fee, routed through the same path as a settlement fee.
   // A revert has NO winning underwriter — no collateral was locked for a swap
   // that never settled — so the underwriter share is zero and the whole revert
   // fee becomes the rewards POOL. `fee_emissions_share_bps` then splits that
   // pool exactly as it does a settlement fee's: the whole revert fee lands in
   // the rewards bucket only under the default zero dial; a configured dial
   // diverts that share to the emissions treasury. Zero bps (no-fault refund)
   // makes both the split and the routing no-ops.
   //
   // Unlike `applyswap` / `applyfromwire`, the backstop below really IS
   // unreachable here, and for a reason specific to this path: a refund charges
   // NO reserve owner fee (the trailing rates are left at 0), so the total is
   // the network fee alone, which `sysio.uwrit::setconfig` caps at MAX_FEE_BPS
   // (9999). Floor division then leaves `net >= 1` for any positive amount.
   // There is no second rate to stack on top and reach 100%. It must hold: this
   // action is inlined from the never-throw `drainfwq` drain.
   const auto fee = opp::amm::split_wire_fee(wire_amount, revert_fee_bps,
                                             /*underwriter_share_bps*/ 0,
                                             fee_emissions_share_bps(get_self()));
   sysio::check(fee.net > 0, "refundwire: revert fee must be below 100%");

   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), recipient,
         asset(static_cast<int64_t>(fee.net), WIRE_SYMBOL),
         std::string("sysio.reserv::refundwire swap-from-WIRE refund"))
   ).send();
   route_wire_fee(get_self(), fee, /*underwriter*/ name{});
}

void reserve::setconfig(uint32_t fee_emissions_share_bps) {
   require_auth(get_self());
   sysio::check(fee_emissions_share_bps <= FEE_SPLIT_TOTAL_BPS,
                "setconfig: fee_emissions_share_bps must be <= 10000 (100% of the rewards pool)");

   reservcfg_t cfg(get_self());
   auto row = cfg.get_or_default(reserve_config{});
   row.fee_emissions_share_bps = fee_emissions_share_bps;
   cfg.set(row, ram_payer);
}

void reserve::setrsvfee(sysio::slug_name chain_code,
                         sysio::slug_name token_code,
                         sysio::slug_name reserve_code,
                         uint32_t        owner_fee_bps) {
   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "setrsvfee: reserve not found");
   sysio::check(it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "setrsvfee: reserve is not ACTIVE");
   // A bootstrap-seeded public reserve has no owner, so nobody can authorize a
   // fee on it — which is exactly the guard that stops WIRE accruing with no
   // claimant. `require_auth(name{})` would abort with a confusing message, so
   // say why first.
   sysio::check(it->owner.value != 0, "setrsvfee: reserve has no owner");
   require_auth(it->owner);

   // 0 disables the fee; anything else must clear the dust floor and stay below
   // the 99% ceiling that keeps a positive remainder on the WIRE leg.
   sysio::check(owner_fee_bps == 0 ||
                   (owner_fee_bps >= MIN_OWNER_FEE_BPS && owner_fee_bps <= MAX_OWNER_FEE_BPS),
                "setrsvfee: owner_fee_bps must be 0 or within [1, 9900]");

   tbl.modify(ram_payer, pk, [&](auto& row) { row.owner_fee_bps = owner_fee_bps; });
}

uint64_t reserve::rsvfeebal(sysio::slug_name chain_code,
                             sysio::slug_name token_code,
                             sysio::slug_name reserve_code) {
   reserves_t tbl(get_self());
   auto it = tbl.find(make_key(chain_code, token_code, reserve_code));
   return it == tbl.end() ? 0 : it->owner_fee_accrued;
}

void reserve::claimrsvfee(sysio::slug_name chain_code,
                           sysio::slug_name token_code,
                           sysio::slug_name reserve_code) {
   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "claimrsvfee: reserve not found");
   sysio::check(it->owner.value != 0, "claimrsvfee: reserve has no owner");
   require_auth(it->owner);

   const uint64_t amount = it->owner_fee_accrued;
   const name     owner  = it->owner;
   sysio::check(amount > 0, "claimrsvfee: no unclaimed balance");

   // Zero the accounting balance first, then move the backing WIRE out of
   // custody. `owner_fee_lifetime` is a cumulative audit total — never reduced.
   tbl.modify(ram_payer, pk, [&](auto& row) { row.owner_fee_accrued = 0; });

   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), owner,
         asset(static_cast<int64_t>(amount), WIRE_SYMBOL),
         std::string("sysio.reserv::reserve owner fee claim"))
   ).send();
}

uint64_t reserve::uwfeebal(sysio::name underwriter) {
   uwfees_t uwf(get_self());
   auto it = uwf.find(uw_fee_key{underwriter});
   return it == uwf.end() ? 0 : it->balance;
}

void reserve::claimuwfee(sysio::name underwriter) {
   // Only the earner may sweep their own accrual. There is no depot-side push:
   // an underwriter claims when they choose, and a dormant one costs the chain
   // nothing.
   require_auth(underwriter);

   uwfees_t uwf(get_self());
   uw_fee_key key{underwriter};
   auto it = uwf.find(key);
   sysio::check(it != uwf.end(), "claimuwfee: no accrued swap fees for this underwriter");

   const uint64_t amount = it->balance;
   sysio::check(amount > 0, "claimuwfee: no unclaimed balance");

   // Zero the accounting balance first, then move the backing WIRE out of
   // custody. The row is retained at zero so the lifetime audit totals survive.
   uwf.modify(ram_payer, key, [&](auto& row) {
      row.balance = 0;
      add_capped_u64(row.lifetime_claimed, amount);
   });

   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), underwriter,
         asset(static_cast<int64_t>(amount), WIRE_SYMBOL),
         std::string("sysio.reserv::underwriter swap-fee claim"))
   ).send();
}

} // namespace sysio
