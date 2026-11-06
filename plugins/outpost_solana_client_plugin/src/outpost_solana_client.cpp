#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/base64.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/task/deadline.hpp>
#include <fc/variant_object.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/types/types.pb.h>

namespace sysio {

namespace {

// ── Op labels used for deadline-exceeded error messages ──────────────────
constexpr std::string_view OP_EPOCH_IN    = "deliver_outbound_envelope:epoch_in";
constexpr std::string_view OP_REPORT_LIQ_YIELD = "crank_outpost:report_liq_yield";
/// liqsol-core's permissionless yield-report instruction; only the integrated
/// program declares it.
constexpr auto REPORT_LIQ_YIELD_INSTRUCTION = "report_liq_yield";
constexpr std::string_view OP_DISPATCH_ATTESTATIONS =
   "deliver_outbound_envelope:dispatch_attestations";
constexpr std::string_view OP_READ_LATEST = "read_inbound_envelope:get_account_info";
constexpr std::string_view OP_READ_EMITTED_DIGEST = "read_emitted_envelope_digest:get_account_info";

using outpost_solana_client_detail::derive_epoch_deliveries_pda;
using outpost_solana_client_detail::derive_envelope_chunks_pda;
using outpost_solana_client_detail::CLEANUP_ENVELOPE_CHUNKS_INSTRUCTION;

/// The liqSOL pool's seeds (wire-solana `liqsol-core`: `inbound.rs` and the
/// `report_liq_yield` / `synd` account declarations), same lock-step caveat.
constexpr std::string_view LIQSOL_GLOBAL_STATE_SEED       = "outpost_global_state";
constexpr std::string_view LIQSOL_DISTRIBUTION_STATE_SEED = "distribution_state";
constexpr std::string_view LIQSOL_POOL_SEED               = "liqsol_pool";
constexpr std::string_view LIQSOL_BUCKET_SEED             = "liqsol_bucket";
constexpr std::string_view LIQSOL_USER_RECORD_SEED        = "user_record";

/// Byte width the on-chain `LatestOutboundEnvelope.checksum` field decodes
/// to: `keccak256(encoded_envelope)`, written identically by both program
/// versions (standalone `opp_outpost` and integrated `liqsol_core`).
constexpr size_t LATEST_ENVELOPE_CHECKSUM_BYTES = fc::crypto::keccak256::byte_size;

/// Anchor discriminators precede all account headers used here.
constexpr size_t ANCHOR_DISCRIMINATOR_BYTES = 8;

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
/// The inbound read still decodes through the IDL; the reap gate reads the
/// integrated offset below, checked at boot (no standalone program on `next`).
namespace latest_envelope {
   constexpr auto account_name   = "LatestOutboundEnvelope";
   constexpr auto field_epoch    = "epoch_index";
   constexpr auto field_data     = "data";
   constexpr auto field_checksum = "checksum";
   /// Integrated layout: Anchor discriminator, then bump u8.
   constexpr size_t epoch_offset = ANCHOR_DISCRIMINATOR_BYTES + sizeof(uint8_t);
} // namespace latest_envelope

/// Identifiers of the outbound `EnvelopeLog` account: one record of each envelope the program emitted, kept for
/// its retention window.
namespace envelope_log {
   constexpr auto account_name    = "EnvelopeLog";
   constexpr auto field_envelopes = "envelopes";
   constexpr auto field_epoch     = "epoch_index";
   constexpr auto field_checksum  = "checksum";
} // namespace envelope_log

/// Identifiers of the per-(epoch, uploader) `EnvelopeChunks` staging buffer header the upload resume reads.
namespace envelope_chunks {
   constexpr auto account_name          = "EnvelopeChunks";
   constexpr auto field_epoch_index     = "epoch_index";
   constexpr auto field_total_bytes     = "total_bytes";
   constexpr auto field_total_chunks    = "total_chunks";
   constexpr auto field_received_chunks = "received_chunks";
   constexpr auto field_operator        = "operator";
   /// Every field the resume reads; each must be present in the decoded header.
   constexpr std::array<const char*, 5> fields_read = {field_epoch_index, field_total_bytes, field_total_chunks,
                                                      field_received_chunks, field_operator};
   /// Anchor's account discriminator, which precedes the header.
   constexpr size_t discriminator_bytes = ANCHOR_DISCRIMINATOR_BYTES;
   /// The bytemuck `repr(C)` header: epoch_index u32, total_bytes u32, total_chunks u16, received_chunks u16, bump u8,
   /// _padding [u8; 3], operator pubkey.
   constexpr size_t header_bytes = 4 + 4 + 2 + 2 + 1 + 3 + 32;
   /// `EnvelopeChunks::DATA_OFFSET`. The program sizes the account as exactly this plus the bytes staged so far, and
   /// only ever grows it, which bounds `received_chunks` without the IDL.
   constexpr size_t data_offset = discriminator_bytes + header_bytes;
   static_assert(data_offset == 56, "must equal the program's EnvelopeChunks::DATA_OFFSET");
   /// Byte offset of the epoch used by the boot round-trip.
   constexpr size_t epoch_offset = discriminator_bytes;
   /// Byte offset of `operator` in the account (discriminator, then epoch_index u32, total_bytes u32, total_chunks
   /// u16, received_chunks u16, bump u8, _padding [u8; 3]). The reaper's discovery read filters on it, so it is
   /// the one header offset the relay depends on outside the IDL decode.
   constexpr size_t operator_offset = discriminator_bytes + 4 + 4 + 2 + 2 + 1 + 3;
   static_assert(operator_offset + 32 == data_offset, "`operator` is the header's last field");
} // namespace envelope_chunks

/// `OutpostConfig` -- the singleton whose `next_epoch_index` gates the chunk reaper.
namespace outpost_config {
   constexpr auto account_name           = "OutpostConfig";
   constexpr auto field_next_epoch_index = "next_epoch_index";
   /// Integrated layout: discriminator, bump u8, config_version u8, chain_code u64.
   constexpr size_t epoch_offset = ANCHOR_DISCRIMINATOR_BYTES + 2 * sizeof(uint8_t) + sizeof(uint64_t);
} // namespace outpost_config

/// Reconcile failed closes at processed even though successful closes wait for confirmed status.
/// A transport timeout before the sweep deadline may follow execution in the processed bank.
/// Absence avoids false failure back-off;
/// a dropped fork only delays recovery until a later confirmed discovery sweep, with no rent attributed here.
constexpr auto ENVELOPE_CHUNK_ABSENCE_COMMITMENT = fc::network::solana::commitment_t::processed;

/// Number of accounts requested together for the cleanup eligibility cursors.
constexpr size_t REAP_GATE_ACCOUNT_COUNT = 2;
/// Keep both discriminators and both complete u32 cursors, without the envelope payload.
constexpr size_t REAP_GATE_HEADER_BYTES =
   std::max(outpost_config::epoch_offset, latest_envelope::epoch_offset) + sizeof(uint32_t);

/// Field names of one `getProgramAccounts` result entry, as the Solana JSON-RPC returns it.
namespace program_account_entry {
   constexpr auto field_pubkey   = "pubkey";
   constexpr auto field_account  = "account";
   constexpr auto field_data     = "data";
   constexpr auto field_lamports = "lamports";
} // namespace program_account_entry

/// The JSON-RPC `memcmp` filter object's keys.
namespace memcmp_filter {
   constexpr auto key    = "memcmp";
   constexpr auto offset = "offset";
   constexpr auto bytes  = "bytes";
} // namespace memcmp_filter

/// Identifiers of the per-epoch `EpochDeliveries` account + the two fields the
/// dispatch cursor read consumes. Both are validated at boot for the
/// batch-operator role (`assert_epoch_deliveries_shape`): a drifted
/// declaration would otherwise decode into a row without them, leaving
/// `consensus_reached` false and `dispatched_count` 0 forever.
namespace epoch_deliveries {
   constexpr auto account_name            = "EpochDeliveries";
   constexpr auto field_consensus_reached = "consensus_reached";
   constexpr auto field_dispatched_count  = "dispatched_count";
} // namespace epoch_deliveries

/// `DistributionState` -- the liqSOL pool singleton `handle_desyndicate_liq`
/// binds the pool's mint from. The relay decodes only `liqsol_mint`.
namespace distribution_state {
   constexpr auto account_name      = "DistributionState";
   constexpr auto field_liqsol_mint = "liqsol_mint";
} // namespace distribution_state

} // anonymous namespace

namespace outpost_solana_client_detail {

namespace {

/// Wrap a 32-byte address slice from a DesyndicateLIQ `user.address` into a
/// `solana_public_key`. Returns nullopt on the wrong chain kind or a malformed
/// length — the caller drops that attestation from the remaining_accounts list
/// (the on-chain handler will log+skip it too, so no fatal failure).
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

/// The field list an account is DECLARED with, from whichever IDL home holds
/// it: inline on the `accounts` entry (legacy shape) or in the `types` section
/// (Anchor IDL v2 keeps account struct fields there). Mirrors
/// `solana_program_client::decode_account_data`'s own inline-then-types
/// fallback, so a boot assert validates exactly the declaration the decoder
/// will follow at runtime.
///
/// @throws fc::exception when the account is undeclared, or declared with no
///         field definition in either home.
const std::vector<fc::network::solana::idl::field>&
declared_account_fields(const fc::network::solana::idl::program& program,
                        std::string_view                         account_name) {
   namespace idl = fc::network::solana::idl;
   const std::string name{account_name};
   const idl::account* account = program.find_account(name);
   FC_ASSERT(account, "IDL has no '{}' account definition", name);
   if (!account->fields.empty()) return account->fields;

   const idl::type_def* type_def = program.find_type(name);
   FC_ASSERT(type_def && type_def->is_struct() && type_def->struct_fields,
             "IDL '{}' has no struct field definition", name);
   return *type_def->struct_fields;
}

} // anonymous namespace (within outpost_solana_client_detail)

/// Assert the loaded IDL declares `LatestOutboundEnvelope` with the field
/// types the inbound reader relies on. Field order is unconstrained (the
/// reader decodes through the IDL at runtime). Full contract on the header
/// declaration.
void assert_latest_envelope_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const auto& fields = declared_account_fields(program, latest_envelope::account_name);

