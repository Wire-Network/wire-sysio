#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio.opp.common/opp_table_types.hpp>
#include <sysio.opp.common/amm_math.hpp>
#include <sysio.opp.common/safe_ops.hpp>

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
/// never throw on the consensus path. The cap is unreachable for any real token amount. Delegates
/// to the shared `sysio::opp::safe::add_sat_u64` so the never-wrap rule lives in one place.
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

/// Route a collected WIRE swap fee: accrue the rewards share into the on-chain
/// `rewards_bucket` (the WIRE stays in this contract's custody, earmarked for a
/// future distribution) and transfer the emissions share back to the `sysio`
/// treasury. No-op when there is no fee. The custody invariant is preserved:
/// the rewards share moves from a reserve's WIRE side into `rewards.balance`
/// (same custody), and only the emissions share leaves as a real transfer.
void route_wire_fee(name self, const opp::amm::wire_fee& fee) {
   if (fee.reward_share > 0) {
      reserve::rewardbkt_t bkt(self);
      auto rb = bkt.get_or_default(reserve::rewards_bucket{});
      add_capped_u64(rb.balance,          fee.reward_share);
      add_capped_u64(rb.lifetime_accrued, fee.reward_share);
      bkt.set(rb, ram_payer);
   }
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
   // any failure — malformed key bytes OR no link — reject by inserting a
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
      if (!linked || invalid_amount) {
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
                      "(invalid amount or unlinked / malformed creator key)\n");
         const auto now = current_time_ms();
         tbl.emplace(ram_payer, pk, reserve_row{
            .chain_code             = chain_code,
            .token_code             = token_code,
            .reserve_code           = reserve_code,
            .name                   = std::move(name),
            .description            = std::move(description),
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
   // throw; we only reach the queueout after all soft-validation checks
   // above have silently returned, so the action is safe to send here.
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
   uint64_t src_chain = 0, src_wire = 0; uint32_t src_cw = 0;
   if (!src_is_wire) {
      auto it = tbl.find(make_key(from_chain_code, from_token_code, from_reserve_code));
      if (it == tbl.end() || it->status != opp::types::RESERVE_STATUS_ACTIVE) return 0;
      src_chain = it->reserve_chain_amount;
      src_wire  = it->reserve_wire_amount;
      src_cw    = it->connector_weight_bps;
   }
   uint64_t dst_chain = 0, dst_wire = 0; uint32_t dst_cw = 0;
   if (!dst_is_wire) {
      auto it = tbl.find(make_key(to_chain_code, to_token_code, to_reserve_code));
      if (it == tbl.end() || it->status != opp::types::RESERVE_STATUS_ACTIVE) return 0;
      dst_chain = it->reserve_chain_amount;
      dst_wire  = it->reserve_wire_amount;
      dst_cw    = it->connector_weight_bps;
   }

   return opp::amm::quote_swap(src_is_wire, src_chain, src_wire, src_cw,
                               dst_is_wire, dst_chain, dst_wire, dst_cw,
                               from_amount, uwrit_fee_bps());
}

uint64_t reserve::rewardbal() {
   rewardbkt_t bkt(get_self());
   return bkt.get_or_default(rewards_bucket{}).balance;
}

void reserve::drainrewards(int64_t amount) {
   // Only the system treasury (where sysio.system::payepoch runs) may sweep the
   // rewards bucket. The swept WIRE is redistributed to producers + batch
   // operators at the next pay-epoch.
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
                         uint64_t        dst_amount) {
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
   // source side gives up the full gross WIRE, only `net` continues to the
   // destination side, and `fee` is routed to rewards + emissions.
   const uint64_t w_gross = opp::amm::token_to_wire(src_it->reserve_chain_amount,
                                                    src_it->reserve_wire_amount,
                                                    src_it->connector_weight_bps,
                                                    src_amount);
   sysio::check(w_gross > 0, "applyswap: WIRE intermediate is zero");
   const auto fee = opp::amm::split_wire_fee(w_gross, uwrit_fee_bps(), FEE_REWARD_SHARE_BPS);
   // SEC-26 / WSA-042 settlement backstop: a zero post-fee WIRE leg credits no
   // WIRE to the destination reserve below while still debiting its chain side
   // — draining it at an arbitrary price. `net == 0` is only reachable at a
   // 100% fee, which `sysio.uwrit::setconfig` rejects (MAX_FEE_BPS), so this is
   // unreachable defense-in-depth rather than a live path.
   sysio::check(fee.net > 0, "applyswap: zero post-fee WIRE would credit no destination liquidity");
   sysio::check(src_it->reserve_wire_amount >= w_gross,
                "applyswap: insufficient source reserve WIRE for intermediate");
   sysio::check(dst_it->reserve_chain_amount >= dst_amount,
                "applyswap: insufficient destination reserve balance");

   tbl.modify(ram_payer, src_pk, [&](auto& row) {
      add_capped_u64(row.reserve_chain_amount, src_amount);
      row.reserve_wire_amount  -= w_gross;
   });
   // Same-row swaps (identical triples) compose correctly: the second
   // modify reads the post-first-modify state. The destination receives only
   // the post-fee WIRE.
   tbl.modify(ram_payer, dst_pk, [&](auto& row) {
      add_capped_u64(row.reserve_wire_amount, fee.net);
      row.reserve_chain_amount -= dst_amount;
   });

   // Route the fee (rewards half stays in custody, emissions half leaves).
   route_wire_fee(get_self(), fee);
}

void reserve::applyfromwire(sysio::slug_name dst_chain_code,
                             sysio::slug_name dst_token_code,
                             sysio::slug_name dst_reserve_code,
                             uint64_t        wire_in,
                             uint64_t        dst_amount) {
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
   // becomes destination-reserve liquidity; the fee is routed to rewards +
   // emissions. The full `wire_in` was escrowed in this contract at
   // `swapfromwire` time, so custody stays balanced (net -> Σwire, rewards ->
   // bucket, emissions -> transferred out).
   const auto fee = opp::amm::split_wire_fee(wire_in, uwrit_fee_bps(), FEE_REWARD_SHARE_BPS);
   // SEC-26 / WSA-042 settlement backstop — see applyswap. A zero post-fee WIRE
   // leg would debit the destination reserve below while crediting zero WIRE.
   // Unreachable given `sysio.uwrit::setconfig`'s MAX_FEE_BPS cap.
   sysio::check(fee.net > 0, "applyfromwire: zero post-fee WIRE would credit no destination liquidity");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      add_capped_u64(row.reserve_wire_amount, fee.net);
      row.reserve_chain_amount -= dst_amount;
   });

   route_wire_fee(get_self(), fee);
}

