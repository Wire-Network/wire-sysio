#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fc/crypto/base64.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/task/deadline.hpp>
#include <fc/variant_object.hpp>

#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

namespace sysio {

namespace {

// ── Op labels used for deadline-exceeded error messages ──────────────────
constexpr std::string_view OP_EPOCH_IN    = "deliver_outbound_envelope:epoch_in";
constexpr std::string_view OP_DISPATCH_ATTESTATIONS =
   "deliver_outbound_envelope:dispatch_attestations";
constexpr std::string_view OP_READ_LATEST = "read_inbound_envelope:get_account_info";
constexpr std::string_view OP_UW_COMMIT   = "uw_commit:commit_underwrite";

/// Anchor seed literals for the outpost program's per-epoch PDAs. Byte-exact
/// mirrors of the program's `EPOCH_DELIVERIES_SEED` / `ENVELOPE_CHUNKS_SEED`
/// constants (wire-solana `opp_states.rs`) -- these derivations MUST agree
/// with the program's `#[account(seeds = ...)]` declarations or every
/// seeds-validated call fails.
constexpr std::string_view EPOCH_DELIVERIES_SEED = "epoch_deliveries";
constexpr std::string_view ENVELOPE_CHUNKS_SEED  = "envelope_chunks";

/// The 4-byte little-endian seed encoding of a WIRE epoch index -- the exact
/// bytes the program's `epoch_index.to_le_bytes()` seed component uses.
std::vector<uint8_t> epoch_index_le_seed(uint32_t epoch_index) {
   return {static_cast<uint8_t>(epoch_index & 0xFF),
           static_cast<uint8_t>((epoch_index >> 8) & 0xFF),
           static_cast<uint8_t>((epoch_index >> 16) & 0xFF),
           static_cast<uint8_t>((epoch_index >> 24) & 0xFF)};
}

/// Derive the per-epoch `EpochDeliveries` PDA:
/// seeds `[EPOCH_DELIVERIES_SEED, epoch_index.to_le_bytes()]`.
fc::network::solana::solana_public_key
derive_epoch_deliveries_pda(const fc::network::solana::solana_public_key& program_id,
                            uint32_t                                      epoch_index) {
   return fc::network::solana::system::find_program_address(
             {std::vector<uint8_t>(EPOCH_DELIVERIES_SEED.begin(), EPOCH_DELIVERIES_SEED.end()),
              epoch_index_le_seed(epoch_index)},
             program_id)
      .first;
}

/// Derive the per-(epoch, uploader) envelope chunk-buffer PDA:
/// seeds `[ENVELOPE_CHUNKS_SEED, epoch_index.to_le_bytes(), uploader]`. The
/// uploader's pubkey is the third seed so multiple operators in the same
/// group stage their own buffers without contention.
fc::network::solana::solana_public_key
derive_envelope_chunks_pda(const fc::network::solana::solana_public_key& program_id,
                           uint32_t                                      epoch_index,
                           const fc::network::solana::solana_public_key& uploader) {
   const auto uploader_bytes = uploader.serialize();
   return fc::network::solana::system::find_program_address(
             {std::vector<uint8_t>(ENVELOPE_CHUNKS_SEED.begin(), ENVELOPE_CHUNKS_SEED.end()),
              epoch_index_le_seed(epoch_index),
              std::vector<uint8_t>(uploader_bytes.begin(), uploader_bytes.end())},
             program_id)
      .first;
}

/// Byte width the on-chain `LatestOutboundEnvelope.checksum` field decodes
/// to: `keccak256(encoded_envelope)`, written identically by both program
/// versions (standalone `opp_outpost` and integrated `liqsol_core`).
constexpr size_t LATEST_ENVELOPE_CHECKSUM_BYTES = fc::crypto::keccak256::byte_size;

/// Identifiers of the `LatestOutboundEnvelope` account + the fields the
/// inbound reader consumes. The two program versions declare the struct's
/// fields in a DIFFERENT ORDER:
///   * standalone `opp_outpost`  - {epoch_index, checksum, data, bump}
///   * integrated  `liqsol_core` - {bump, epoch_index, checksum, data}
/// so the reader decodes the WHOLE account through libfc's IDL-driven
/// `decode_account_data` (which follows the loaded IDL's declared field order
/// and verifies the Anchor discriminator) instead of hand-deriving byte
/// offsets. Historical context: hardcoded STANDALONE offsets (epoch@8)
/// decoding the INTEGRATED account produced the epoch=511 RCA - the
/// integrated layout puts `bump`=0xFF at byte 8 and `epoch_index`=1 at byte
/// 9, so a u32 read at byte 8 yields 0xFF | (1<<8) = 511.
namespace latest_envelope {
   constexpr auto account_name   = "LatestOutboundEnvelope";
   constexpr auto field_epoch    = "epoch_index";
   constexpr auto field_data     = "data";
   constexpr auto field_checksum = "checksum";
} // namespace latest_envelope

} // anonymous namespace

namespace outpost_solana_client_detail {

namespace {

/// Wrap a 32-byte address slice from `op_address.address` /
/// `depositor.address` into a `solana_public_key`. Returns nullopt
/// on the wrong chain kind or a malformed length — caller drops these
/// attestations from the remaining_accounts list (the on-chain handler
/// will log+skip them too, so no fatal failure).
std::optional<fc::network::solana::solana_public_key> sol_pubkey_from_chain_address(
   const sysio::opp::types::ChainAddress& addr) {
   if (addr.kind() != sysio::opp::types::CHAIN_KIND_SVM) return std::nullopt;
   if (addr.address().size() != 32)                         return std::nullopt;
   std::array<uint8_t, 32> bytes{};
   std::memcpy(bytes.data(), addr.address().data(), 32);
   return fc::network::solana::solana_public_key(bytes);
}

} // anonymous namespace (within outpost_solana_client_detail)

namespace {

/// Render an IDL type for diagnostics. `idl_type::to_string()` assumes the
/// per-kind payload members are populated (it dereferences them without
/// checking), so a malformed type object must be reported instead of
/// formatted - the diagnostic path is exactly where malformed inputs appear.
std::string describe_idl_type(const fc::network::solana::idl::idl_type& type) {
   if ((type.is_primitive() && !type.primitive) ||
       (type.is_defined()   && !type.defined_name) ||
       (type.is_option()    && !type.option_inner) ||
       (type.is_vec()       && !type.vec_element) ||
       (type.is_array()     && (!type.array_element || !type.array_len)) ||
       (type.is_tuple()     && !type.tuple_elements)) {
      return "<malformed idl type>";
   }
   return type.to_string();
}

} // anonymous namespace (within outpost_solana_client_detail)

/// Assert the loaded IDL declares `LatestOutboundEnvelope` with the field
/// types the inbound reader relies on. Field order is unconstrained (the
/// reader decodes through the IDL at runtime). Full contract on the header
/// declaration.
void assert_latest_envelope_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const idl::account* account = program.find_account(latest_envelope::account_name);
   FC_ASSERT(account, "IDL has no '{}' account definition", latest_envelope::account_name);

