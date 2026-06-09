#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

#include <zpp_bits.h>

#include <cstring>
#include <limits>

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
                          uint32_t        connector_weight_bps) {
   require_priv_caller();
   sysio::check(is_bootstrap_window(),
                "regreserve is bootstrap-window only; post-bootstrap reserves go through create_reserve");
   sysio::check(connector_weight_bps > 0 && connector_weight_bps <= MAX_CONNECTOR_WEIGHT_BPS,
                "connector_weight_bps must be in (0, 10000]");
   sysio::check(initial_chain_amount > 0 && initial_wire_amount > 0,
                "bootstrap reserve must seed both chain_amount and wire_amount > 0");

   reserves_t tbl(get_self());
   auto pk = make_key(chain_code, token_code, reserve_code);
   sysio::check(tbl.find(pk) == tbl.end(), "reserve already registered");

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
                            std::vector<char>     creator_chain_addr) {
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

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.status              = opp::types::RESERVE_STATUS_ACTIVE;
      row.reserve_wire_amount = wire_amount;
      row.activated_at_ms     = current_time_ms();
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

} // namespace sysio