   bool has_epoch = false;
   bool has_data  = false;
   for (const auto& field : fields) {
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

void assert_envelope_chunks_shape(opp_solana_outpost_client& program_client) {
   constexpr uint32_t probe_epoch = 0x12345678;
   constexpr uint8_t probe_key_byte = 0xAB;
   const auto* program = program_client.get_program();
   FC_ASSERT(program != nullptr, "no IDL program loaded");
   const auto* account = program->find_account(envelope_chunks::account_name);
   FC_ASSERT(account != nullptr, "outpost program IDL declares no EnvelopeChunks");
   std::vector<uint8_t> header(envelope_chunks::data_offset);
   std::copy(account->discriminator.begin(), account->discriminator.end(), header.begin());
   const auto epoch = epoch_index_le_seed(probe_epoch);
   std::copy(epoch.begin(), epoch.end(), header.begin() + envelope_chunks::epoch_offset);
   std::fill(header.begin() + envelope_chunks::operator_offset, header.end(), probe_key_byte);
   fc::variant decoded;
   try {
      decoded = program_client.decode_account_info_data(envelope_chunks::account_name, header);
   } catch (fc::exception& e) {
      FC_RETHROW_EXCEPTION(e, warn, "{} IDL probe cannot be decoded for chunk discovery filter",
                           envelope_chunks::account_name);
   }
   const auto& fields = decoded.get_object();
   FC_ASSERT(fields.contains(envelope_chunks::field_epoch_index) && fields.contains(envelope_chunks::field_operator),
             "EnvelopeChunks IDL missing epoch_index/operator fields for chunk discovery");
   const auto key = fc::network::solana::solana_public_key::from_base58_string(
      fields[envelope_chunks::field_operator].as_string()).serialize();
   const bool operator_matches =
      std::all_of(key.begin(), key.end(), [](uint8_t byte) { return byte == probe_key_byte; });
   FC_ASSERT(fields[envelope_chunks::field_epoch_index].as_uint64() == probe_epoch && operator_matches,
             "EnvelopeChunks IDL epoch/operator offsets disagree with chunk discovery");
}

void assert_envelope_chunk_gate_shapes(opp_solana_outpost_client& program_client) {
   constexpr uint32_t probe_epoch = 0x12345678;
   // Zero vector lengths and a generous zero tail allow the full IDL decode to reach both cursors.
   constexpr size_t probe_account_bytes = 1024;
   const auto probe = [&](const char* name, const char* field, size_t offset) {
      const auto* account = program_client.get_program()->find_account(name);
      FC_ASSERT(account != nullptr, "outpost program IDL declares no {}", name);
      std::vector<uint8_t> bytes(probe_account_bytes);
      std::copy(account->discriminator.begin(), account->discriminator.end(), bytes.begin());
      const auto epoch = epoch_index_le_seed(probe_epoch);
      std::copy(epoch.begin(), epoch.end(), bytes.begin() + offset);
      fc::variant decoded;
      try {
         decoded = program_client.decode_account_info_data(name, bytes);
      } catch (fc::exception& e) {
         FC_RETHROW_EXCEPTION(e, warn, "{} IDL probe cannot be decoded for chunk reap gates", name);
      }
      const auto& fields = decoded.get_object();
      FC_ASSERT(fields.contains(field), "{} IDL missing '{}' for chunk reap gates", name, field);
      FC_ASSERT(fields[field].as_uint64() == probe_epoch,
                "{} IDL cursor offset disagrees with chunk reap gates", name);
   };
   FC_ASSERT(program_client.get_program() != nullptr, "no IDL program loaded");
   probe(outpost_config::account_name, outpost_config::field_next_epoch_index, outpost_config::epoch_offset);
   probe(latest_envelope::account_name, latest_envelope::field_epoch, latest_envelope::epoch_offset);
}

/// Assert the loaded IDL declares `EpochDeliveries` with the two cursor fields
/// `read_epoch_dispatch_progress` decodes. Full contract on the header
/// declaration.
void assert_epoch_deliveries_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const auto& fields = declared_account_fields(program, epoch_deliveries::account_name);

   bool has_consensus = false;
   bool has_cursor    = false;
   for (const auto& field : fields) {
      // Compare against the optional member directly, exactly as the envelope
      // shape check does: a malformed type object must reach the per-field
      // diagnostic rather than `get_primitive()`'s generic throw.
      if (field.name == epoch_deliveries::field_consensus_reached) {
         FC_ASSERT(field.type.is_primitive() && field.type.primitive == idl::primitive_type::bool_t,
                   "EpochDeliveries '{}' must be declared bool, got '{}'",
                   epoch_deliveries::field_consensus_reached, describe_idl_type(field.type));
         has_consensus = true;
      } else if (field.name == epoch_deliveries::field_dispatched_count) {
         // The cursor read narrows the decoded value to uint32; a differently
         // declared width means the IDL disagrees with the on-chain struct.
         FC_ASSERT(field.type.is_primitive() && field.type.primitive == idl::primitive_type::u32,
                   "EpochDeliveries '{}' must be declared u32, got '{}'",
                   epoch_deliveries::field_dispatched_count, describe_idl_type(field.type));
         has_cursor = true;
      }
   }
   FC_ASSERT(has_consensus,
             "EpochDeliveries IDL missing '{}' field; the dispatch crank would read consensus as "
             "never reached and no-op on every tick, stalling the epoch silently",
             epoch_deliveries::field_consensus_reached);
   FC_ASSERT(has_cursor,
             "EpochDeliveries IDL missing '{}' field; the dispatch crank would resume from 0 on "
             "every tick and re-send settled windows forever",
             epoch_deliveries::field_dispatched_count);
}

/// Assert the loaded IDL declares the outbound `EnvelopeLog` the way the digest reader decodes it. Full contract on
/// the header declaration.
void assert_envelope_log_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const std::vector<idl::field>& fields = declared_account_fields(program, envelope_log::account_name);
   const auto log = std::ranges::find(fields, std::string_view{envelope_log::field_envelopes}, &idl::field::name);
   FC_ASSERT(log != fields.end(), "EnvelopeLog IDL missing '{}' field", envelope_log::field_envelopes);
   FC_ASSERT(log->type.is_vec() && log->type.vec_element && log->type.vec_element->is_defined() &&
                log->type.vec_element->defined_name,
             "EnvelopeLog '{}' must be declared a Vec of a defined record type, got '{}'",
             envelope_log::field_envelopes, describe_idl_type(log->type));

   const std::string&   record_name = *log->type.vec_element->defined_name;
   const idl::type_def* record      = program.find_type(record_name);
   FC_ASSERT(record && record->is_struct() && record->struct_fields, "IDL '{}' has no struct field definition",
             record_name);
   bool has_epoch    = false;
   bool has_checksum = false;
   for (const idl::field& field : *record->struct_fields) {
      if (field.name == envelope_log::field_epoch) {
         FC_ASSERT(field.type.is_primitive() && field.type.primitive == idl::primitive_type::u32,
                   "{} '{}' must be declared u32, got '{}'", record_name, envelope_log::field_epoch,
                   describe_idl_type(field.type));
         has_epoch = true;
      } else if (field.name == envelope_log::field_checksum) {
         FC_ASSERT(field.type.is_array() && field.type.array_len == sizeof(fc::sha256) && field.type.array_element &&
                      field.type.array_element->is_primitive() &&
                      field.type.array_element->primitive == idl::primitive_type::u8,
                   "{} '{}' must be declared [u8; 32], got '{}'", record_name, envelope_log::field_checksum,
                   describe_idl_type(field.type));
         has_checksum = true;
      }
   }
   FC_ASSERT(has_epoch, "{} IDL missing '{}' field", record_name, envelope_log::field_epoch);
   FC_ASSERT(has_checksum, "{} IDL missing '{}' field", record_name, envelope_log::field_checksum);
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

/// Seed bytes of a `solana_public_key` for Anchor PDA derivation.
std::vector<uint8_t> pubkey_seed(const fc::network::solana::solana_public_key& key) {
   return std::vector<uint8_t>(key._data.begin(), key._data.end());
}

}

namespace {

/// A single-literal-seed PDA of `program_id`.
fc::network::solana::solana_public_key literal_seed_pda(
   const fc::network::solana::solana_public_key& program_id, std::string_view seed) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>(seed.begin(), seed.end())}, program_id).first;
}

} // anonymous namespace

// The liqSOL pool's fixed PDAs. Full contract on the header declarations.
fc::network::solana::solana_public_key derive_liqsol_global_state_pda(
   const fc::network::solana::solana_public_key& program_id) {
   return literal_seed_pda(program_id, LIQSOL_GLOBAL_STATE_SEED);
}

fc::network::solana::solana_public_key derive_liqsol_distribution_state_pda(
   const fc::network::solana::solana_public_key& program_id) {
   return literal_seed_pda(program_id, LIQSOL_DISTRIBUTION_STATE_SEED);
}

fc::network::solana::solana_public_key derive_liqsol_pool_authority_pda(
   const fc::network::solana::solana_public_key& program_id) {
   return literal_seed_pda(program_id, LIQSOL_POOL_SEED);
}

fc::network::solana::solana_public_key derive_liqsol_bucket_authority_pda(
   const fc::network::solana::solana_public_key& program_id) {
   return literal_seed_pda(program_id, LIQSOL_BUCKET_SEED);
}

/// The `PendingPayout` PDA of one depot request id. Full contract on the header
/// declaration.
fc::network::solana::solana_public_key derive_pending_desyndication_pda(
   const fc::network::solana::solana_public_key& program_id, uint64_t request_id) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>(PENDING_DESYNDICATION_SEED.begin(), PENDING_DESYNDICATION_SEED.end()),
       u64_seed(request_id)},
      program_id).first;
}

/// The `UserRecord` PDA of one liqSOL token account. Full contract on the
/// header declaration.
fc::network::solana::solana_public_key derive_liqsol_user_record_pda(
   const fc::network::solana::solana_public_key& program_id,
   const fc::network::solana::solana_public_key& token_account) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>(LIQSOL_USER_RECORD_SEED.begin(), LIQSOL_USER_RECORD_SEED.end()),
       pubkey_seed(token_account)},
      program_id).first;
}

/// Assert the loaded IDL declares `DistributionState` with the pubkey
/// `liqsol_mint` the DESYNDICATE_LIQ manifest reads. Full contract on the
/// header declaration.
void assert_distribution_state_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const auto& fields = declared_account_fields(program, distribution_state::account_name);
   for (const auto& field : fields) {
      if (field.name != distribution_state::field_liqsol_mint) continue;
      FC_ASSERT(field.type.is_primitive() && field.type.primitive == idl::primitive_type::pubkey,
                "DistributionState '{}' must be declared pubkey, got '{}'",
                distribution_state::field_liqsol_mint, describe_idl_type(field.type));
      return;
   }
   FC_ASSERT(false,
             "DistributionState IDL missing '{}'; a DESYNDICATE_LIQ manifest resolves the pool's "
             "mint from it and cannot be built without it",
             distribution_state::field_liqsol_mint);
}

/// Assert the loaded IDL declares the yield-crank fields with their
/// expected types. Full contract on the header declaration.
void assert_global_state_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const auto& fields = declared_account_fields(program, global_state::account_name);
   for (const auto flag : {global_state::field_frozen, global_state::field_paused}) {
      const auto field = std::ranges::find(fields, std::string_view{flag}, &idl::field::name);
      FC_ASSERT(field != fields.end(), "GlobalState IDL missing '{}'; the yield crank gates on it", flag);
      FC_ASSERT(field->type.is_primitive() && field->type.primitive == idl::primitive_type::bool_t,
                "GlobalState '{}' must be declared bool, got '{}'; the yield crank gates on it",
                flag, describe_idl_type(field->type));
   }
   for (const auto& field : fields) {
      if (field.name != global_state::field_wire_state) continue;
      FC_ASSERT(field.type.is_defined(),
                "GlobalState '{}' must be declared as an enum type, got '{}'",
                global_state::field_wire_state, describe_idl_type(field.type));
      const idl::type_def* def = program.find_type(field.type.get_defined_name());
      FC_ASSERT(def && def->is_enum(),
                "GlobalState '{}' type '{}' is not an enum in the IDL",
                global_state::field_wire_state, field.type.get_defined_name());
      const bool has_post_launch =
         std::any_of(def->enum_variants->begin(), def->enum_variants->end(),
                     [](const idl::enum_variant& v) { return v.name == global_state::post_launch; });
      FC_ASSERT(has_post_launch,
                "GlobalState '{}' enum '{}' has no '{}' variant; the yield crank gates on it",
                global_state::field_wire_state, field.type.get_defined_name(),
                global_state::post_launch);
      return;
   }
   FC_ASSERT(false, "GlobalState IDL missing '{}'; the yield crank gates on it",
             global_state::field_wire_state);
}

namespace {
/// Fail closed unless the named GlobalState bool is explicitly false.
bool global_state_flag_blocks_reporting(const fc::variant_object& row, const char* name) {
   const auto flag = row.find(name);
   return flag == row.end() || !flag->value().is_bool() || flag->value().as_bool();
}
} // namespace

bool liq_outpost_frozen(const fc::variant_object& global_state_row) {
   return global_state_flag_blocks_reporting(global_state_row, global_state::field_frozen);
}

bool liq_outpost_paused(const fc::variant_object& global_state_row) {
   return global_state_flag_blocks_reporting(global_state_row, global_state::field_paused);
}

bool liq_yield_report_due(const fc::variant_object& global_state_row) {
   if (liq_outpost_frozen(global_state_row) || liq_outpost_paused(global_state_row)) return false;
   auto state = global_state_row.find(global_state::field_wire_state);
   if (state == global_state_row.end() || !state->value().is_object()) return false;
   const auto& state_obj = state->value().get_object();
   auto variant = state_obj.find(IDL_ENUM_VARIANT_KEY);
   return variant != state_obj.end() && variant->value().is_string() &&
          variant->value().as_string() == global_state::post_launch;
}

liq_yield_report_state drive_liq_yield_report(
   const std::function<std::optional<fc::network::solana::account_info>()>& read_global_state,
   const std::function<fc::variant(const std::vector<uint8_t>&)>& decode_global_state,
   const std::function<void(const std::vector<fc::network::solana::instruction>&)>& report_yield) {
   const auto info = read_global_state();
   if (!info || info->data.empty()) return liq_yield_report_state::no_account;
   const auto decoded = decode_global_state(info->data);
   const auto& row = decoded.get_object();
   if (liq_outpost_frozen(row)) return liq_yield_report_state::frozen;
   if (liq_outpost_paused(row)) return liq_yield_report_state::paused;
   if (!liq_yield_report_due(row)) return liq_yield_report_state::not_due;
   report_yield({fc::network::solana::system::compute_budget::set_compute_unit_limit(
      REPORT_LIQ_YIELD_COMPUTE_UNITS)});
   return liq_yield_report_state::due;
}