   // Anchor IDL v2 keeps the struct's fields in the `types` section; the
   // `accounts` entry carries only name + discriminator. Mirror
   // `solana_program_client::decode_account_data`'s inline-then-types fallback.
   const std::vector<idl::field>* fields = &account->fields;
   if (fields->empty()) {
      const idl::type_def* type_def = program.find_type(latest_envelope::account_name);
      FC_ASSERT(type_def && type_def->is_struct() && type_def->struct_fields,
                "IDL '{}' has no struct field definition", latest_envelope::account_name);
      fields = &(*type_def->struct_fields);
   }

   bool has_epoch = false;
   bool has_data  = false;
   for (const auto& field : *fields) {
      // Compare against the optional member directly: a malformed type object
      // (kind==primitive but no primitive value) must produce the per-field
      // diagnostic below, not `get_primitive()`'s generic throw.
      if (field.name == latest_envelope::field_epoch) {
         // The reader narrows the decoded value to uint32; a differently
         // declared width means the IDL disagrees with the on-chain struct.
         FC_ASSERT(field.type.is_primitive() && field.type.primitive == idl::primitive_type::u32,
                   "LatestOutboundEnvelope '{}' must be declared u32, got '{}'",
                   latest_envelope::field_epoch, describe_idl_type(field.type));
         has_epoch = true;
      } else if (field.name == latest_envelope::field_data) {
         // The payload must be length-prefixed `bytes` / `Vec<u8>`. A fixed
         // `[u8; N]` array would decode, but cannot represent the on-chain
         // variable-length envelope - such an IDL disagrees with both known
         // program versions, so refuse it at boot.
         const bool is_bytes = field.type.is_primitive() &&
                               field.type.primitive == idl::primitive_type::bytes;
         const bool is_vec_u8 = field.type.is_vec() && field.type.vec_element &&
                                field.type.vec_element->is_primitive() &&
                                field.type.vec_element->primitive == idl::primitive_type::u8;
         FC_ASSERT(is_bytes || is_vec_u8,
                   "LatestOutboundEnvelope '{}' must be declared bytes/Vec<u8>, got '{}'",
                   latest_envelope::field_data, describe_idl_type(field.type));
         has_data = true;
      }
   }
   FC_ASSERT(has_epoch, "LatestOutboundEnvelope IDL missing '{}' field", latest_envelope::field_epoch);
   FC_ASSERT(has_data, "LatestOutboundEnvelope IDL missing '{}' field", latest_envelope::field_data);
}

/// Keep only the candidate IDLs whose declared address matches the deployed
/// program id, so `--solana-idl-file` ORDER can never decide which same-named
/// IDL version's field order drives account decoding. Full contract on the
/// header declaration.
std::vector<fc::network::solana::idl::program>
select_program_idls_matching(std::vector<fc::network::solana::idl::program> program_idls,
                             const fc::network::solana::solana_public_key&  program_id) {
   namespace idl = fc::network::solana::idl;
   std::vector<idl::program> matching;
   size_t declared_addresses = 0;
   for (auto& candidate : program_idls) {
      if (candidate.address.empty())
         continue;
      ++declared_addresses;
      try {
         if (fc::network::solana::solana_public_key::from_base58_string(candidate.address) == program_id) {
            // Moved-from entries are only left behind when `matching` is
            // non-empty, in which case `program_idls` is never returned.
            matching.push_back(std::move(candidate));
         }
      } catch (const fc::exception&) {
         wlog("IDL '{}' declares unparseable program address '{}'; treating as non-matching",
              candidate.name, candidate.address);
      }
   }
   if (!matching.empty()) {
      if (matching.size() < program_idls.size()) {
         ilog("selected {} of {} loaded IDL(s) whose declared address matches program id {}",
              matching.size(), program_idls.size(),
              program_id.to_string(fc::yield_function_t{}));
      }
      return matching;
   }
   if (program_idls.size() == 1) {
      // Address-less stub/dev IDLs stay usable; a declared-but-mismatched
      // address is suspicious but unambiguous, so warn instead of refusing.
      if (declared_addresses > 0) {
         wlog("the single loaded IDL '{}' declares address {} which does not match the configured "
              "program id {}; using it anyway - verify --solana-idl-file matches the deployment",
              program_idls.front().name, program_idls.front().address,
              program_id.to_string(fc::yield_function_t{}));
      }
      return program_idls;
   }
   FC_ASSERT(false,
             "{} IDLs are loaded for this program name but none declares address {}; "
             "which one to trust would depend on --solana-idl-file order, and a wrong pick "
             "silently misreads accounts. Load only the IDL generated from the deployed "
             "program, or ensure it declares the deployed address",
             program_idls.size(), program_id.to_string(fc::yield_function_t{}));
}

/// Extract raw payload bytes from a decoded `bytes` (base64 string variant)
/// or `Vec<u8>` (integer array variant) field. Full contract on the header
/// declaration.
std::vector<char> borsh_payload_bytes(const fc::variant& field_value) {
   if (field_value.is_string())
      return fc::base64_decode(field_value.as_string());
   FC_ASSERT(field_value.is_array(),
             "payload field decoded to neither a base64 string (bytes) nor a byte array (Vec<u8>)");
   const auto& arr = field_value.get_array();
   std::vector<char> out;
   out.reserve(arr.size());
   for (const auto& element : arr) {
      const auto value = element.as_uint64();
      FC_ASSERT(value <= 0xFF, "payload array element {} is out of byte range", value);
      out.push_back(static_cast<char>(value));
   }
   return out;
}

/// Decode + validate a fetched `LatestOutboundEnvelope` account through the
/// loaded IDL. Full contract on the header declaration.
std::vector<char> decode_latest_envelope_account(opp_solana_outpost_client&  program_client,
                                                 const std::vector<uint8_t>& account_data,
                                                 uint32_t                    epoch_index,
                                                 const std::string&          log_label) {
   uint32_t          stored_epoch = 0;
   std::vector<char> envelope_bytes;
   bool              checksum_ok = true;
   try {
      // IDL-driven decode: verifies the 8-byte Anchor discriminator and
      // follows the loaded IDL's declared field order, so the standalone and
      // integrated `LatestOutboundEnvelope` layouts both decode value-exactly.
      const auto decoded = program_client.decode_account_info_data(latest_envelope::account_name,
                                                                   account_data);
      const auto& obj = decoded.get_object();
      stored_epoch    = static_cast<uint32_t>(obj[latest_envelope::field_epoch].as_uint64());
      if (stored_epoch != 0 && stored_epoch == epoch_index) {
         envelope_bytes = borsh_payload_bytes(obj[latest_envelope::field_data]);
         // Both program versions store `checksum = keccak256(encoded_envelope)`.
         // Verifying it proves the bytes extracted as `data` are the bytes the
         // program hashed - i.e. the IDL's field order matches the deployment.
         // An IDL that omits the field (or declares an unrecognized shape)
         // skips the check rather than failing envelopes it cannot verify.
         if (obj.contains(latest_envelope::field_checksum)) {
            const auto& checksum_v = obj[latest_envelope::field_checksum];
            if (checksum_v.is_array() && checksum_v.get_array().size() == LATEST_ENVELOPE_CHECKSUM_BYTES) {
               const auto& stored = checksum_v.get_array();
               const auto  actual = fc::crypto::keccak256::hash(std::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(envelope_bytes.data()), envelope_bytes.size()));
               for (size_t i = 0; i < LATEST_ENVELOPE_CHECKSUM_BYTES; ++i) {
                  if (static_cast<uint8_t>(stored[i].as_uint64()) != actual.data()[i]) {
                     checksum_ok = false;
                     break;
                  }
               }
            }
         }
      }
   } catch (const fc::exception& e) {
      // Permanently undecodable account bytes are an IDL-vs-deployment drift
      // signal, not a transient condition - keep this visible at default log
      // level so a stalled relay is diagnosable.
      wlog("outpost_solana_client[{}]: cannot decode latest_outbound_envelope account "
           "({} bytes) through the loaded IDL - IDL/deployment drift? {}",
           log_label, account_data.size(), e.to_detail_string());
      return {};
   }

