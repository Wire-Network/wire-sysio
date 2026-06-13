#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

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

uint64_t cp_output(uint64_t reserve_src, uint64_t reserve_dst, uint64_t src_amount) {
   if (reserve_src == 0 || reserve_dst == 0 || src_amount == 0) return 0;
   uint128_t numerator   = static_cast<uint128_t>(reserve_dst) * src_amount;
   uint128_t denominator = static_cast<uint128_t>(reserve_src) + src_amount;
   uint128_t result      = numerator / denominator;
   if (result > static_cast<uint128_t>(std::numeric_limits<uint64_t>::max())) {
      return std::numeric_limits<uint64_t>::max();
   }
   return static_cast<uint64_t>(result);
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

} // namespace

void reserve::regreserve(sysio::slug_name chain_code,
                          sysio::slug_name token_code,
                          sysio::slug_name reserve_code,
                          std::string     name,
                          std::string     description,
                          uint64_t        initial_chain_amount,
                          uint64_t        initial_wire_amount,
                          uint32_t        connector_weight_bps,
                          bool            is_private,
                          sysio::name     owner) {
   require_priv_caller();
   sysio::check(is_bootstrap_window(),
                "regreserve is bootstrap-window only; post-bootstrap reserves go through create_reserve");
   sysio::check(connector_weight_bps > 0 && connector_weight_bps <= MAX_CONNECTOR_WEIGHT_BPS,
                "connector_weight_bps must be in (0, 10000]");
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
   if (external_token_amount == 0 || requested_wire_amount == 0) {
      sysio::print("oncrtreserve: zero deposit / requested amount; skipping\n");
      return;
   }

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   if (tbl.find(pk) != tbl.end()) {
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
   // CANCELLED row (idempotency + audit; mirrors the cancel path's
   // code-burned semantics) and queueing RESERVE_CREATE_CANCELLED so the
   // outpost refunds the creator's escrow. Never throws.
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
      if (!linked) {
         sysio::print("oncrtreserve: creator has no authex link (or malformed "
                      "creator key); rejecting with RESERVE_CREATE_CANCELLED\n");
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
   tbl.emplace(ram_payer, pk, reserve_row{
      .chain_code             = chain_code,
      .token_code             = token_code,
      .reserve_code           = reserve_code,
      .name                   = std::move(name),
      .description            = std::move(description),
      .status                 = opp::types::RESERVE_STATUS_PENDING,
      .reserve_chain_amount   = external_token_amount,
      .reserve_wire_amount    = 0,
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
   });
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

   reserves_t tbl(get_self());
   auto src_pk = make_key(from_chain_code, from_token_code, from_reserve_code);
   auto dst_pk = make_key(to_chain_code,   to_token_code,   to_reserve_code);
   auto src_it = tbl.find(src_pk);
   auto dst_it = tbl.find(dst_pk);
   if (src_it == tbl.end() || dst_it == tbl.end()) return 0;
   if (src_it->status != opp::types::RESERVE_STATUS_ACTIVE) return 0;
   if (dst_it->status != opp::types::RESERVE_STATUS_ACTIVE) return 0;

   uint64_t wire_intermediate = cp_output(src_it->reserve_chain_amount,
                                          src_it->reserve_wire_amount,
                                          from_amount);
   if (wire_intermediate == 0) return 0;
   return cp_output(dst_it->reserve_wire_amount, dst_it->reserve_chain_amount, wire_intermediate);
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

void reserve::onreject(checksum256       /*original_swap_remit_id*/,
                        sysio::slug_name   chain_code,
                        sysio::slug_name   token_code,
                        sysio::slug_name   reserve_code,
                        uint64_t          unremitted_amount,
                        std::vector<char> /*recipient_address*/,
                        std::string       /*reason*/) {
   require_auth(MSGCH_ACCOUNT);
   if (unremitted_amount == 0) return;

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   if (it == tbl.end()) {
      sysio::print("onreject: reserve not found; silently skipping\n");
      return;
   }
   if (it->status != opp::types::RESERVE_STATUS_ACTIVE) {
      sysio::print("onreject: reserve not ACTIVE; silently skipping\n");
      return;
   }
   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.reserve_chain_amount += unremitted_amount;
   });
}

void reserve::onreward(sysio::slug_name chain_code,
                        sysio::slug_name token_code,
                        sysio::slug_name reserve_code,
                        uint64_t        outpost_amount) {
   require_auth(MSGCH_ACCOUNT);
   if (outpost_amount == 0) return;

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   auto it = tbl.find(pk);
   if (it == tbl.end()) {
      sysio::print("onreward: reserve not found; silently skipping\n");
      return;
   }
   if (it->status != opp::types::RESERVE_STATUS_ACTIVE) {
      sysio::print("onreward: reserve not ACTIVE; silently skipping\n");
      return;
   }
   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.reserve_chain_amount += outpost_amount;
   });
}

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

   // The WIRE intermediate is derived from the PRE-mutation source row —
   // the same forward-from-source definition `sysio.uwrit::swap_quote`
   // uses, so the depot's books and its quotes share one curve.
   const uint64_t w = cp_output(src_it->reserve_chain_amount,
                                 src_it->reserve_wire_amount,
                                 src_amount);
   sysio::check(w > 0, "applyswap: WIRE intermediate is zero");
   sysio::check(src_it->reserve_wire_amount >= w,
                "applyswap: insufficient source reserve WIRE for intermediate");
   sysio::check(dst_it->reserve_chain_amount >= dst_amount,
                "applyswap: insufficient destination reserve balance");

   tbl.modify(ram_payer, src_pk, [&](auto& row) {
      row.reserve_chain_amount += src_amount;
      row.reserve_wire_amount  -= w;
   });
   // Same-row swaps (identical triples) compose correctly: the second
   // modify reads the post-first-modify state.
   tbl.modify(ram_payer, dst_pk, [&](auto& row) {
      row.reserve_wire_amount  += w;
      row.reserve_chain_amount -= dst_amount;
   });
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

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.reserve_wire_amount  += wire_in;
      row.reserve_chain_amount -= dst_amount;
   });
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
   sysio::check(it->reserve_wire_amount >= wire_out,
                "paywire: insufficient source reserve WIRE for payout");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.reserve_chain_amount += src_amount;
      row.reserve_wire_amount  -= wire_out;
   });

   // REAL WIRE leaves custody — `Σ reserve_wire_amount` and the contract's
   // token balance drop together, preserving the custody invariant.
   action(
      permission_level{get_self(), "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(get_self(), recipient,
         asset(static_cast<int64_t>(wire_out), WIRE_SYMBOL),
         std::string("sysio.reserv::paywire swap-to-WIRE payout"))
   ).send();
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