fc::network::solana::account_overrides_t report_liq_yield_overrides(
   const fc::network::solana::solana_public_key& program_id,
   const fc::network::solana::solana_public_key& liqsol_mint,
   const fc::network::solana::solana_public_key& hook_program,
   const fc::network::solana::solana_public_key& config_pda,
   const fc::network::solana::solana_public_key& outbound_message_buffer_pda) {
   namespace accounts = report_liq_yield_accounts;
   const auto& token_2022       = fc::network::solana::system::program_ids::TOKEN_2022_PROGRAM;
   const auto  pool_authority   = derive_liqsol_pool_authority_pda(program_id);
   const auto  bucket_authority = derive_liqsol_bucket_authority_pda(program_id);
   const auto  pool_ata         = fc::network::solana::system::get_associated_token_address(
      pool_authority, liqsol_mint, token_2022);
   const auto  bucket_ata       = fc::network::solana::system::get_associated_token_address(
      bucket_authority, liqsol_mint, token_2022);
   return {
      {accounts::liqsol_mint,              liqsol_mint},
      {accounts::global_state,             derive_liqsol_global_state_pda(program_id)},
      {accounts::distribution_state,       derive_liqsol_distribution_state_pda(program_id)},
      {accounts::pool_authority,           pool_authority},
      {accounts::bucket_authority,         bucket_authority},
      {accounts::bucket_token_account,     bucket_ata},
      {accounts::bucket_user_record,       derive_liqsol_user_record_pda(program_id, bucket_ata)},
      {accounts::liqsol_pool_ata,          pool_ata},
      {accounts::pool_user_record,         derive_liqsol_user_record_pda(program_id, pool_ata)},
      {accounts::extra_account_meta_list,  derive_extra_account_metas_pda(hook_program, liqsol_mint)},
      {accounts::liqsol_core_program,      program_id},
      {accounts::transfer_hook_program,    hook_program},
      {accounts::config,                   config_pda},
      {accounts::outbound_message_buffer,  outbound_message_buffer_pda},
      {accounts::token_program,            token_2022},
      {accounts::associated_token_program, fc::network::solana::system::program_ids::ASSOCIATED_TOKEN_PROGRAM},
      {accounts::system_program,           fc::network::solana::system::program_ids::SYSTEM_PROGRAM},
   };
}

// ── Token-2022 transfer-hook resolution (SOL-396 lock-step) ─────────────────
//
// Layout constants are the on-chain ones, not guesses:
//   * a Token-2022 MINT with extensions is padded to `Account::LEN` (165),
//     byte 165 is the AccountType tag, and the TLV starts at 166. Each entry is
//     `type: u16 LE`, `length: u16 LE`, `value[length]`.
//   * `ExtensionType::TransferHook` is 14, and its value is
//     `authority: [u8;32]` then `program_id: [u8;32]`, each an
//     OptionalNonZeroPubkey where all-zero means None.
//   * the validation account is spl-type-length-value: `discriminator: [u8;8]`,
//     `length: u32 LE`, `value`. The Execute entry's discriminator is the first
//     8 bytes of sha256("spl-transfer-hook-interface:execute"). Its value is a
//     PodSlice: `count: u32 LE` then `count` × 35-byte `ExtraAccountMeta`.
namespace {

constexpr size_t   TOKEN_ACCOUNT_LEN        = 165;   // Account::LEN
constexpr size_t   TLV_START                = TOKEN_ACCOUNT_LEN + 1;
constexpr uint16_t EXT_TYPE_TRANSFER_HOOK   = 14;
constexpr size_t   EXTRA_ACCOUNT_META_SIZE  = 35;
constexpr uint8_t  META_DISC_LITERAL        = 0;
constexpr uint8_t  META_DISC_HOOK_PDA       = 1;
constexpr uint8_t  META_DISC_PUBKEY_DATA    = 2;
constexpr uint8_t  META_DISC_INDEX_BASE     = 0x80;  // U8_TOP_BIT
constexpr uint8_t  SEED_UNINITIALIZED       = 0;
constexpr uint8_t  SEED_LITERAL             = 1;
constexpr uint8_t  SEED_ACCOUNT_KEY         = 3;
/// A `TransferHook` TLV value is a 32-byte authority followed by a 32-byte
/// program id; anything shorter cannot be read as either a hook or its absence.
constexpr uint16_t TRANSFER_HOOK_EXT_MIN_LEN = 64;

/// First 8 bytes of sha256("spl-transfer-hook-interface:execute").
const std::array<uint8_t, 8> EXECUTE_TLV_DISCRIMINATOR = {
   0x69, 0x25, 0x65, 0xc5, 0x4b, 0xfb, 0x66, 0x1a};

uint16_t le_u16(const unsigned char* p) {
   return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t le_u32(const unsigned char* p) {
   return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // anonymous namespace

std::optional<fc::network::solana::solana_public_key>
mint_transfer_hook_program(const std::vector<uint8_t>& mint_account_data) {
   // A legacy SPL mint is exactly 82 bytes and carries no TLV at all; a
   // Token-2022 mint without extensions never reaches TLV_START either.
   if (mint_account_data.size() <= TLV_START) return std::nullopt;
   const auto* bytes = mint_account_data.data();

   size_t cursor = TLV_START;
   while (cursor + 4 <= mint_account_data.size()) {
      const uint16_t ext_type = le_u16(bytes + cursor);
      const uint16_t ext_len  = le_u16(bytes + cursor + 2);
      const size_t   value    = cursor + 4;
      if (ext_type == 0) break;  // Uninitialized terminator: the normal end of the TLV
      // A truncated entry is NOT "no hook". Degrading here builds a hook-free
      // manifest for a mint that may well declare one, and the CPI then aborts
      // with the cursor pinned -- the partial-parse outcome `parse_extra_account_metas`
      // is strict to prevent. Fail against the mint we could not read instead.
      FC_ASSERT(value + ext_len <= mint_account_data.size(),
                "custody mint TLV entry at offset {} claims {} bytes but the account holds {}; "
                "refusing to guess whether it declares a transfer hook",
                cursor, ext_len, mint_account_data.size());
      if (ext_type == EXT_TYPE_TRANSFER_HOOK) {
         // Same asymmetry: a TransferHook entry too short to carry its
         // authority+program pair cannot be read as "hook disabled".
         FC_ASSERT(ext_len >= TRANSFER_HOOK_EXT_MIN_LEN,
                   "custody mint declares a TransferHook extension of {} bytes, below the {} "
                   "its authority and program id require",
                   ext_len, TRANSFER_HOOK_EXT_MIN_LEN);
         // authority occupies the first 32; program_id the second.
         std::array<uint8_t, 32> program{};
         std::memcpy(program.data(), bytes + value + 32, 32);
         const bool none = std::all_of(program.begin(), program.end(),
                                       [](uint8_t b) { return b == 0; });
         if (none) return std::nullopt;   // extension present, hook disabled
         return fc::network::solana::solana_public_key(program);
      }
      cursor = value + ext_len;
   }
   return std::nullopt;
}

fc::network::solana::solana_public_key
derive_extra_account_metas_pda(const fc::network::solana::solana_public_key& hook_program,
                               const fc::network::solana::solana_public_key& mint) {
   static const std::string seed = "extra-account-metas";
   return fc::network::solana::system::find_program_address(
             {std::vector<uint8_t>(seed.begin(), seed.end()), pubkey_seed(mint)}, hook_program)
      .first;
}

std::vector<extra_account_meta>
parse_extra_account_metas(const std::vector<uint8_t>& validation_account_data) {
   // EVERY malformed shape below throws rather than returning a short list.
   // A partial parse is the worst possible outcome: `mint_transfer_hook_for`
   // would wrap it as authoritative, the manifest would carry the hook program
   // and validation PDA while silently omitting declared metas, and the CPI
   // would then fail with `IncorrectAccount` -- window aborted, cursor pinned,
   // nothing in the log naming the cause. That is exactly what the resolver
   // below refuses to do, and the parser must not undercut it.
   std::vector<extra_account_meta> metas;
   const size_t size = validation_account_data.size();
   FC_ASSERT(size >= 12,
             "ExtraAccountMetaList account is {} bytes, too short to hold even one TLV header",
             size);
   const auto* bytes = validation_account_data.data();

   size_t cursor = 0;
   while (cursor + 12 <= size) {
      const bool is_execute =
         std::memcmp(bytes + cursor, EXECUTE_TLV_DISCRIMINATOR.data(), 8) == 0;
      const uint32_t entry_len = le_u32(bytes + cursor + 8);
      const size_t   value     = cursor + 12;
      FC_ASSERT(value + entry_len <= size,
                "ExtraAccountMetaList TLV entry at {} claims {} bytes but only {} remain",
                cursor, entry_len, size - value);
      if (is_execute) {
         FC_ASSERT(entry_len >= 4,
                   "ExtraAccountMetaList Execute entry is {} bytes, too short for its count",
                   entry_len);
         const uint32_t count = le_u32(bytes + value);
         // `count` comes from account data we do not control. Bound the walk by
         // the ENTRY's extent, never by the account's: a count that overruns the
         // entry would otherwise parse the FOLLOWING TLV bytes as metas and push
         // arbitrary pubkeys into the manifest as writable accounts.
         const size_t declared = static_cast<size_t>(count) * EXTRA_ACCOUNT_META_SIZE;
         FC_ASSERT(4 + declared <= entry_len,
                   "ExtraAccountMetaList declares {} metas ({} bytes) but its entry holds {}",
                   count, declared, entry_len);
         metas.reserve(count);
         for (uint32_t i = 0; i < count; ++i) {
            const size_t at = value + 4 + static_cast<size_t>(i) * EXTRA_ACCOUNT_META_SIZE;
            extra_account_meta meta;
            meta.discriminator = bytes[at];
            std::memcpy(meta.address_config.data(), bytes + at + 1, 32);
            meta.is_signer   = bytes[at + 33] != 0;
            meta.is_writable = bytes[at + 34] != 0;
            metas.push_back(meta);
         }
         return metas;
      }
      cursor = value + entry_len;
   }
   FC_ASSERT(false,
             "ExtraAccountMetaList carries no Execute entry; the mint declares a transfer hook "
             "whose required accounts cannot be determined");
   return metas;   // unreachable; FC_ASSERT above always throws
}

std::vector<fc::network::solana::account_meta>
resolve_hook_metas(const std::vector<extra_account_meta>&        metas,
                   const fc::network::solana::solana_public_key& hook_program,
                   const fc::network::solana::solana_public_key& source,
                   const fc::network::solana::solana_public_key& mint,
                   const fc::network::solana::solana_public_key& destination,
                   const fc::network::solana::solana_public_key& authority,
                   const fc::network::solana::solana_public_key& validation_pda) {
   // The Execute account list the hook's seeds index into. Resolved metas are
   // appended as we go, because a later meta may key off an earlier one.
   std::vector<fc::network::solana::solana_public_key> accounts = {
      source, mint, destination, authority, validation_pda};

   std::vector<fc::network::solana::account_meta> resolved;
   resolved.reserve(metas.size());

   for (size_t i = 0; i < metas.size(); ++i) {
      const auto& meta = metas[i];
      // The dispatch transaction is signed by the batch operator alone, so a
      // meta demanding a signature is a shape this builder cannot satisfy: the
      // on-chain `add_extra_accounts_for_execute_cpi` would carry `is_signer`
      // through (de-escalation only applies to accounts already on the CPI
      // instruction, never a freshly appended extra) and `invoke_signed` would
      // abort as an unauthorized signer -- inside the CPI, with the cursor
      // pinned and no local diagnostic. Refuse loudly here, naming the meta,
      // exactly as the unsupported seed and discriminator forms below do.
      FC_ASSERT(!meta.is_signer,
                "transfer-hook meta {} demands a signature, which the dispatch transaction "
                "cannot provide", i);
      fc::network::solana::solana_public_key key;

      if (meta.discriminator == META_DISC_LITERAL) {
         key = fc::network::solana::solana_public_key(meta.address_config);
      } else if (meta.discriminator == META_DISC_HOOK_PDA
                 || meta.discriminator >= META_DISC_INDEX_BASE) {
         const auto& seed_program =
            meta.discriminator == META_DISC_HOOK_PDA
               ? hook_program
               : [&]() -> const fc::network::solana::solana_public_key& {
                    const size_t idx = meta.discriminator - META_DISC_INDEX_BASE;
                    FC_ASSERT(idx < accounts.size(),
                              "transfer-hook meta {} names program index {}, but only {} "
                              "accounts are resolved at that point",
                              i, idx, accounts.size());
                    return accounts[idx];
                 }();

         std::vector<std::vector<uint8_t>> seeds;
         size_t at = 0;
         while (at < meta.address_config.size()) {
            const uint8_t tag = meta.address_config[at];
            if (tag == SEED_UNINITIALIZED) break;
            if (tag == SEED_LITERAL) {
               FC_ASSERT(at + 1 < meta.address_config.size(), "truncated literal seed");
               const uint8_t len = meta.address_config[at + 1];
               FC_ASSERT(at + 2 + len <= meta.address_config.size(), "literal seed overruns config");
               seeds.emplace_back(meta.address_config.begin() + at + 2,
                                  meta.address_config.begin() + at + 2 + len);
               at += 2 + len;
            } else if (tag == SEED_ACCOUNT_KEY) {
               FC_ASSERT(at + 1 < meta.address_config.size(), "truncated account-key seed");
               const size_t idx = meta.address_config[at + 1];
               FC_ASSERT(idx < accounts.size(),
                         "transfer-hook meta {} seeds on account index {}, but only {} "
                         "accounts are resolved at that point",
                         i, idx, accounts.size());
               seeds.push_back(pubkey_seed(accounts[idx]));
               at += 2;
            } else {
               // InstructionData / AccountData. Both are resolvable only with
               // the Execute payload or another account's contents, neither of
               // which this builder has. Refuse rather than emit a manifest
               // that is short an account the CPI will demand.
               FC_ASSERT(false,
                         "transfer-hook meta {} uses unsupported seed form {}; the dispatch "
                         "manifest cannot be completed for this mint",
                         i, static_cast<unsigned>(tag));
            }
         }
         key = fc::network::solana::system::find_program_address(seeds, seed_program).first;
      } else {
         FC_ASSERT(meta.discriminator != META_DISC_PUBKEY_DATA,
                   "transfer-hook meta {} resolves a pubkey out of account data, which this "
                   "builder cannot read", i);
         FC_ASSERT(false, "transfer-hook meta {} has unknown discriminator {}",
                   i, static_cast<unsigned>(meta.discriminator));
      }

      accounts.push_back(key);
      resolved.push_back(fc::network::solana::account_meta{key, meta.is_signer, meta.is_writable});
   }
   return resolved;
}

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
            // The depot releasing a user's syndicated liqSOL. The user's pubkey is
            // the only payload fact the manifest needs -- the pool's mint comes from
            // `DistributionState`, and the handler's own token_code check precedes
            // every account it requires, so a wrong token is a logged skip on chain.
            case sysio::opp::types::ATTESTATION_TYPE_DESYNDICATE_LIQ: {
               sysio::opp::attestations::DesyndicateLIQ dl;
               if (!dl.ParseFromString(entry.data())) continue;
               if (auto pk = sol_pubkey_from_chain_address(dl.user())) {
                  effects.push_back(inbound_effect{
                     at, effect_shape::desyndicate_liq, *pk, dl.request_id()});
               }
               break;
            }
            default:
               break;
         }
      }
   }

   return effects;
}