   if (stored_epoch == 0) {
      // Initialized state: outpost has not emitted any envelope yet.
      // Expected during cluster warm-up; resolves on the next emit.
      dlog("outpost_solana_client[{}]: latest_outbound_envelope unwritten (epoch=0)", log_label);
      return {};
   }
   if (stored_epoch != epoch_index) {
      if (stored_epoch > epoch_index) {
         // The outpost claims an epoch AHEAD of the one this relay is trying
         // to read - either the account is being misread (IDL/deployment
         // field-order drift; the epoch=511 RCA surfaced exactly here) or the
         // relay's WIRE view is far behind. Neither self-heals quickly, so
         // stay visible at default log level.
         wlog("outpost_solana_client[{}]: latest_outbound_envelope stored_epoch={} is AHEAD of "
              "requested {} - possible IDL/deployment drift misreading the account",
              log_label, stored_epoch, epoch_index);
      } else {
         // Timing skew between the WIRE batch op and the outpost's emit
         // cadence. Resolves on the next poll once the outpost catches up;
         // kept at debug so steady-state polling isn't noisy (matches the
         // ethereum sibling's identical branch).
         dlog("outpost_solana_client[{}]: latest_outbound_envelope stored_epoch={} != requested {}",
              log_label, stored_epoch, epoch_index);
      }
      return {};
   }
   if (!checksum_ok) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope checksum does not match "
           "keccak256 of the decoded payload ({} bytes) - IDL/deployment drift?",
           log_label, envelope_bytes.size());
      return {};
   }
   if (envelope_bytes.size() > SOLANA_MAX_ENVELOPE_BYTES) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope data length "
           "{} exceeds envelope cap of {} bytes",
           log_label, envelope_bytes.size(), SOLANA_MAX_ENVELOPE_BYTES);
      return {};
   }

   sysio::opp::Envelope envelope;
   if (!envelope.ParseFromArray(envelope_bytes.data(),
                                static_cast<int>(envelope_bytes.size()))) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope did not "
           "decode as a protobuf Envelope ({} bytes)",
           log_label, envelope_bytes.size());
      return {};
   }
   if (static_cast<uint32_t>(envelope.epoch_index()) != epoch_index) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope inner "
           "epoch={} != requested epoch={}",
           log_label, envelope.epoch_index(), epoch_index);
      return {};
   }

   ilog("outpost_solana_client[{}]: read inbound envelope for epoch {} ({} bytes)",
        log_label, epoch_index, envelope_bytes.size());
   return envelope_bytes;
}

/// Append a terminal remaining-account meta, merging permissions when a
/// previous effect branch already added the same pubkey.
void record_terminal_account(std::vector<fc::network::solana::account_meta>& metas,
                             const fc::network::solana::solana_public_key& key,
                             bool is_writable) {
   auto it = std::find_if(metas.begin(), metas.end(), [&](const auto& meta) {
      return meta.key == key;
   });
   if (it == metas.end()) {
      metas.push_back(is_writable
                         ? fc::network::solana::account_meta::writable(key, false)
                         : fc::network::solana::account_meta::readonly(key, false));
      return;
   }
   it->is_writable = it->is_writable || is_writable;
}

namespace {

/// Little-endian seed bytes for Anchor PDA derivation from a `u64`.
std::vector<uint8_t> u64_seed(uint64_t value) {
   std::vector<uint8_t> out(8);
   for (size_t i = 0; i < out.size(); ++i) {
      out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
   }
   return out;
}

fc::network::solana::solana_public_key derive_reserve_pda(
   const fc::network::solana::solana_public_key& program_id,
   uint64_t token_code,
   uint64_t reserve_code) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>{'r','e','s','e','r','v','e'},
       u64_seed(token_code),
       u64_seed(reserve_code)},
      program_id).first;
}

fc::network::solana::solana_public_key derive_reserve_vault_pda(
   const fc::network::solana::solana_public_key& program_id,
   uint64_t token_code,
   uint64_t reserve_code) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>{'r','e','s','e','r','v','e','_','v','a','u','l','t'},
       u64_seed(token_code),
       u64_seed(reserve_code)},
      program_id).first;
}

} // anonymous namespace (within outpost_solana_client_detail)

uint32_t count_inbound_attestations(const std::vector<char>& envelope_bytes) {
   sysio::opp::Envelope env;
   if (!env.ParseFromArray(envelope_bytes.data(),
                           static_cast<int>(envelope_bytes.size()))) {
      return 0;
   }
   uint32_t count = 0;
   for (const auto& message : env.messages()) {
      count += static_cast<uint32_t>(message.payload().attestations_size());
   }
   return count;
}