void reserve::paywire(sysio::slug_name src_chain_code,
                       sysio::slug_name src_token_code,
                       sysio::slug_name src_reserve_code,
                       uint64_t        src_amount,
                       sysio::name     recipient,
                       uint64_t        wire_out) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(src_amount > 0 && wire_out > 0, "paywire: amounts must be positive");
   sysio::check(is_account(recipient), "paywire: recipient account does not exist");

   reserves_t tbl(get_self());
   auto pk = make_key(src_chain_code, src_token_code, src_reserve_code);
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "paywire: source reserve not found");
   sysio::check(it->status == opp::types::RESERVE_STATUS_ACTIVE,
                "paywire: source reserve not ACTIVE");

   // Swap-to-WIRE: the recipient receives exactly `wire_out` (their target), and
   // the fee is charged on the gross WIRE the source side produces — the same
   // WIRE leg the quote priced. The source reserve gives up `wire_out + fee`
   // and keeps any surplus (when the user targeted below the post-fee quote).
   const uint64_t w_gross = opp::amm::token_to_wire(it->reserve_chain_amount,
                                                    it->reserve_wire_amount,
                                                    it->connector_weight_bps,
                                                    src_amount);
   sysio::check(w_gross > 0, "paywire: WIRE leg is zero");
   const auto fee = opp::amm::split_wire_fee(w_gross, uwrit_fee_bps(), FEE_REWARD_SHARE_BPS);
   const uint64_t wire_leaving = wire_out + fee.fee;
   sysio::check(it->reserve_wire_amount >= wire_leaving,
                "paywire: insufficient source reserve WIRE for payout + fee");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      add_capped_u64(row.reserve_chain_amount, src_amount);
      row.reserve_wire_amount  -= wire_leaving;
   });

   // REAL WIRE leaves custody to the recipient; the fee's emissions half also
   // leaves (rewards half stays in the bucket). `Σ reserve_wire_amount` and the
   // contract's token balance drop together, preserving the custody invariant.
   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), recipient,
         asset(static_cast<int64_t>(wire_out), WIRE_SYMBOL),
         std::string("sysio.reserv::paywire swap-to-WIRE payout"))
   ).send();
   route_wire_fee(get_self(), fee);
}

void reserve::refundwire(sysio::name recipient,
                          uint64_t   wire_amount) {
   require_auth(UWRIT_ACCOUNT);
   sysio::check(wire_amount > 0, "refundwire: amount must be positive");
   sysio::check(is_account(recipient), "refundwire: recipient account does not exist");

   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), recipient,
         asset(static_cast<int64_t>(wire_amount), WIRE_SYMBOL),
         std::string("sysio.reserv::refundwire swap-from-WIRE refund"))
   ).send();
}

} // namespace sysio