std::vector<std::vector<fc::network::solana::account_meta>> build_dispatch_manifests(
   const fc::network::solana::solana_public_key& program_id,
   const std::vector<inbound_effect>&            effects,
   uint32_t                                      total_attestations,
   const std::function<void()>&                  throw_if_past_deadline,
   const transfer_hook_reader&                   read_transfer_hook,
   const liq_pool_reader&                        read_liq_pool,
   const std::string&                            log_label) {
   const auto& token_2022_program_id =
      fc::network::solana::system::program_ids::TOKEN_2022_PROGRAM;

   // One read per DISTINCT custody mint for the whole build, matching the two
   // caches around it. Without this every syndicated SPL payout pays a
   // `get_account_info(custody_mint)` -- two when the mint has a hook, since the
   // reader also reads the validation PDA -- so repeated payouts of one mint
   // would repeat those reads inside a build that probes the
   // deadline per effect and aborts wholesale on a transient RPC throw.
   // `nullopt` (no hook) is memoised too: that is every mint in the system today.
   std::map<fc::network::solana::solana_public_key, std::optional<mint_transfer_hook>> hook_cache;
   auto transfer_hook =
      [&](const fc::network::solana::solana_public_key& custody_mint)
      -> const std::optional<mint_transfer_hook>& {
      auto it = hook_cache.find(custody_mint);
      if (it != hook_cache.end()) return it->second;
      return hook_cache.emplace(custody_mint, read_transfer_hook(custody_mint)).first->second;
   };

   // One DistributionState read for the whole build -- the pool singleton is
   // the same for every DESYNDICATE_LIQ in the envelope -- memoised like the
   // caches above, an absent read included.
   std::optional<std::optional<liq_pool_info>> liq_pool_cache;
   auto liq_pool = [&]() -> const std::optional<liq_pool_info>& {
      if (!liq_pool_cache.has_value()) liq_pool_cache = read_liq_pool();
      return *liq_pool_cache;
   };

   std::vector<std::vector<fc::network::solana::account_meta>> per_attestation(total_attestations);
   for (const auto& effect : effects) {
      // The deadline is probed per effect, BEFORE its custody read: a build
      // that runs past the tick deadline must fail here, where the remaining
      // work is known, rather than deep inside the RPC layer with every
      // manifest already paid for and none of them dispatched.
      throw_if_past_deadline();

      if (effect.attestation_index >= per_attestation.size()) {
         // Defensive: the index comes from the same walk that produced
         // `total_attestations`, so this cannot happen without a decode
         // disagreeing with itself -- log it rather than writing out of range.
         elog("outpost_solana_client[{}]: effect index {} is past the envelope's {} attestations; "
              "skipping it rather than misaligning the dispatch cursor",
              log_label, effect.attestation_index, per_attestation.size());
         continue;
      }

      auto& metas = per_attestation[effect.attestation_index];
      auto  add   = [&](const fc::network::solana::solana_public_key& key, bool is_writable) {
         record_terminal_account(metas, key, is_writable);
      };

      // DESYNDICATE_LIQ: the depot releases a user's syndicated liqSOL. Everything
      // derives from the user's pubkey, the program's fixed pool PDAs and the mint
      // pinned on `DistributionState` -- the account the handler itself binds the
      // mint from.
      //
      // LOCK-STEP: `handle_desyndicate_liq`'s `require_remaining_account` list
      // (wire-solana `inbound.rs`) IS this manifest. The two state singletons come
      // first because the handler loads them before anything else and turns an
      // uninitialized pool into a logged skip; every later account aborts the
      // window when missing, which is the caller-fixable retry the program wants.
      //
      // The request's `PendingPayout` PDA follows them, writable, whether or not
      // the pool reads: `defer_desyndication` creates it when the outpost is
      // frozen (checked right after the singletons, before any pool account) or a
      // share record is still the legacy layout (after them), and aborts the
      // window with `EffectAccountMissing` when it is absent. The paid path never
      // touches it. Request id 0 cannot key one -- the handler records that payout
      // as unpaid before asking for the account -- so it gets none.
      if (effect.shape == effect_shape::desyndicate_liq) {
         if (!effect.recipient) continue;
         add(derive_liqsol_global_state_pda(program_id), true);
         add(derive_liqsol_distribution_state_pda(program_id), true);
         if (effect.desyndication_request_id.value_or(0) != 0) {
            add(derive_pending_desyndication_pda(program_id, *effect.desyndication_request_id), true);
         }

         const auto& pool_opt = liq_pool();
         if (!pool_opt.has_value()) {
            wlog("outpost_solana_client[{}]: DistributionState absent or empty while building the "
                 "terminal manifest for attestation {}; passing the state singletons only -- the "
                 "handler will log-and-skip an uninitialized pool",
                 log_label, effect.attestation_index);
            continue;
         }
         const auto& mint             = pool_opt->liqsol_mint;
         const auto  pool_authority   = derive_liqsol_pool_authority_pda(program_id);
         const auto  bucket_authority = derive_liqsol_bucket_authority_pda(program_id);
         const auto  pool_ata         = fc::network::solana::system::get_associated_token_address(
            pool_authority, mint, token_2022_program_id);
         const auto  user_ata         = fc::network::solana::system::get_associated_token_address(
            *effect.recipient, mint, token_2022_program_id);
         const auto  bucket_ata       = fc::network::solana::system::get_associated_token_address(
            bucket_authority, mint, token_2022_program_id);

         // The pool-authority-signed transfer and the share move it settles.
         add(pool_authority, false);
         add(pool_ata, true);
         add(user_ata, true);
         add(derive_liqsol_user_record_pda(program_id, pool_ata), true);
         add(derive_liqsol_user_record_pda(program_id, user_ata), true);
         add(bucket_ata, false);
         add(mint, false);
         add(token_2022_program_id, false);
         // The liqSOL transfer hook's accounts: `invoke_transfer_checked` resolves
         // the hook program, the mint's extra-metas PDA and the metas that PDA
         // declares out of `remaining_accounts`, and the handler requires the
         // bucket authority and liqsol-core itself on top.
         add(bucket_authority, false);
         add(program_id, false);
         const auto& hook = transfer_hook(mint);
         if (!hook.has_value()) {
            wlog("outpost_solana_client[{}]: liqSOL mint {} carries no transfer hook; the "
                 "DESYNDICATE_LIQ manifest for attestation {} omits the hook accounts the handler "
                 "requires, so its dispatch window aborts until the mint is fixed",
                 log_label, mint.to_string(fc::yield_function_t{}), effect.attestation_index);
            continue;
         }
         const auto validation_pda = derive_extra_account_metas_pda(hook->program, mint);
         add(hook->program, false);
         add(validation_pda, false);
         for (const auto& meta : resolve_hook_metas(hook->declared, hook->program, pool_ata, mint,
                                                    user_ata, pool_authority, validation_pda)) {
            add(meta.key, meta.is_writable);
         }
         continue;
      }

   }

   return per_attestation;
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

envelope_chunk_reap_gates decode_envelope_chunk_reap_gates(
   opp_solana_outpost_client& program_client,
   const std::vector<std::optional<fc::network::solana::account_info>>& infos) {
   FC_ASSERT(program_client.get_program() != nullptr, "no IDL program loaded");
   FC_ASSERT(infos.size() == REAP_GATE_ACCOUNT_COUNT, "getMultipleAccounts returned {} accounts for {} addresses",
             infos.size(), REAP_GATE_ACCOUNT_COUNT);
   const auto read_field = [&](const std::optional<fc::network::solana::account_info>& info,
                               const char* account_name, size_t offset) -> uint32_t {
      FC_ASSERT(info.has_value() && !info.value().data.empty(), "{} is absent or empty", account_name);
      const auto& account = info.value();
      FC_ASSERT(account.owner == program_client.program_id, "{} is owned by {} instead of program {}", account_name,
                account.owner.to_string(fc::yield_function_t{}),
                program_client.program_id.to_string(fc::yield_function_t{}));
      const auto* declaration = program_client.get_program()->find_account(account_name);
      FC_ASSERT(declaration != nullptr, "outpost program IDL declares no {}", account_name);
      FC_ASSERT(account.data.size() >= offset + sizeof(uint32_t), "{} cursor header is truncated", account_name);
      FC_ASSERT(declaration->discriminator.size() <= account.data.size() &&
                   std::equal(declaration->discriminator.begin(), declaration->discriminator.end(),
                              account.data.begin()),
                "{} discriminator disagrees with the loaded IDL", account_name);
      return le_u32(account.data.data() + offset);
   };
   return {
      .next_epoch_index =
         read_field(infos[0], outpost_config::account_name, outpost_config::epoch_offset),
      .latest_outbound_epoch = read_field(infos[1], latest_envelope::account_name, latest_envelope::epoch_offset),
   };
}

bool envelope_chunks_reapable(const envelope_chunk_reap_gates& gates, uint32_t epoch_index) {
   return gates.next_epoch_index > epoch_index && gates.latest_outbound_epoch >= epoch_index;
}

bool outpost_epoch_settled(const envelope_chunk_reap_gates& gates, uint32_t epoch_index) {
   return gates.next_epoch_index > epoch_index;
}

bool envelope_chunk_sweep_schedule::backing_off(fc::time_point now) const {
   return now < _retry_after;
}

bool envelope_chunk_sweep_schedule::due(uint32_t next_epoch_index, fc::time_point now) const {
   return !backing_off(now) && _swept_through != next_epoch_index;
}

bool envelope_chunk_sweep_schedule::record(uint32_t                          next_epoch_index,
                                           const envelope_chunk_reap_result& result,
                                           fc::time_point                    now) {
   _cut_off_close = result.cut_off_close;
   if (result.deadline_reached && result.reaped == 0 && result.already_closed == 0 && result.failed == 0) {
      return record_deadline_stop(now);
   }
   reset_deadline_stops();
   if (result.failed > 0) {
      record_failure(now);
      return false;
   }
   // Partial walks stay due without back-off.
   if (!result.complete()) return false;
   _swept_through = next_epoch_index;
   return false;
}

uint32_t envelope_chunk_sweep_schedule::reconcile_cut_off_close(
   const std::vector<envelope_chunk_buffer>& buffers) {
   if (!_cut_off_close) return 0;
   const bool missing = std::none_of(buffers.begin(), buffers.end(), [&](const auto& buffer) {
      return buffer.address == *_cut_off_close;
   });
   _cut_off_close.reset();
   return missing ? 1 : 0;
}

bool envelope_chunk_sweep_schedule::record_deadline_stop(fc::time_point now) {
   if (++_deadline_stops >= ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT) {
      _deadline_stops = 0;
      _retry_after = now + ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
      return true;
   }
   return false;
}

void envelope_chunk_sweep_schedule::reset_deadline_stops() {
   _deadline_stops = 0;
}

void envelope_chunk_sweep_schedule::record_failure(fc::time_point now) {
   reset_deadline_stops();
   _retry_after = now + ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF;
}

std::vector<fc::variant> envelope_chunk_buffer_filters(const std::array<uint8_t, 8>&                 discriminator,
                                                       const fc::network::solana::solana_public_key& uploader) {
   const auto memcmp = [](size_t offset, std::string base58_bytes) {
      return fc::variant(fc::mutable_variant_object()(
         memcmp_filter::key,
         fc::mutable_variant_object()(memcmp_filter::offset, offset)(memcmp_filter::bytes, std::move(base58_bytes))));
   };
   return {
      memcmp(0, fc::to_base58(reinterpret_cast<const char*>(discriminator.data()), discriminator.size(),
                              fc::yield_function_t{})),
      memcmp(envelope_chunks::operator_offset, uploader.to_string(fc::yield_function_t{})),
   };
}

fc::network::solana::data_slice envelope_chunk_buffer_header_slice() {
   return {.offset = 0, .length = envelope_chunks::data_offset};
}

std::vector<envelope_chunk_buffer> decode_envelope_chunk_buffers(
   opp_solana_outpost_client& program_client, const fc::variant& program_accounts,
   const fc::network::solana::solana_public_key& uploader) {
   const auto* program = program_client.get_program();
   FC_ASSERT(program != nullptr && program->find_account(envelope_chunks::account_name) != nullptr,
             "outpost program IDL declares no {}; this relay cannot find its chunk buffers",
             envelope_chunks::account_name);
   FC_ASSERT(program_accounts.is_array(), "getProgramAccounts did not return an array: {}",
             fc::json::to_string(program_accounts, fc::json::yield_function_t{}));

   std::vector<envelope_chunk_buffer> buffers;
   for (const fc::variant& entry_v : program_accounts.get_array()) {
      const auto& entry   = entry_v.get_object();
      const auto  address = fc::network::solana::solana_public_key::from_base58_string(
         entry[program_account_entry::field_pubkey].as_string());
      const auto& account = entry[program_account_entry::field_account].get_object();
      // `data` is `[<base64>, "base64"]`, the encoding the discovery read requests.
      const auto decoded_b64 =
         fc::base64_decode(account[program_account_entry::field_data].get_array().at(0).as_string());
      const std::vector<uint8_t> header(decoded_b64.begin(), decoded_b64.end());

      const fc::variant decoded =
         program_client.decode_account_info_data(envelope_chunks::account_name, header);
      const fc::variant_object& fields  = decoded.get_object();
      FC_ASSERT(fields.contains(envelope_chunks::field_epoch_index) && fields.contains(envelope_chunks::field_operator),
                "EnvelopeChunks decoded without '{}' / '{}'; the loaded IDL disagrees with the deployed program",
                envelope_chunks::field_epoch_index, envelope_chunks::field_operator);
      const uint64_t epoch_index = fields[envelope_chunks::field_epoch_index].as_uint64();
      const auto decoded_operator = fc::network::solana::solana_public_key::from_base58_string(
         fields[envelope_chunks::field_operator].as_string());

      // The memcmp filter already selected `operator == uploader`, so a buffer that decodes to another operator, an
      // out-of-range epoch, or an address its own (epoch, uploader) seeds do not derive can only be a decode that
      // disagrees with the program. A close built from it would fail its seeds check -- skip it, loudly.
      if (decoded_operator != uploader || epoch_index > std::numeric_limits<uint32_t>::max() ||
          derive_envelope_chunks_pda(program_client.program_id, static_cast<uint32_t>(epoch_index), uploader) !=
             address) {
         wlog("outpost_solana_client: EnvelopeChunks {} decoded as epoch={} operator={}, which does not derive that "
              "address for uploader {}; the loaded IDL disagrees with the program, leaving the buffer in place",
              address.to_string(fc::yield_function_t{}), epoch_index,
              decoded_operator.to_string(fc::yield_function_t{}), uploader.to_string(fc::yield_function_t{}));
         continue;
      }
      buffers.push_back(envelope_chunk_buffer{address, static_cast<uint32_t>(epoch_index),
                                              account[program_account_entry::field_lamports].as_uint64()});
   }
   std::ranges::sort(buffers, {}, &envelope_chunk_buffer::epoch_index);
   return buffers;
}

envelope_chunk_reap_result drive_envelope_chunk_reap(
   const envelope_chunk_reap_gates&                     gates,
   std::vector<envelope_chunk_buffer>                   buffers,
   const std::function<bool()>&                         deadline_passed,
   const std::function<std::string(uint32_t)>&          send_cleanup,
   const std::string&                                   log_label,
   const std::function<bool(const envelope_chunk_buffer&)>& buffer_absent) {
   envelope_chunk_reap_result result;
   for (const auto& buffer : buffers) {
      // One confirmed snapshot observes drain and emit atomically; rejected epochs have not drained yet.
      if (!envelope_chunks_reapable(gates, buffer.epoch_index)) {
         ++result.not_yet_eligible;
         continue;
      }
      if (result.deadline_reached ||
          result.reaped + result.already_closed + result.failed >= MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP) {
         ++result.deferred;
         continue;
      }
      if (deadline_passed()) {
         result.deadline_reached = true;
         ++result.deferred;
         continue;
      }
      // A close that throws after the deadline passed was cut short by it -- its confirmation polling and the RPC
      // transport both stop at the scoped deadline -- so it is left for the next sweep rather than counted as a failed
      // close. It may even have landed; the next discovery read no longer lists the buffer if it did.
      const auto close_threw = [&](const std::string& error) {
         // A generic Anchor AccountNotInitialized can refer to config or the latest envelope as well.
         // Verify this exact buffer instead of parsing error strings. nullopt represents absence, but libfc also
         // returns it for a malformed reply. That can delay recovery until a later discovery; no rent is claimed.
         // Empty data is still present, and thrown read errors preserve failure. The callback shares the deadline.
         if (!deadline_passed() && buffer_absent) {
            try {
               if (buffer_absent(buffer)) {
                  ++result.already_closed;
                  ilog("outpost_solana_client[{}]: cleanup buffer={} epoch={} already closed; rent not attributed",
                       log_label, buffer.address.to_string(fc::yield_function_t{}), buffer.epoch_index);
                  return;
               }
            } catch (const fc::exception& e) {
               dlog("outpost_solana_client[{}]: cleanup absence check failed buffer={}: {}",
                    log_label, buffer.address.to_string(fc::yield_function_t{}), e.to_detail_string());
            } catch (const std::exception& e) {
               dlog("outpost_solana_client[{}]: cleanup absence check failed buffer={}: {}",
                    log_label, buffer.address.to_string(fc::yield_function_t{}), e.what());
            }
         }
         if (deadline_passed()) {
            result.deadline_reached = true;
            result.cut_off_close = buffer.address;
            ++result.deferred;
            ilog("outpost_solana_client[{}]: cleanup_envelope_chunks for epoch={} buffer={} ran into the sweep "
                 "deadline: {}; the next sweep retries it if it did not land",
                 log_label, buffer.epoch_index, buffer.address.to_string(fc::yield_function_t{}), error);
            return;
         }
         ++result.failed;
         wlog("outpost_solana_client[{}]: cleanup_envelope_chunks failed for epoch={} buffer={}: {}; the next sweep "
              "retries it",
              log_label, buffer.epoch_index, buffer.address.to_string(fc::yield_function_t{}), error);
      };
      try {
         const auto sig = send_cleanup(buffer.epoch_index);
         ++result.reaped;
         result.lamports_reaped += buffer.lamports;
         ilog("outpost_solana_client[{}]: cleanup_envelope_chunks closed epoch={} buffer={} returning {} lamports "
              "sig={}",
              log_label, buffer.epoch_index, buffer.address.to_string(fc::yield_function_t{}), buffer.lamports, sig);
      } catch (const fc::exception& e) {
         close_threw(e.to_detail_string());
      } catch (const std::exception& e) {
         close_threw(e.what());
      }
   }
   return result;
}

envelope_chunk_sweep_outcome sweep_envelope_chunks(envelope_chunk_sweep_schedule&                  schedule,
                                                   fc::time_point                                  sweep_start,
                                                   fc::time_point                                  deadline_abs,
                                                   const std::optional<envelope_chunk_reap_gates>& gates,
                                                   const envelope_chunk_sweep_touchpoints&         touchpoints,
                                                   const std::string&                              log_label) {
   // Both checks precede every RPC: a backing-off schedule and a spent deadline each cost nothing.
   if (schedule.backing_off(touchpoints.now())) return envelope_chunk_sweep_outcome::backing_off;
   if (touchpoints.now() >= deadline_abs) {
      dlog("outpost_solana_client[{}]: no deadline left for the chunk-buffer sweep; the next call runs it", log_label);
      return envelope_chunk_sweep_outcome::deadline_spent;
   }
   bool discovery_complete = false;
   const auto deadline_passed = [&] { return touchpoints.now() >= deadline_abs; };
   /// Attribute the back-off warning to this client for both thrown reads and returned walks.
   const auto warn_deadline_backoff = [&] {
      wlog("outpost_solana_client[{}]: chunk-buffer sweep reached {} consecutive deadline stops without progress; "
           "sweeps retry after {} s",
           log_label, ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT,
           ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF.to_seconds());
   };

   /// Only attribute read exhaustion to the endpoint when it had the full sweep budget.
   const auto warn_discovery_exhausted = [&] {
      if (deadline_abs - sweep_start >= ENVELOPE_CHUNK_SWEEP_BUDGET) {
         wlog("outpost_solana_client[{}]: chunk-buffer gate/discovery reads exhausted the sweep budget "
              "({} s) before any close attempt; the reads alone used the full budget",
              log_label, ENVELOPE_CHUNK_SWEEP_BUDGET.to_seconds());
      } else {
         ilog("outpost_solana_client[{}]: chunk-buffer gate/discovery reads reached the deadline; "
              "the caller or outer scope left less than the full {} s sweep budget",
              log_label, ENVELOPE_CHUNK_SWEEP_BUDGET.to_seconds());
      }
   };

   // A throw is a failed sweep -- one that backs off -- only while the deadline still had time on it. A read the
   // deadline cut short (the RPC transport and the confirmation polling both stop at the scoped deadline and throw)
   // counts toward a consecutive-stop limit, so one tight tick does not stand every later sweep down.
   // Repeated slow reads stand down every sweep; all paths share the same short budget.
   // The test is the clock, not the exception type: an RPC's own connect or read timeout also throws
   // `fc::timeout_exception`, and a dead endpoint must back off.
   const auto sweep_threw = [&](const std::string& error) {
      if (deadline_passed()) {
         if (!discovery_complete) warn_discovery_exhausted();
         ilog("outpost_solana_client[{}]: chunk-buffer sweep stopped at its deadline: {}",
              log_label, error);
         if (schedule.record_deadline_stop(touchpoints.now())) warn_deadline_backoff();
         return envelope_chunk_sweep_outcome::cut_short;
      }
      schedule.record_failure(touchpoints.now());
      wlog("outpost_solana_client[{}]: chunk-buffer sweep failed: {}; the buffers stay on chain and the sweep "
           "retries them after {} s",
           log_label, error, ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF.to_seconds());
      return envelope_chunk_sweep_outcome::failed;
   };
   try {
      const auto cursors = gates.has_value() ? *gates : touchpoints.read_gates();
      if (deadline_passed()) {
         warn_discovery_exhausted();
         if (schedule.record_deadline_stop(touchpoints.now())) warn_deadline_backoff();
         return envelope_chunk_sweep_outcome::cut_short;
      }
      // Not due: no epoch has settled since the last complete sweep, so nothing new can be reapable.
      if (!schedule.due(cursors.next_epoch_index, touchpoints.now())) {
         schedule.reset_deadline_stops();
         return envelope_chunk_sweep_outcome::not_due;
      }

      const auto buffers = touchpoints.read_buffers();
      discovery_complete = true;
      if (deadline_passed()) warn_discovery_exhausted();
      const auto already_closed = schedule.reconcile_cut_off_close(buffers);
      auto result = drive_envelope_chunk_reap(cursors, buffers, deadline_passed,
                                              touchpoints.send_cleanup, log_label, touchpoints.buffer_absent);
      result.already_closed += already_closed;
      if (result.reaped + result.already_closed + result.failed + result.deferred > 0) {
         ilog("outpost_solana_client[{}]: chunk-buffer sweep at next_epoch={} closed {} ({} lamports), failed {}, "
              "already closed {} (rent not attributed), deferred {}{}, {} not yet settled",
              log_label, cursors.next_epoch_index, result.reaped, result.lamports_reaped, result.failed,
              result.already_closed,
              result.deferred, result.deadline_reached ? " (stopped at the deadline)" : "", result.not_yet_eligible);
      }
      if (schedule.record(cursors.next_epoch_index, result, touchpoints.now())) warn_deadline_backoff();
      return envelope_chunk_sweep_outcome::swept;
   } catch (const fc::exception& e) {
      return sweep_threw(e.to_detail_string());
   } catch (const std::exception& e) {
      return sweep_threw(e.what());
   }
}

bool skip_settled_epoch_delivery(uint32_t                                                     epoch_index,
                                 const std::function<envelope_chunk_reap_gates()>&            read_gates,
                                 const std::function<void(const envelope_chunk_reap_gates&)>& sweep,
                                 const std::string&                                           log_label) {
   // Never let the settled check block delivery: an unread cursor means "not known settled", so upload as usual.
   const auto check_failed = [&](const std::string& error) {
      wlog("outpost_solana_client[{}]: settled-epoch check for epoch={} failed: {}; delivering normally", log_label,
           epoch_index, error);
   };
   std::optional<envelope_chunk_reap_gates> gates;
   try {
      gates = read_gates();
   } catch (const fc::exception& e) {
      check_failed(e.to_detail_string());
      return false;
   } catch (const std::exception& e) {
      check_failed(e.what());
      return false;
   }
   if (!outpost_epoch_settled(*gates, epoch_index)) return false;

   ilog("outpost_solana_client[{}]: epoch_in epoch={} skipped; the outpost already settled it (next_epoch={})",
        log_label, epoch_index, gates->next_epoch_index);
   sweep(*gates);
   return true;
}

std::vector<char> read_envelope_then_sweep_when_idle(const std::function<std::vector<char>()>& read_envelope,
                                                     const std::function<void(fc::time_point)>& sweep,
                                                     fc::time_point                            deadline_abs) {
   auto envelope = read_envelope();
   if (envelope.empty()) sweep(deadline_abs);
   return envelope;
}

std::optional<fc::sha256> decode_envelope_log_digest(opp_solana_outpost_client&  program_client,
                                                     const std::vector<uint8_t>& account_data,
                                                     uint32_t                    epoch_index) {
   const fc::variant   decoded   = program_client.decode_account_info_data(envelope_log::account_name, account_data);
   const fc::variants& envelopes = decoded.get_object()[envelope_log::field_envelopes].get_array();
   for (const fc::variant& entry : envelopes) {
      const fc::variant_object& record = entry.get_object();
      if (record[envelope_log::field_epoch].as_uint64() != epoch_index) continue;
      const fc::variants& checksum = record[envelope_log::field_checksum].get_array();
      FC_ASSERT(checksum.size() == sizeof(fc::sha256), "EnvelopeLog record for epoch {} carries a {}-byte checksum",
                epoch_index, checksum.size());
      fc::sha256 digest;
      for (size_t i = 0; i < checksum.size(); ++i) {
         const uint64_t byte = checksum[i].as_uint64();
         FC_ASSERT(byte <= std::numeric_limits<uint8_t>::max(), "EnvelopeLog checksum byte {} is out of range",
                   byte);
         digest.data()[i] = static_cast<char>(byte);
      }
      return digest;
   }
   return std::nullopt;
}

uint16_t decode_first_unstaged_chunk(opp_solana_outpost_client&                              program_client,
                                     const std::optional<fc::network::solana::account_info>& buffer,
                                     uint32_t                                                epoch_index,
                                     uint16_t                                                total_chunks,
                                     uint32_t                                                total_bytes,
                                     const fc::network::solana::solana_public_key&           uploader) {
   // Absent, or still system-owned before its first chunk created it: nothing staged.
   if (!buffer.has_value() || buffer->data.empty() || buffer->owner != program_client.program_id) return 0;
   const auto* program = program_client.get_program();
   if (program == nullptr || program->find_account(envelope_chunks::account_name) == nullptr) return 0;

   // The account is bytemuck `repr(C)` with explicit padding, so its header decodes byte-for-byte as the Borsh
   // fields the IDL declares; the staged payload tail past the header is simply not read.
   const fc::variant         decoded = program_client.decode_account_info_data(envelope_chunks::account_name,
                                                                               buffer->data);
   const fc::variant_object& header  = decoded.get_object();
   // Name the drifted field rather than fail on a bare missing-key lookup: the caller turns any throw here into a
   // resume from 0, so this message is the only trace of an IDL that silently disabled the resume.
   for (const auto field : envelope_chunks::fields_read) {
      FC_ASSERT(header.contains(field),
                "EnvelopeChunks decoded without '{}'; the loaded IDL disagrees with the deployed program", field);
   }
   // The PDA seeds bind the epoch and the uploader, so a buffer at this address that disagrees on either can only
   // be a decode that disagrees with the program -- IDL drift that still decodes, e.g. a dropped `_padding` reading
   // `operator` 3 bytes off. Warn: it silently disables the resume otherwise.
   const uint64_t decoded_epoch    = header[envelope_chunks::field_epoch_index].as_uint64();
   const auto     decoded_operator = fc::network::solana::solana_public_key::from_base58_string(
      header[envelope_chunks::field_operator].as_string());
   if (decoded_epoch != epoch_index || decoded_operator != uploader) {
      wlog("outpost_solana_client: EnvelopeChunks for epoch={} decoded as epoch={} operator={}, but its address is "
           "seeded by epoch={} uploader={}; the loaded IDL disagrees with the program, uploading from chunk 0",
           epoch_index, decoded_epoch, decoded_operator.to_string(fc::yield_function_t{}), epoch_index,
           uploader.to_string(fc::yield_function_t{}));
      return 0;
   }
   // A different `total_chunks` / `total_bytes` is a real shape change, which the program itself reports as
   // `ChunkBufferShapeMismatch` on chunk 0.
   if (header[envelope_chunks::field_total_chunks].as_uint64() != total_chunks ||
       header[envelope_chunks::field_total_bytes].as_uint64() != total_bytes) {
      return 0;
   }
   // Never resume past what the account's length can hold. A misread `received_chunks` (an IDL whose field order
   // drifted from the program) would otherwise start every tick at a chunk the program rejects as out of order.
   const size_t   staged_bytes   = buffer->data.size() > envelope_chunks::data_offset
                                      ? buffer->data.size() - envelope_chunks::data_offset
                                      : 0;
   const uint64_t chunks_by_size = (staged_bytes + SOLANA_MAX_CHUNK_BYTES - 1) / SOLANA_MAX_CHUNK_BYTES;
   const uint64_t received       = header[envelope_chunks::field_received_chunks].as_uint64();
   if (received > chunks_by_size) {
      // The program sizes the account to exactly the bytes received, so this too means the decode disagrees with it.
      wlog("outpost_solana_client: EnvelopeChunks for epoch={} claims received_chunks={} but holds only {} staged "
           "bytes ({} chunks); the loaded IDL disagrees with the program, resuming at chunk {}",
           epoch_index, received, staged_bytes, chunks_by_size, std::min<uint64_t>(chunks_by_size, total_chunks));
   }
   return static_cast<uint16_t>(std::min<uint64_t>({received, chunks_by_size, uint64_t{total_chunks}}));
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
   if (role == solana_outpost_role::underwriter) {
      FC_ASSERT(_program_client->get_program(),
                "outpost_solana_client: no IDL program loaded; cannot validate the EnvelopeLog declaration");
      outpost_solana_client_detail::assert_envelope_log_shape(*_program_client->get_program());
   }
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
      // The instruction existing is not enough: the crank is driven from the
      // `EpochDeliveries` cursor, so a PRESENT but drifted declaration (a
      // renamed or dropped `consensus_reached` / `dispatched_count`) would
      // leave every drain reading "consensus not reached, cursor at 0" behind
      // a dlog -- the outpost would stop settling with no error anywhere.
      // Fail at boot instead, where the IDL is still fixable.
      outpost_solana_client_detail::assert_epoch_deliveries_shape(*_program_client->get_program());
      // Every delivery stages an `EnvelopeChunks` buffer the relay pays rent for, and only the reaper returns it.
      // Require cleanup declarations and validate the header offsets used by discovery before the first sweep.
      FC_ASSERT(_program_client->has_idl(CLEANUP_ENVELOPE_CHUNKS_INSTRUCTION),
                "outpost program IDL lacks `{}`; this relay could never return its chunk-buffer rent",
                CLEANUP_ENVELOPE_CHUNKS_INSTRUCTION);
      for (const auto* account_name : {envelope_chunks::account_name, outpost_config::account_name}) {
         FC_ASSERT(_program_client->get_program()->find_account(account_name),
                   "outpost program IDL declares no {}; the chunk-buffer reaper reads it", account_name);
      }
      outpost_solana_client_detail::assert_envelope_chunks_shape(*_program_client);
      outpost_solana_client_detail::assert_envelope_chunk_gate_shapes(*_program_client);
      // `DistributionState` is read only for DESYNDICATE_LIQ manifests, and only
      // the integrated liqsol-core program declares it -- a standalone outpost
      // IDL has no syndicated pool to release from, and its DistributionState
      // PDA never exists on chain. Where it IS declared, a drifted `liqsol_mint`
      // would wedge every desyndication window.
      if (_program_client->get_program()->find_account(distribution_state::account_name)) {
         outpost_solana_client_detail::assert_distribution_state_shape(
            *_program_client->get_program());
      } else {
         ilog("outpost_solana_client[{}]: IDL declares no DistributionState; DESYNDICATE_LIQ "
              "manifests are not derivable on this outpost program",
              to_string());
      }
      // The yield-report crank exists only where the program declares it. There,
      // it gates on `GlobalState.wire_state`, `frozen` and `paused`, and derives its accounts from
      // `DistributionState`, so both declarations are boot-checked with it.
      if (_program_client->has_idl(REPORT_LIQ_YIELD_INSTRUCTION)) {
         FC_ASSERT(_program_client->get_program()->find_account(distribution_state::account_name),
                   "outpost program IDL declares `{}` but no DistributionState; the crank "
                   "derives the pool accounts from it",
                   REPORT_LIQ_YIELD_INSTRUCTION);
         outpost_solana_client_detail::assert_global_state_shape(*_program_client->get_program());
      } else {
         ilog("outpost_solana_client[{}]: IDL declares no `{}`; the yield-report crank is idle on "
              "this outpost program",
              to_string(), REPORT_LIQ_YIELD_INSTRUCTION);
      }
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

   const auto decoded =
      _program_client->decode_account_info_data(epoch_deliveries::account_name, account_info->data);
   const auto& row = decoded.get_object();
   // Both fields are boot-validated for this role (`assert_epoch_deliveries_shape`),
   // so their absence here means the loaded IDL changed under a running relay.
   // Assert rather than default: a defaulted `consensus_reached` reads as "not
   // reached" and would no-op the crank on every tick with only a dlog, which
   // is exactly the silent epoch stall the boot check exists to prevent.
   FC_ASSERT(row.contains(epoch_deliveries::field_consensus_reached) &&
                row.contains(epoch_deliveries::field_dispatched_count),
             "EpochDeliveries decoded without '{}' / '{}'; the loaded IDL disagrees with the "
             "deployed program and the dispatch cursor cannot be read",
             epoch_deliveries::field_consensus_reached, epoch_deliveries::field_dispatched_count);
   progress.consensus_reached = row[epoch_deliveries::field_consensus_reached].as_bool();
   progress.dispatched_count =
      static_cast<uint32_t>(row[epoch_deliveries::field_dispatched_count].as_uint64());
   return progress;
}

uint16_t outpost_solana_client::read_first_unstaged_chunk(uint32_t epoch_index, uint16_t total_chunks,
                                                          uint32_t total_bytes) {
   // Starting at 0 never skips data (the program no-ops staged chunks), so a failure anywhere in the lookup costs
   // only the resume, never the delivery: an RPC error must not stop this tick from uploading.
   const auto upload_from_zero = [&](const std::string& reason) -> uint16_t {
      wlog("outpost_solana_client[{}]: cannot read staged EnvelopeChunks for epoch={}: {}; uploading from chunk 0",
           to_string(), epoch_index, reason);
      return 0;
   };
   try {
      const auto uploader   = _entry->client->get_pubkey();
      const auto buffer_pda = derive_envelope_chunks_pda(_program_id, epoch_index, uploader);
      // `processed`, to match the chunk writes `execute_tx_and_confirm` waits for. A processed read that later
      // rolls back over-reports; the next chunk then fails `ChunkOutOfOrder` and the following tick re-reads. A
      // lagging read under-reports, which only re-sends no-op chunks.
      const auto buffer = _entry->client->get_account_info(buffer_pda, fc::network::solana::commitment_t::processed);
      return outpost_solana_client_detail::decode_first_unstaged_chunk(*_program_client, buffer, epoch_index,
                                                                      total_chunks, total_bytes, uploader);
   } catch (const fc::exception& e) {
      return upload_from_zero(e.to_detail_string());
   } catch (const std::exception& e) {
      return upload_from_zero(e.what());
   }
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
   };
   const auto& instr = _program_client->get_idl("dispatch_attestations");
   fc::network::solana::program_invoke_data_items params = {
      fc::variant(epoch_index),
      fc::variant(dispatch_limit),
   };
   // The complete producer-tested budget rides with the work it protects:
   // every round decodes the staged envelope and dispatches effects, while the
   // draining round also encodes the outbound emit. The helper is shared with
   // the packet regression so the tested preamble is the submitted preamble.
   auto pre_ixs = outpost_solana_client_detail::relay_compute_budget_pre_instructions();
   auto accounts = _program_client->resolve_accounts(instr, params, overrides);
   accounts.reserve(accounts.size() + extra_remaining_accounts.size());
   accounts.insert(accounts.end(), extra_remaining_accounts.begin(),
                   extra_remaining_accounts.end());
   return _program_client->execute_tx_and_confirm(instr, accounts, params, pre_ixs);
}