std::vector<inbound_effect>
extract_inbound_effects(const std::vector<char>& envelope_bytes) {
   std::vector<inbound_effect> effects;

   sysio::opp::Envelope env;
   if (!env.ParseFromArray(envelope_bytes.data(),
                           static_cast<int>(envelope_bytes.size()))) {
      // Two things in the old wording are no longer true: the terminal
      // `epoch_in` carries no extras by design now, and a missing effect
      // account ABORTS on-chain rather than logging and skipping. With no
      // manifest, `drain_dispatch` cannot pack a batch at all, so the cursor
      // stays put and the epoch waits -- which is the safe failure, but it
      // needs investigating rather than riding out.
      wlog("outpost_solana_client: envelope decode for effect-account "
           "extraction failed; no manifest can be built, so dispatch cannot "
           "settle this epoch and the cursor will not advance");
      return effects;
   }

   // The flat index MUST advance for every attestation, including ones that
   // need no accounts and ones that fail to decode -- it is the same sequence
   // the program's dispatch cursor counts, so skipping a position here would
   // misalign every later dispatch_limit.
   size_t index = 0;
   for (const auto& message : env.messages()) {
      for (const auto& entry : message.payload().attestations()) {
         const size_t at = index++;
         switch (entry.type()) {
            case sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION: {
               sysio::opp::attestations::OperatorAction oa;
               if (!oa.ParseFromString(entry.data())) continue;
               if (oa.action_type() !=
                     sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT) {
                  continue;
               }
               if (auto pk = sol_pubkey_from_chain_address(oa.op_address())) {
                  effects.push_back(inbound_effect{at, effect_shape::native_payee, *pk, std::nullopt});
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_DEPOSIT_REVERT: {
               sysio::opp::attestations::DepositRevert dr;
               if (!dr.ParseFromString(entry.data())) continue;
               if (auto pk = sol_pubkey_from_chain_address(dr.depositor())) {
                  effects.push_back(inbound_effect{at, effect_shape::native_payee, *pk, std::nullopt});
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_SWAP_REMIT: {
               sysio::opp::attestations::SwapRemit sr;
               if (!sr.ParseFromString(entry.data())) continue;
               effects.push_back(inbound_effect{
                  at, effect_shape::swap_remit,
                  sol_pubkey_from_chain_address(sr.recipient()),
                  reserve_pda_seeds{sr.amount().token_code(), sr.reserve_code()}});
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_SWAP_REVERT: {
               sysio::opp::attestations::SwapRevert sr;
               if (!sr.ParseFromString(entry.data())) continue;
               effects.push_back(inbound_effect{
                  at, effect_shape::swap_revert,
                  sol_pubkey_from_chain_address(sr.depositor()),
                  reserve_pda_seeds{sr.refund_amount().token_code(), sr.source_reserve_code()}});
               break;
            }
            // The reserve-lifecycle round-trips need the per-(token, reserve)
            // Reserve PDA too: `handle_reserve_ready` flips its status field,
            // and `handle_reserve_create_cancelled` reads the refund
            // amount/creator off it. RESERVE_READY rides exactly ONE envelope
            // (queued once at `matchreserve`), so a missing PDA does not defer
            // the flip -- it strands the reserve in PENDING permanently.
            case sysio::opp::types::ATTESTATION_TYPE_RESERVE_READY: {
               sysio::opp::attestations::ReserveReady rr;
               if (!rr.ParseFromString(entry.data())) continue;
               effects.push_back(inbound_effect{
                  at, effect_shape::reserve_ready, std::nullopt,
                  reserve_pda_seeds{rr.token_code(), rr.reserve_code()}});
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED: {
               sysio::opp::attestations::ReserveCreateCancelled rcc;
               if (!rcc.ParseFromString(entry.data())) continue;
               effects.push_back(inbound_effect{
                  at, effect_shape::reserve_create_cancelled, std::nullopt,
                  reserve_pda_seeds{rcc.token_code(), rcc.reserve_code()}});
               break;
            }
            default:
               break;
         }
      }
   }

   return effects;
}

token_custody_info resolve_token_custody(const fc::variant_object& outpost_config,
                                         uint64_t token_code) {
   token_custody_info custody;
   bool address_found   = false;
   bool precision_found = false;

   if (outpost_config.contains("token_addresses_by_code")) {
      for (const auto& entry_v : outpost_config["token_addresses_by_code"].get_array()) {
         const auto& entry = entry_v.get_object();
         if (entry["token_code"].as_uint64() == token_code) {
            custody.mint =
               fc::network::solana::solana_public_key::from_base58_string(entry["mint"].as_string());
            address_found = true;
            break;
         }
      }
   }
   FC_ASSERT(address_found,
             "token address binding unconfigured for token_code {} — the outpost's "
             "token_addresses_by_code map must carry an explicit entry (zero mint = native)",
             token_code);

   if (outpost_config.contains("precision_by_token_code")) {
      for (const auto& entry_v : outpost_config["precision_by_token_code"].get_array()) {
         const auto& entry = entry_v.get_object();
         if (entry["token_code"].as_uint64() == token_code) {
            custody.decimals  = static_cast<uint8_t>(entry["decimals"].as_uint64());
            precision_found = true;
            break;
         }
      }
   }
   FC_ASSERT(precision_found,
             "token precision unconfigured for token_code {} — required, same as "
             "wire-ethereum's WIRE_TokenPrecisionUnset and the program's PrecisionUnconfigured",
             token_code);

   return custody;
}

std::string drive_dispatch_rounds(
   uint32_t                                                           epoch_index,
   const std::vector<std::vector<fc::network::solana::account_meta>>& per_attestation,
   const std::function<void()>&                                       throw_if_past_deadline,
   const std::function<epoch_dispatch_progress()>&                    read_progress,
   const std::function<std::string(uint32_t, std::vector<fc::network::solana::account_meta>)>&
                                                                      send_dispatch,
   const std::string&                                                 log_label) {
   const auto total_attestations = static_cast<uint32_t>(per_attestation.size());
   std::string last_sig;

   // Round 0's own `before = read_progress()` below is what actually seeds
   // the cursor (and re-confirms consensus) -- this initializer is never read.
   uint32_t settled = 0;
   for (uint32_t round = 0; round < MAX_DISPATCH_ROUNDS; ++round) {
      throw_if_past_deadline();

      // Consensus has not tipped yet: this operator has delivered and there is
      // nothing to settle. That is the normal path for every operator but the
      // one whose delivery reaches the threshold.
      const auto before = read_progress();
      if (!before.consensus_reached) {
         ilog("outpost_solana_client[{}]: epoch={} delivered; consensus not yet "
              "reached, nothing to dispatch from this relay",
              log_label, epoch_index);
         return last_sig;
      }
      settled = before.dispatched_count;

      // A zero-attestation envelope still has to CLOSE its epoch. The terminal
      // `epoch_in` deliberately records the delivery and stops, and
      // `dispatch_attestations` is the ONLY place `next_epoch_index` advances:
      // the program's completion block runs on a crank whose window clamps to
      // the empty envelope. Without this send, `0 >= 0` would read as "already
      // drained" on EVERY relay at once and the epoch would stall with
      // consensus already tipped -- neither re-deliverable nor rejectable. One
      // clamped crank closes it; on an epoch that already closed the crank is
      // a benign on-chain no-op (opp-consensus.md), so a re-send from a later
      // tick costs a fee and nothing else.
      if (total_attestations == 0) {
         last_sig = send_dispatch(1, {});
         ilog("outpost_solana_client[{}]: zero-attestation close crank sent "
              "epoch={} sig={}",
              log_label, epoch_index, last_sig);
         return last_sig;
      }
      if (settled >= total_attestations) return last_sig;

      // Greedy pack from the cursor: extend while the account UNION still fits.
      // Always take at least one attestation so a single oversized effect makes
      // progress. An attestation whose own manifest cannot fit a packet throws
      // locally in `transaction::serialize` -- that is an alarm, not a retry.
      std::vector<fc::network::solana::account_meta> batch_accounts;
      uint32_t batch_end = settled;
      while (batch_end < total_attestations) {
         auto candidate = batch_accounts;
         for (const auto& meta : per_attestation[batch_end]) {
            record_terminal_account(candidate, meta.key, meta.is_writable);
         }
         if (candidate.size() > MAX_TERMINAL_DYNAMIC_ACCOUNTS && batch_end > settled) break;
         batch_accounts = std::move(candidate);
         ++batch_end;
      }
      const uint32_t dispatch_limit = batch_end - settled;
      FC_ASSERT(dispatch_limit > 0,
                "outpost_solana_client: empty dispatch batch at cursor {} of {}",
                settled, total_attestations);

      const size_t batch_extra_count = batch_accounts.size();
      last_sig = send_dispatch(dispatch_limit, std::move(batch_accounts));
      ilog("outpost_solana_client[{}]: dispatch_attestations sent epoch={} "
           "round={} window=[{},{}) of {} extras={} sig={}",
           log_label, epoch_index, round, settled, batch_end, total_attestations,
           batch_extra_count, last_sig);

      // Never assume the batch landed whole -- re-read rather than infer.
      const auto progress = read_progress();
      if (progress.dispatched_count <= settled) {
         ilog("outpost_solana_client[{}]: epoch={} cursor did not advance past "
              "{} (reported {}); another caller is draining this envelope",
              log_label, epoch_index, settled, progress.dispatched_count);
         return last_sig;
      }
      settled = progress.dispatched_count;
      if (settled >= total_attestations) return last_sig;
   }

   // Exhaustion is NOT success -- but under tick-driven cranking it is not a
   // caller failure either: the cursor persists on-chain and the next inbound
   // tick resumes from it. Alarm loudly (an undrained cursor blocks every
   // later epoch -- epoch-stall-is-fatal.md) and hand the remainder to the
   // next tick rather than converting bounded forward progress into a throw
   // that the tick loop would only log anyway.
   elog("outpost_solana_client[{}]: dispatch round budget ({}) exhausted for "
        "epoch {} at {}/{}; the next tick resumes from the on-chain cursor",
        log_label, MAX_DISPATCH_ROUNDS, epoch_index, settled, total_attestations);
   return last_sig;
}

} // namespace outpost_solana_client_detail

outpost_solana_client::outpost_solana_client(
   solana_client_entry_ptr                        entry,
   fc::network::solana::solana_public_key         program_id,
   std::vector<fc::network::solana::idl::program> program_idls,
   uint64_t                                       chain_code,
   uint32_t                                       chain_id,
   solana_outpost_role                            role)
   : _entry(std::move(entry))
   , _program_id(program_id)
   , _outpost_id(chain_code)
   , _chain_id(chain_id) {
   FC_ASSERT(_entry && _entry->client,
             "solana_client_entry must carry a client");
   FC_ASSERT(!program_idls.empty(),
             "Solana outpost requires at least one program IDL");

   // Reduce same-named IDL versions to the one(s) whose declared address
   // matches the deployed program id BEFORE the program client caches its
   // decode-driving IDL, so `--solana-idl-file` order can never decide which
   // field order accounts are decoded with.
   program_idls =
      outpost_solana_client_detail::select_program_idls_matching(std::move(program_idls), _program_id);

   _program_client = std::make_shared<opp_solana_outpost_client>(
      _entry->client, _program_id, program_idls);

   // Role-gated boot validation: only roles that read inbound envelopes need
   // a decodable `LatestOutboundEnvelope` in the IDL. For those roles the
   // shape is asserted HERE so a misshaped IDL throws at boot
   // (`create_outpost_client`) rather than on the first inbound poll - the
   // poll loop wlogs and retries forever, which would hide the misconfig.
   // The IDL is immutable after construction, so the check can never go stale.
   if (role == solana_outpost_role::batch_operator) {
      FC_ASSERT(_program_client->get_program(),
                "outpost_solana_client: no IDL program loaded; cannot validate "
                "the LatestOutboundEnvelope declaration");
      outpost_solana_client_detail::assert_latest_envelope_shape(*_program_client->get_program());
      // The crank path is a batch-operator concern, so it boot-validates under
      // the same role gate as the envelope shape: a deployment whose IDL lacks
      // the instruction must fail HERE, not at the first drain.
      FC_ASSERT(_program_client->has_idl("dispatch_attestations"),
                "outpost program IDL lacks `dispatch_attestations`; this relay "
                "requires the two-phase dispatch program (delivery-only "
                "epoch_in + dispatch_attestations crank)");
   }
}

sysio::opp::types::ChainKind outpost_solana_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_SVM;
}

std::vector<uint8_t>
outpost_solana_client::authenticated_caller_address() const {
   const auto public_key = _entry->client->get_pubkey().serialize();
   return {public_key.begin(), public_key.end()};
}

outpost_solana_client_detail::epoch_dispatch_progress
outpost_solana_client::read_epoch_dispatch_progress(uint32_t epoch_index) {
   const auto epoch_deliveries_pda = derive_epoch_deliveries_pda(_program_id, epoch_index);

   outpost_solana_client_detail::epoch_dispatch_progress progress;
   // Pinned to `processed` to MATCH THE WRITE. `execute_tx_and_confirm` returns
   // once the tx is `processed`, while `get_account_info` defaults to
   // `confirmed`; when the confirmed bank lags the processed slot an unpinned
   // read returns the PRE-tx cursor, and the drain loop takes a false "another
   // caller is draining" break with the remainder unsettled. `read_inbound_envelope`
   // pins its read commitment for the same reason.
   const auto account_info = _entry->client->get_account_info(
      epoch_deliveries_pda, fc::network::solana::commitment_t::processed);
   if (!account_info.has_value() || account_info->data.empty()) {
      // No delivery recorded yet for this epoch -- zero progress, not an error.
      return progress;
   }

   const auto decoded = _program_client->decode_account_info_data("EpochDeliveries", account_info->data);
   const auto& row = decoded.get_object();
   if (row.contains("consensus_reached")) {
      progress.consensus_reached = row["consensus_reached"].as_bool();
   }
   if (row.contains("dispatched_count")) {
      progress.dispatched_count = static_cast<uint32_t>(row["dispatched_count"].as_uint64());
   }
   return progress;
}

std::optional<outpost_solana_client::reserve_terminal_info>
outpost_solana_client::reserve_info_for_codes(uint64_t token_code, uint64_t reserve_code) {
   const auto reserve_pda =
      outpost_solana_client_detail::derive_reserve_pda(_program_id, token_code, reserve_code);

   const auto account_info = _entry->client->get_account_info(reserve_pda);
   if (!account_info.has_value()) {
      wlog("outpost_solana_client[{}]: Reserve({}, {}) absent at {}; "
           "terminal manifest will omit branch-specific accounts for this reserve",
           to_string(),
           token_code,
           reserve_code,
           reserve_pda.to_string(fc::yield_function_t{}));
      return std::nullopt;
   }

   FC_ASSERT(!account_info->data.empty(),
             "Reserve({}, {}) at {} has no account data",
             token_code,
             reserve_code,
             reserve_pda.to_string(fc::yield_function_t{}));

   const auto reserve_v = _program_client->decode_account_info_data("Reserve", account_info->data);
   const auto& reserve = reserve_v.get_object();
   FC_ASSERT(reserve.contains("creator"), "Reserve account missing creator field");

   // Custody (mint / decimals) lives on the OutpostConfig maps keyed by
   // token_code — the clean-room program resolves it there at dispatch time,
   // so the relay mirrors that lookup to stay account-consistent with the
   // on-chain handlers.
   const auto config_info = _entry->client->get_account_info(_program_client->config_pda);
   FC_ASSERT(config_info.has_value() && !config_info->data.empty(),
             "OutpostConfig account missing at {} — outpost not initialized",
             _program_client->config_pda.to_string(fc::yield_function_t{}));
   const auto config_v =
      _program_client->decode_account_info_data("OutpostConfig", config_info->data);
   const auto custody =
      outpost_solana_client_detail::resolve_token_custody(config_v.get_object(), token_code);

   return reserve_terminal_info{
      fc::network::solana::solana_public_key::from_base58_string(reserve["creator"].as_string()),
      custody.mint,
      custody.decimals
   };
}

std::string outpost_solana_client::send_dispatch_attestations(
   uint32_t                                       epoch_index,
   uint32_t                                       dispatch_limit,
   std::vector<fc::network::solana::account_meta> extra_remaining_accounts) {
   const auto epoch_deliveries_pda = derive_epoch_deliveries_pda(_program_id, epoch_index);
   // This relay cranks with ITS OWN staged buffer. The program pins the
   // decoded bytes to the consensus digest, so any operator's buffer is
   // acceptable -- but ours is the one we know exists, because we staged it
   // during delivery.
   const auto chunk_buffer_pda =
      derive_envelope_chunks_pda(_program_id, epoch_index, _entry->client->get_pubkey());
   fc::network::solana::account_overrides_t overrides = {
      {"config",                   _program_client->config_pda},
      {"operator_registry",        _program_client->operator_registry_pda},
      {"epoch_deliveries",         epoch_deliveries_pda},
      {"chunk_buffer",             chunk_buffer_pda},
      {"outbound_message_buffer",  _program_client->outbound_message_buffer_pda},
      {"outbound_envelopes",       _program_client->outbound_envelopes_pda},
      {"latest_outbound_envelope", _program_client->latest_outbound_envelope_pda},
      {"vault",                    _program_client->vault_pda},
      {"reserve_aggregate",        _program_client->reserve_pda},
   };
   const auto& instr = _program_client->get_idl("dispatch_attestations");
   fc::network::solana::program_invoke_data_items params = {
      fc::variant(epoch_index),
      fc::variant(dispatch_limit),
   };
   // The heap frame rides with the work it protects: this call decodes the
   // staged envelope, dispatches its effects, and on the draining call
   // encodes the outbound emit.
   std::vector<fc::network::solana::instruction> pre_ixs;
   pre_ixs.push_back(
      fc::network::solana::system::compute_budget::request_heap_frame(256'000));
   auto accounts = _program_client->resolve_accounts(instr, params, overrides);
   accounts.reserve(accounts.size() + extra_remaining_accounts.size());
   accounts.insert(accounts.end(), extra_remaining_accounts.begin(),
                   extra_remaining_accounts.end());
   return _program_client->execute_tx_and_confirm(instr, accounts, params, pre_ixs);
}

std::string outpost_solana_client::drain_dispatch(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);
   std::string last_sig;
   // Build the zero-data terminal call's remaining-account manifest. Data
   // chunks only upload bytes; the terminal call triggers `finalize_envelope`
   // on-chain, so every account touched by effect handlers must be declared
   // here with the right writable flag. Reserve-backed effects fetch the
   // Reserve PDA first and use its pinned custody facts, not mutable
   // OutpostConfig token rows.
   std::map<std::pair<uint64_t, uint64_t>, std::optional<reserve_terminal_info>> reserve_info_cache;

   auto reserve_info = [&](uint64_t token_code, uint64_t reserve_code) -> std::optional<reserve_terminal_info> {
      const auto cache_key = std::make_pair(token_code, reserve_code);
      auto it = reserve_info_cache.find(cache_key);
      if (it != reserve_info_cache.end()) return it->second;

      auto info = reserve_info_for_codes(token_code, reserve_code);
      return reserve_info_cache.emplace(cache_key, std::move(info)).first->second;
   };
   auto is_native_custody = [](const fc::network::solana::solana_public_key& mint) {
      return mint == fc::network::solana::system::program_ids::SYSTEM_PROGRAM;
   };

   const auto& token_program_id =
      fc::network::solana::system::program_ids::TOKEN_PROGRAM;
   const auto& associated_token_program_id =
      fc::network::solana::system::program_ids::ASSOCIATED_TOKEN_PROGRAM;
   const auto& system_program_id =
      fc::network::solana::system::program_ids::SYSTEM_PROGRAM;

   // Effect accounts for ONE attestation. Reserve-backed shapes read the
   // pinned Reserve record for custody facts rather than the mutable
   // OutpostConfig token rows, matching what the on-chain handler resolves.
   auto accounts_for_effect =
      [&](const outpost_solana_client_detail::inbound_effect& effect)
      -> std::vector<fc::network::solana::account_meta> {
      using shape = outpost_solana_client_detail::effect_shape;
      std::vector<fc::network::solana::account_meta> metas;
      auto add = [&](const fc::network::solana::solana_public_key& key, bool is_writable) {
         outpost_solana_client_detail::record_terminal_account(metas, key, is_writable);
      };

      if (effect.shape == shape::native_payee) {
         if (effect.recipient) add(*effect.recipient, true);
         return metas;
      }

      if (!effect.reserve) return metas;
      const auto token_code   = effect.reserve->token_code;
      const auto reserve_code = effect.reserve->reserve_code;

      // Every reserve-backed handler loads the Reserve PDA first.
      add(outpost_solana_client_detail::derive_reserve_pda(_program_id, token_code, reserve_code),
          true);
      if (effect.shape == shape::reserve_ready) return metas;

      const auto vault_pda =
         outpost_solana_client_detail::derive_reserve_vault_pda(_program_id, token_code, reserve_code);

      const auto info_opt = reserve_info(token_code, reserve_code);
      if (!info_opt.has_value()) {
         // The handler now ABORTS on an effect account it needs and did not
         // receive, rather than skipping the effect -- so omitting accounts
         // here can no longer silently drop a remit; at worst the terminal
         // call fails and is re-driven. Pass everything still derivable
         // without the Reserve record. Only the recipient/creator ATA is
         // truly out of reach (it needs the custody mint), so if this reserve
         // turns out to be SPL the call aborts and retries -- which is the
         // correct outcome for a transient read failure, and a no-op when the
         // Reserve PDA is simply uninitialized (the handler then has nothing
         // to settle).
         wlog("outpost_solana_client[{}]: Reserve({}, {}) unreadable while building the "
              "terminal manifest; passing derivable accounts only -- an SPL reserve will "
              "abort this call and be retried",
              to_string(), token_code, reserve_code);
         add(vault_pda, true);
         if (effect.recipient) add(*effect.recipient, true);
         add(token_program_id, false);
         return metas;
      }
      const auto& info = *info_opt;

      switch (effect.shape) {
         case shape::swap_remit: {
            if (!effect.recipient) break;
            if (is_native_custody(info.custody_mint)) { add(*effect.recipient, true); break; }
            add(vault_pda, true);
            add(fc::network::solana::system::get_associated_token_address(
                   *effect.recipient, info.custody_mint), true);
            add(token_program_id, false);
            break;
         }
         case shape::swap_revert: {
            if (!effect.recipient) break;
            if (is_native_custody(info.custody_mint)) { add(*effect.recipient, true); break; }
            add(vault_pda, true);
            add(info.custody_mint, false);
            add(*effect.recipient, true);
            add(fc::network::solana::system::get_associated_token_address(
                   *effect.recipient, info.custody_mint), true);
            add(token_program_id, false);
            add(associated_token_program_id, false);
            add(system_program_id, false);
            break;
         }
         case shape::reserve_create_cancelled: {
            if (is_native_custody(info.custody_mint)) { add(info.creator, true); break; }
            add(vault_pda, true);
            add(info.creator, false);
            add(fc::network::solana::system::get_associated_token_address(
                   info.creator, info.custody_mint), true);
            add(info.custody_mint, false);
            add(token_program_id, false);
            add(associated_token_program_id, false);
            add(system_program_id, false);
            break;
         }
         default:
            break;
      }
      return metas;
   };

   // The attestation count comes from a decode this function REQUIRES to
   // succeed. A local decode failure must THROW, never read as "zero
   // attestations": identical bytes fail identically on every relay, so a
   // quiet zero would take the zero-attestation close path (or an early
   // "already drained" return) on every group member at once, against an
   // envelope whose manifest nobody could actually build. The throw surfaces
   // in the crank's wlog and keeps the tick retrying while the bytes are
   // investigated.
   {
      sysio::opp::Envelope decode_probe;
      FC_ASSERT(decode_probe.ParseFromArray(envelope_bytes.data(),
                                            static_cast<int>(envelope_bytes.size())),
                "outpost_solana_client: inbound envelope for epoch {} does not "
                "decode locally; no dispatch manifest can be built and the "
                "cursor cannot drain",
                epoch_index);
   }

   // Consensus-first: until it has tipped, there is nothing to settle -- the
   // normal state for every operator but the one whose delivery reaches the
   // threshold. Checking here, after the decode probe above but BEFORE the
   // manifest extraction below, means an undecodable envelope still throws
   // (it must never read as empty) while the per-attestation account-manifest
   // work is skipped on the overwhelmingly common no-op path. `drive_dispatch_rounds`
   // does its own authoritative progress read at the top of round 0, so this
   // read is not threaded through as a seed -- it exists purely to gate.
   const auto progress = read_epoch_dispatch_progress(epoch_index);
   if (!progress.consensus_reached) {
      dlog("outpost_solana_client[{}]: epoch={} consensus not yet reached — no-op",
           to_string(), epoch_index);
      return last_sig;
   }

   // Per-attestation manifests, indexed by the SAME flat position the on-chain
   // dispatch cursor counts. Attestations needing no effect account keep an
   // empty entry so the indices stay aligned.
   const auto effects = outpost_solana_client_detail::extract_inbound_effects(envelope_bytes);
   const uint32_t total_attestations =
      outpost_solana_client_detail::count_inbound_attestations(envelope_bytes);
   std::vector<std::vector<fc::network::solana::account_meta>> per_attestation(total_attestations);
   for (const auto& effect : effects) {
      if (effect.attestation_index >= per_attestation.size()) continue;  // defensive
      per_attestation[effect.attestation_index] = accounts_for_effect(effect);
   }

   // Settlement is a SEPARATE instruction, driven from the on-chain cursor.
   // The crank loop lives in `drive_dispatch_rounds`, factored over its RPC
   // touchpoints so the unit tests can drive the full state machine.
   return outpost_solana_client_detail::drive_dispatch_rounds(
      epoch_index,
      per_attestation,
      [&] { throw_if_past_deadline(deadline_abs, OP_DISPATCH_ATTESTATIONS); },
      [&] { return read_epoch_dispatch_progress(epoch_index); },
      [&](uint32_t dispatch_limit, std::vector<fc::network::solana::account_meta> batch_accounts) {
         return send_dispatch_attestations(
            epoch_index, dispatch_limit, std::move(batch_accounts));
      },
      to_string());
}

std::string outpost_solana_client::deliver_outbound_envelope(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

   const size_t total = envelope_bytes.size();
   FC_ASSERT(total > 0,
             "outpost_solana_client: refusing to deliver an empty envelope");
   FC_ASSERT(total <= SOLANA_MAX_ENVELOPE_BYTES,
             "outpost_solana_client: envelope ({} bytes) exceeds Solana hard "
             "cap of {} bytes; the program will reject it",
             total, SOLANA_MAX_ENVELOPE_BYTES);

   const uint16_t total_chunks = static_cast<uint16_t>(
      (total + SOLANA_MAX_CHUNK_BYTES - 1) / SOLANA_MAX_CHUNK_BYTES);

   // Stream the envelope into the per-(epoch, signer) chunk buffer. Each call
   // goes through `solana_program_client::execute_tx_and_confirm`, which
   // serialises submission + waits for `processed`-commitment confirmation
   // before returning. Chunks are submitted sequentially -- the **batch
   // operator's only Solana-side instruction family is `epoch_in`**: all
   // non-empty data chunks stage bytes, then terminal calls trigger the
   // program's `finalize_envelope`.
   std::string last_sig;
   for (uint16_t i = 0; i < total_chunks; ++i) {
      throw_if_past_deadline(deadline_abs, OP_EPOCH_IN);

      const size_t off = static_cast<size_t>(i) * SOLANA_MAX_CHUNK_BYTES;
      const size_t len = std::min(SOLANA_MAX_CHUNK_BYTES, total - off);
      std::vector<uint8_t> chunk(
         reinterpret_cast<const uint8_t*>(envelope_bytes.data() + off),
         reinterpret_cast<const uint8_t*>(envelope_bytes.data() + off + len));

      last_sig = _program_client->epoch_in(
         epoch_index, i, total_chunks, static_cast<uint32_t>(total), chunk, {});
      ilog("outpost_solana_client[{}]: epoch_in chunk sent epoch={} chunk={}/{} bytes={} sig={}",
           to_string(), epoch_index, i, total_chunks, len, last_sig);
   }

   // Memoize once the buffer is fully staged: the program refuses to crank a
   // partial buffer (TerminalChunkBeforeDataComplete), so the crank-once-consensus-
   // tips path is only reachable from a COMPLETE staged buffer. A partial upload
   // leaves this unset and the next run_outbound tick re-uploads from offset zero.
   _delivered_envelope = std::make_pair(epoch_index, envelope_bytes);

   // ONE terminal call: records this operator's delivery and runs the consensus
   // predicate. It carries no effect accounts and no dispatch limit, so it is
   // fixed-size and cannot fail on account budget -- a dispatch concern can no
   // longer wedge consensus, and a decode failure can no longer prevent the
   // delivery from being recorded.
   last_sig = _program_client->epoch_in(
      epoch_index, total_chunks, total_chunks, static_cast<uint32_t>(total), {}, {});
   ilog("outpost_solana_client[{}]: epoch_in terminal delivery sent epoch={} sig={}",
        to_string(), epoch_index, last_sig);

   // Delivery IS complete at this point -- the terminal call above recorded it
   // and ran the consensus predicate. Settlement is a separate instruction and
   // a separate concern (`drain_dispatch`): kick it inline so the tipping
   // operator normally closes the epoch immediately, but never let a
   // settlement failure travel back into the delivery result. A throw here
   // would make the caller treat the DELIVERY as failed and re-upload every
   // chunk from offset zero on its next tick, even though the chunks and the
   // terminal call already landed; settlement instead resumes from the
   // on-chain cursor on a later drain. The failure is logged, never swallowed
   // -- the chain-side reason is the diagnostic.
   try {
      drain_dispatch(epoch_index, envelope_bytes, deadline_abs - fc::time_point::now());
   } catch (const fc::exception& e) {
      wlog("outpost_solana_client[{}]: inline dispatch drain after delivery "
           "failed for epoch={}: {}; a later drain resumes from the on-chain "
           "cursor",
           to_string(), epoch_index, e.to_detail_string());
   } catch (const std::exception& e) {
      wlog("outpost_solana_client[{}]: inline dispatch drain after delivery "
           "failed for epoch={}: {}; a later drain resumes from the on-chain "
           "cursor",
           to_string(), epoch_index, e.what());
   }
   return last_sig;
}


std::vector<char> outpost_solana_client::read_inbound_envelope(
   uint32_t         epoch_index,
   fc::microseconds deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

   throw_if_past_deadline(deadline_abs, OP_READ_LATEST);

   // Drain-then-read. The outbound envelope this read is after only EXISTS
   // once the epoch's inbound dispatch cursor drains, and this method is the
   // call the relay makes every tick for exactly as long as that envelope is
   // missing -- so it doubles as the standing recovery driver for a stuck
   // cursor (any elected operator's tick can unstick it; the epoch-overdue
   // window stays open). Best-effort: a drain failure is logged with the
   // chain-side reason and the read proceeds -- an undrained epoch simply
   // reads back empty via the stale-epoch check below, and the next tick
   // resumes from the on-chain cursor. Consensus-not-reached is a cheap
   // internal no-op (one progress read) at the top of `drain_dispatch`,
   // ahead of any manifest work.
   if (_delivered_envelope && _delivered_envelope->first == epoch_index) {
      try {
         drain_dispatch(epoch_index, _delivered_envelope->second,
                        deadline_abs - fc::time_point::now());
      } catch (const fc::exception& e) {
         wlog("outpost_solana_client[{}]: pre-read dispatch drain failed for "
              "epoch={}: {}; reading the outbound PDA anyway",
              to_string(), epoch_index, e.to_detail_string());
      } catch (const std::exception& e) {
         wlog("outpost_solana_client[{}]: pre-read dispatch drain failed for "
              "epoch={}: {}; reading the outbound PDA anyway",
              to_string(), epoch_index, e.what());
      }
   }

   // Single RPC: fetch the `latest_outbound_envelope` PDA. The Solana
   // program overwrites this account with the most recent emitted
   // envelope's bytes. The OPP cycle is atomic across actors — at any
   // time only the most-recent emitted envelope is in flight — so a
   // single-slot PDA is sufficient and historical reads are out of
   // scope (off-chain audit tooling owns them).
   // Read at `finalized`, not `confirmed`. WIRE consensus on inbound is committed forward against this
   // read: `confirmed` is supermajority lockout but can still revert below it during cluster instability,
   // which would leave WIRE state derived from a Solana slot that no longer exists. `finalized` is the
   // only commitment that cannot be rolled back. Deliberately not operator-configurable: the read
   // commitment is a consensus parameter, and operators reading at different commitments would deliver
   // divergent envelopes for the same epoch, manufacturing disputes among honest operators.
   auto info = _entry->client->get_account_info(
      _program_client->latest_outbound_envelope_pda,
      fc::network::solana::commitment_t::finalized);
   if (!info.has_value()) {
      // PDA was init'd at outpost initialize - absence here means the
      // RPC is out of sync or the program redeployed mid-run. Surface.
      wlog("outpost_solana_client[{}]: latest_outbound_envelope PDA absent",
           to_string());
      return {};
   }
   if (info->data.empty()) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope PDA returned empty data",
           to_string());
      return {};
   }
   if (info->owner != _program_id) {
      // A program upgrade that re-homed the seed (or an RPC serving a stale
      // fork) would otherwise decode foreign account bytes into an
      // undiagnosable stall - cheap defense-in-depth before decoding.
      wlog("outpost_solana_client[{}]: latest_outbound_envelope PDA is owned by {} "
           "instead of program {}; refusing to decode",
           to_string(), info->owner.to_string(fc::yield_function_t{}),
           _program_id.to_string(fc::yield_function_t{}));
      return {};
   }

   // Decode the fetched account through the loaded IDL: the IDL's declared
   // field order drives the decode, so the same nodeop reads both the
   // standalone `opp_outpost` and integrated `liqsol_core`
   // `LatestOutboundEnvelope` layouts value-exactly (same path the class
   // already uses for `Reserve` / `OutpostConfig`). For inbound-reading roles
   // the account declaration was validated at boot, so a decode failure here
   // signals IDL-vs-deployment drift and is logged at warning level.
   return outpost_solana_client_detail::decode_latest_envelope_account(
      *_program_client, info->data, epoch_index, to_string());
}

std::string outpost_solana_client::uw_commit(
   uint64_t                 uw_request_id,
   const std::vector<char>& uic_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

   throw_if_past_deadline(deadline_abs, OP_UW_COMMIT);

   // Submit the original canonical UIC bytes unchanged. The on-chain
   // `commit_underwrite` handler decodes them and binds the signed SVM caller
   // plus claimed ACTIVE roster identity before relaying them. The typed
   // wrapper supplies the IDL-derived signer, operator-registry, and outbound
   // message-buffer accounts; the underwriter does not override that list.
   std::vector<uint8_t> uic_bytes_u8(uic_bytes.begin(), uic_bytes.end());
   auto signature = _program_client->commit_underwrite(std::move(uic_bytes_u8));
   ilog("outpost_solana_client[{}]: uw_commit confirmed uwreq={} sig={} bytes={}",
        to_string(), uw_request_id, signature, uic_bytes.size());
   return signature;
}

} // namespace sysio