std::vector<std::optional<fc::network::solana::account_info>> outpost_solana_client::read_chunk_reap_accounts(
   const std::vector<fc::network::solana::solana_public_key>& accounts, fc::network::solana::commitment_t commitment,
   const std::optional<fc::network::solana::data_slice>& slice) {
   return _entry->client->get_multiple_accounts(accounts, commitment, slice);
}

fc::variant outpost_solana_client::read_chunk_program_accounts(
   const fc::network::solana::solana_public_key& program_id, const std::vector<fc::variant>& filters,
   fc::network::solana::commitment_t commitment, const std::optional<fc::network::solana::data_slice>& slice) {
   return _entry->client->get_program_accounts(program_id, filters, commitment, slice);
}

outpost_solana_client_detail::envelope_chunk_reap_gates outpost_solana_client::read_envelope_chunk_reap_gates() {
   // One confirmed bank observes the atomic drain and riding emit together.
   const auto infos = read_chunk_reap_accounts(
      {_program_client->config_pda, _program_client->latest_outbound_envelope_pda},
      fc::network::solana::commitment_t::confirmed,
      fc::network::solana::data_slice{.offset = 0, .length = REAP_GATE_HEADER_BYTES});
   return outpost_solana_client_detail::decode_envelope_chunk_reap_gates(*_program_client, infos);
}

std::vector<outpost_solana_client_detail::envelope_chunk_buffer>
outpost_solana_client::read_own_envelope_chunk_buffers() {
   const auto* program = _program_client->get_program();
   FC_ASSERT(program != nullptr, "no IDL program loaded");
   const auto* chunks_account = program->find_account(envelope_chunks::account_name);
   FC_ASSERT(chunks_account != nullptr, "outpost program IDL declares no {}", envelope_chunks::account_name);

   const auto uploader = _entry->client->get_pubkey();
   const auto result   = read_chunk_program_accounts(
      _program_id, outpost_solana_client_detail::envelope_chunk_buffer_filters(chunks_account->discriminator, uploader),
      fc::network::solana::commitment_t::confirmed, outpost_solana_client_detail::envelope_chunk_buffer_header_slice());
   return outpost_solana_client_detail::decode_envelope_chunk_buffers(*_program_client, result, uploader);
}

std::optional<fc::network::solana::account_info> outpost_solana_client::read_envelope_chunk_buffer(
   const fc::network::solana::solana_public_key& address, fc::network::solana::commitment_t commitment) {
   return _entry->client->get_account_info(address, commitment);
}

void outpost_solana_client::reap_settled_envelope_chunks(
   fc::time_point                                                               deadline_abs,
   const std::optional<outpost_solana_client_detail::envelope_chunk_reap_gates>& gates) {
   const auto sweep_start = fc::time_point::now();
   deadline_abs = fc::task::clamp_to_current_deadline(
      std::min(deadline_abs, sweep_start + outpost_solana_client_detail::ENVELOPE_CHUNK_SWEEP_BUDGET));
   // Every RPC the sweep makes -- the gate read, the discovery read, each close and its confirmation polling -- runs
   // under this scope, so none outlives `deadline_abs` (a nested scope only ever shortens the caller's).
   fc::task::deadline_scope rpc_deadline(deadline_abs);
   outpost_solana_client_detail::sweep_envelope_chunks(
      _chunk_sweep_schedule, sweep_start, deadline_abs, gates,
      {
         .now          = &fc::time_point::now,
         .read_gates   = [this] { return read_envelope_chunk_reap_gates(); },
         .read_buffers = [this] { return read_own_envelope_chunk_buffers(); },
         .send_cleanup = [this](uint32_t epoch_index) { return _program_client->cleanup_envelope_chunks(epoch_index); },
         .buffer_absent = [this](const outpost_solana_client_detail::envelope_chunk_buffer& buffer) {
            return !read_envelope_chunk_buffer(buffer.address, ENVELOPE_CHUNK_ABSENCE_COMMITMENT).has_value();
         },
      },
      to_string());
}

std::string outpost_solana_client::drain_dispatch(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);
   std::string last_sig;

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
   // empty entry so the indices stay aligned. The build reads the syndicated
   // pool once, probes the deadline per effect, and degrades a
   // single absent account instead of abandoning the envelope's other
   // manifests.
   const auto effects = outpost_solana_client_detail::extract_inbound_effects(envelope_bytes);
   const uint32_t total_attestations =
      outpost_solana_client_detail::count_inbound_attestations(envelope_bytes);
   const auto per_attestation = outpost_solana_client_detail::build_dispatch_manifests(
      _program_id,
      effects,
      total_attestations,
      [&] { throw_if_past_deadline(deadline_abs, OP_DISPATCH_ATTESTATIONS); },
      [&](const fc::network::solana::solana_public_key& custody_mint) {
         return mint_transfer_hook_for(custody_mint);
      },
      [&] { return syndicated_liq_pool(); },
      to_string());

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

/// Read one custody mint's transfer-hook configuration off chain.
///
/// Called only for a Token-2022 custody mint -- a legacy-SPL mint cannot
/// carry the extension, so the caller gates and this never reads that mint.
///
/// No TransferHook extension, or a hook explicitly disabled (an all-zero
/// program id), returns `nullopt` and leaves the manifest unchanged -- which is
/// every mint in the system today. An ABSENT mint does not: see below.
///
/// A hook that IS configured but whose validation PDA cannot be read is NOT
/// degraded to `nullopt`: that would silently emit a manifest short the very
/// accounts the CPI is about to demand, wedging the epoch with no diagnostic.
/// Throwing surfaces it against the account we could not read.
std::optional<outpost_solana_client_detail::mint_transfer_hook>
outpost_solana_client::mint_transfer_hook_for(
   const fc::network::solana::solana_public_key& custody_mint) {
   // NOT degraded to `nullopt`: the custody mint is pinned on a syndicated pool that
   // demonstrably exists, so "absent" is an anomaly (a lagging bank behind the
   // default commitment, say), not a mint without a hook. Reporting "no hook"
   // here would build a hook-free manifest for a hook mint and abort inside the
   // CPI with no local signal -- the same asymmetry this function already
   // refuses for the validation PDA below.
   const auto mint_info = _entry->client->get_account_info(custody_mint);
   FC_ASSERT(mint_info.has_value() && !mint_info->data.empty(),
             "custody mint {} is absent or empty; it is pinned on an existing syndicated pool, so this "
             "is a read anomaly rather than a mint without a transfer hook",
             custody_mint.to_string(fc::yield_function_t{}));

   const auto hook_program =
      outpost_solana_client_detail::mint_transfer_hook_program(mint_info->data);
   if (!hook_program.has_value()) return std::nullopt;

   const auto validation_pda =
      outpost_solana_client_detail::derive_extra_account_metas_pda(*hook_program, custody_mint);
   const auto validation_info = _entry->client->get_account_info(validation_pda);
   FC_ASSERT(validation_info.has_value() && !validation_info->data.empty(),
             "custody mint {} declares transfer hook {}, but its ExtraAccountMetaList {} is "
             "absent or empty; the dispatch manifest cannot be completed",
             custody_mint.to_string(fc::yield_function_t{}),
             hook_program->to_string(fc::yield_function_t{}),
             validation_pda.to_string(fc::yield_function_t{}));

   return outpost_solana_client_detail::mint_transfer_hook{
      *hook_program,
      outpost_solana_client_detail::parse_extra_account_metas(validation_info->data)};
}

std::optional<outpost_solana_client_detail::liq_pool_info>
outpost_solana_client::syndicated_liq_pool() {
   const auto state_pda =
      outpost_solana_client_detail::derive_liqsol_distribution_state_pda(_program_id);
   const auto pda_label = state_pda.to_string(fc::yield_function_t{});

   // An RPC/deadline exception is not evidence that the pool is absent, so the
   // read deliberately sits outside the decode-only try/catch.
   const auto account_info = _entry->client->get_account_info(state_pda);
   if (!account_info.has_value() || account_info->data.empty()) {
      wlog("outpost_solana_client[{}]: DistributionState absent or empty at {}; DESYNDICATE_LIQ "
           "manifests carry the state singletons only -- the handler log-and-skips an "
           "uninitialized pool",
           to_string(), pda_label);
      return std::nullopt;
   }

   try {
      const auto state_v = _program_client->decode_account_info_data(
         distribution_state::account_name, account_info->data);
      const auto& state = state_v.get_object();
      FC_ASSERT(state.contains(distribution_state::field_liqsol_mint),
                "DistributionState account missing '{}' field",
                distribution_state::field_liqsol_mint);
      return outpost_solana_client_detail::liq_pool_info{
         fc::network::solana::solana_public_key::from_base58_string(
            state[distribution_state::field_liqsol_mint].as_string())};
   } catch (const fc::exception& e) {
      elog("outpost_solana_client[{}]: DistributionState at {} EXISTS ({} bytes) but this relay "
           "cannot read its liqSOL mint; refusing to build a manifest the program is guaranteed to "
           "abort on. The dispatch cursor is left untouched and this epoch cannot settle until the "
           "cause is fixed -- check the loaded IDL against the deployed program: {}",
           to_string(), pda_label, account_info->data.size(), e.to_detail_string());
      throw;
   }
}

void outpost_solana_client::crank_outpost(uint32_t epoch_index, fc::microseconds deadline) {
   // A standalone outpost program has no syndicated pool to report on.
   if (!_program_client->has_idl(REPORT_LIQ_YIELD_INSTRUCTION)) return;

   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);
   throw_if_past_deadline(deadline_abs, OP_REPORT_LIQ_YIELD);

   // Fetch, decode and decide through the same path exercised by the crank tests.
   const auto global_state_pda =
      outpost_solana_client_detail::derive_liqsol_global_state_pda(_program_id);
   const auto pda_label = global_state_pda.to_string(fc::yield_function_t{});
   const auto state = outpost_solana_client_detail::drive_liq_yield_report(
      [&] { return _entry->client->get_account_info(global_state_pda); },
      [&](const std::vector<uint8_t>& data) {
         try {
            return _program_client->decode_account_info_data(
               outpost_solana_client_detail::global_state::account_name, data);
         } catch (const fc::exception& e) {
            elog("outpost_solana_client[{}]: GlobalState at {} EXISTS ({} bytes) but this relay "
                 "cannot decode it; refusing report_liq_yield -- check the loaded IDL against "
                 "the deployed program: {}",
                 to_string(), pda_label, data.size(), e.to_detail_string());
            throw;
         }
      },
      [&](const std::vector<fc::network::solana::instruction>& pre_instructions) {
         const auto pool = syndicated_liq_pool();
         if (!pool) return; // already logged: no pool, nothing to report
         const auto hook = mint_transfer_hook_for(pool->liqsol_mint);
         if (!hook) {
            wlog("outpost_solana_client[{}]: liqSOL mint {} carries no transfer hook; report_liq_yield "
                 "needs the hook's accounts and is skipped until the mint is fixed",
                 to_string(), pool->liqsol_mint.to_string(fc::yield_function_t{}));
            return;
         }
         throw_if_past_deadline(deadline_abs, OP_REPORT_LIQ_YIELD);
         const auto overrides = outpost_solana_client_detail::report_liq_yield_overrides(
            _program_id, pool->liqsol_mint, hook->program, _program_client->config_pda,
            _program_client->outbound_message_buffer_pda);
         const auto& instr = _program_client->get_idl(REPORT_LIQ_YIELD_INSTRUCTION);
         // The instruction takes no arguments; the cranker is this operator's own key.
         const fc::network::solana::program_invoke_data_items params;
         const auto accounts = _program_client->resolve_accounts(instr, params, overrides);
         const auto sig = _program_client->execute_tx_and_confirm(instr, accounts, params, pre_instructions);
         ilog("outpost_solana_client[{}]: report_liq_yield sent for epoch {} sig={}",
              to_string(), epoch_index, sig);
      });
   using report_state = outpost_solana_client_detail::liq_yield_report_state;
   switch (state) {
      case report_state::no_account:
         dlog("outpost_solana_client[{}]: no GlobalState at {} -- no liq yield to report", to_string(), pda_label);
         break;
      case report_state::frozen:
      case report_state::paused:
         ilog("outpost_solana_client[{}]: outpost is {} (GlobalState.{}) -- report_liq_yield is "
              "not sent for epoch {}",
              to_string(), magic_enum::enum_name(state), magic_enum::enum_name(state), epoch_index);
         break;
      case report_state::not_due:
         dlog("outpost_solana_client[{}]: outpost is not PostLaunch -- no liq yield to report", to_string());
         break;
      case report_state::due:
         break;
   }
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

   // An epoch the outpost has already settled has nothing left to deliver: the program answers every `epoch_in` for
   // it with a late no-op. This check has to come before the staged-chunk read, because the sweep closes a settled
   // epoch's buffer -- a depot consensus retry for that epoch would then find no buffer, read 0 chunks staged, and
   // re-send every chunk plus the terminal call as paid no-ops, overrunning the delivery deadline on a large envelope
   // every tick. An empty tx id tells the job the outpost already finalized the epoch.
   //
   // The gates are read at `confirmed`, the commitment the sweep reads them at. A buffer is reaped only after a
   // `confirmed` read showed its epoch settled, and a later `confirmed` read from the same node normally still shows it
   // settled, so a retry for a reaped epoch takes this branch and never reaches the re-upload below. That assumes one
   // RPC node, or nodes that agree: `confirmed` can still revert, and a load-balanced endpoint can answer from a node
   // that is behind. When such a read shows a reaped epoch open, that one retry re-sends its chunks and terminal call
   // as paid late no-ops; a later retry reads the cursor past the epoch again and takes this branch.
   if (outpost_solana_client_detail::skip_settled_epoch_delivery(
          epoch_index, [this] {
             // This optional read must leave time for upload even when the endpoint stalls.
             fc::task::deadline_scope check_deadline(
                fc::time_point::now() + outpost_solana_client_detail::ENVELOPE_CHUNK_SETTLED_CHECK_BUDGET);
             return read_envelope_chunk_reap_gates();
          },
          [&](const outpost_solana_client_detail::envelope_chunk_reap_gates& gates) {
             reap_settled_envelope_chunks(deadline_abs, gates);
          },
          to_string())) {
      return {};
   }

   // Stream the envelope into the per-(epoch, signer) chunk buffer. Each call
   // goes through `solana_program_client::execute_tx_and_confirm`, which
   // serialises submission + waits for `processed`-commitment confirmation
   // before returning. Chunks are submitted sequentially -- the **batch
   // operator's delivery instruction family is `epoch_in`**: all non-empty
   // data chunks stage bytes, then the terminal call triggers the program's
   // `finalize_envelope`. Settlement follows through `dispatch_attestations`.
   //
   // Resume past whatever an earlier tick already staged. Restarting at 0
   // spent the whole delivery deadline re-sending no-op chunks, so an
   // envelope larger than one deadline's worth of chunks never completed.
   const uint16_t first_chunk =
      read_first_unstaged_chunk(epoch_index, total_chunks, static_cast<uint32_t>(total));
   // The read swallows its own failures, including an RPC cut short by the
   // deadline, and a fully staged buffer skips the loop's per-chunk check
   // entirely -- so enforce the deadline here before sending anything.
   throw_if_past_deadline(deadline_abs, OP_EPOCH_IN);
   if (first_chunk == total_chunks) {
      ilog("outpost_solana_client[{}]: epoch_in epoch={} has all {} chunks staged; sending only the terminal call",
           to_string(), epoch_index, total_chunks);
   } else if (first_chunk > 0) {
      ilog("outpost_solana_client[{}]: epoch_in resuming epoch={} at chunk {}/{} already staged",
           to_string(), epoch_index, first_chunk, total_chunks);
   }
   std::string last_sig;
   // One past the last chunk this tick confirmed; the loop advances it only
   // after a chunk's `epoch_in` returns.
   uint16_t next_chunk = first_chunk;
   try {
      for (; next_chunk < total_chunks; ++next_chunk) {
         throw_if_past_deadline(deadline_abs, OP_EPOCH_IN);

         const size_t off = static_cast<size_t>(next_chunk) * SOLANA_MAX_CHUNK_BYTES;
         const size_t len = std::min(SOLANA_MAX_CHUNK_BYTES, total - off);
         std::vector<uint8_t> chunk(
            reinterpret_cast<const uint8_t*>(envelope_bytes.data() + off),
            reinterpret_cast<const uint8_t*>(envelope_bytes.data() + off + len));

         last_sig = _program_client->epoch_in(
            epoch_index, next_chunk, total_chunks, static_cast<uint32_t>(total), chunk, {});
         ilog("outpost_solana_client[{}]: epoch_in chunk sent epoch={} chunk={}/{} bytes={} sig={}",
              to_string(), epoch_index, next_chunk, total_chunks, len, last_sig);
      }

      // Memoize once the buffer is fully staged: the program refuses to crank a
      // partial buffer (TerminalChunkBeforeDataComplete), so the crank-once-consensus-
      // tips path is only reachable from a COMPLETE staged buffer. A partial upload
      // leaves this unset and the next run_outbound tick resumes from the buffer's
      // on-chain `received_chunks`.
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
   } catch (const fc::exception& e) {
      // An envelope larger than one deadline's worth of chunks is EXPECTED to
      // span ticks. A tick whose deadline ran out after it staged at least one
      // chunk made progress the next tick resumes from, so report it as an
      // epoch left open -- not as a failure, which would warn on every tick of a
      // healthy large upload and hide a real stall among them. The deadline can
      // end the tick at the loop's own check or inside an `epoch_in` (its
      // confirmation polling is clamped to the scoped deadline), so the test is
      // the clock rather than the exception type. A tick that staged nothing
      // keeps the original error: that is what a stall looks like.
      if (next_chunk > first_chunk && fc::time_point::now() >= deadline_abs) {
         SYS_THROW(chain::outpost_delivery_incomplete_exception,
                   "outpost_solana_client[{}]: epoch={} staged chunks {}..{} of {} before the delivery deadline; the "
                   "next tick resumes from the staged buffer ({})",
                   to_string(), epoch_index, first_chunk, next_chunk - 1, total_chunks, e.top_message());
      }
      throw;
   }

   // Delivery IS complete at this point -- the terminal call above recorded it
   // and ran the consensus predicate. Settlement is a separate instruction and
   // a separate concern (`drain_dispatch`): kick it inline so the tipping
   // operator normally closes the epoch immediately, but never let a
   // settlement failure travel back into the delivery result. A throw here
   // would make the caller treat the DELIVERY as failed and deliver again on
   // its next tick -- the staged buffer is complete, so that is a redundant
   // terminal call -- even though the chunks and the terminal call already
   // landed; settlement instead resumes from the
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
   // Return the rent of buffers this relay staged for OLDER epochs that have since settled. The epoch just delivered
   // is rarely among them: the drain above confirms at `processed`, while the sweep reads its gates at `confirmed`,
   // which normally still shows this epoch open -- a later sweep reaps its buffer. Best-effort and non-throwing, like
   // the drain: the delivery has already landed.
   reap_settled_envelope_chunks(deadline_abs);
   return last_sig;
}

std::optional<fc::sha256> outpost_solana_client::read_emitted_envelope_digest(uint32_t         epoch_index,
                                                                              fc::microseconds deadline) {
   const fc::time_point deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

   throw_if_past_deadline(deadline_abs, OP_READ_EMITTED_DIGEST);
   // `finalized`, as `read_inbound_envelope` reads: a record from a slot that can still roll back proves nothing.
   const std::optional<fc::network::solana::account_info> info = _entry->client->get_account_info(
      _program_client->outbound_envelopes_pda, fc::network::solana::commitment_t::finalized);
   if (!info.has_value()) return std::nullopt;   // the log is created at initialize: nothing recorded yet
   FC_ASSERT(info->owner == _program_id,
             "outpost_solana_client[{}]: the outbound envelope log is owned by {} instead of program {}", to_string(),
             info->owner.to_string(fc::yield_function_t{}), _program_id.to_string(fc::yield_function_t{}));
   return outpost_solana_client_detail::decode_envelope_log_digest(*_program_client, info->data, epoch_index);
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

   // A found envelope goes straight back: the caller delivers it to the depot only after this returns, so a sweep here
   // would sit in front of that delivery. An empty read is an idle tick, and only that tick sweeps the chunk buffers,
   // within `ENVELOPE_CHUNK_SWEEP_BUDGET` -- the tick that finds the confirmed cursor moved often reads the
   // finalized envelope still empty, and the caller holds its job lock across this call.
   return outpost_solana_client_detail::read_envelope_then_sweep_when_idle(
      [&] { return read_latest_outbound_envelope(epoch_index, deadline_abs); },
      [this](fc::time_point sweep_deadline_abs) { reap_settled_envelope_chunks(sweep_deadline_abs); },
      deadline_abs);
}

std::vector<char> outpost_solana_client::read_latest_outbound_envelope(uint32_t       epoch_index,
                                                                       fc::time_point deadline_abs) {
   throw_if_past_deadline(deadline_abs, OP_READ_LATEST);

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
   // uses for `DistributionState`). For inbound-reading roles
   // the account declaration was validated at boot, so a decode failure here
   // signals IDL-vs-deployment drift and is logged at warning level.
   return outpost_solana_client_detail::decode_latest_envelope_account(
      *_program_client, info->data, epoch_index, to_string());
}

} // namespace sysio
