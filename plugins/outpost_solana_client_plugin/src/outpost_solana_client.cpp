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

/// Anchor seed literals for the collateral-settlement PDAs. Byte-exact
/// mirrors of the program's `COLLATERAL_POSITION_SEED` /
/// `COLLATERAL_VAULT_SEED` constants (wire-solana `opp_states.rs`) -- a
/// one-character drift derives a well-formed WRONG PDA that only surfaces at
/// runtime as `EffectAccountMissing`, holding the dispatch cursor.
constexpr std::string_view COLLATERAL_POSITION_SEED = "collateral_position";
constexpr std::string_view COLLATERAL_VAULT_SEED    = "collateral_vault";

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

/// Field identifiers of the per-`(token_code, reserve_code)` `Reserve`
/// account. `custody_mint` / `custody_decimals` / `custody_token_program` are
/// pinned at reserve creation
/// and are what the on-chain terminal handlers branch on, so the relay's
/// manifest must read custody from HERE and nowhere else.
namespace reserve_account {
   constexpr auto account_name                = "Reserve";
   constexpr auto field_creator               = "creator";
   constexpr auto field_custody_mint          = "custody_mint";
   constexpr auto field_custody_decimals      = "custody_decimals";
   constexpr auto field_custody_token_program = "custody_token_program";
} // namespace reserve_account

/// Field identifiers of the per-`(operator, token_code)` collateral position.
/// Its custody mint is pinned at first deposit and is what the on-chain
/// settlement handlers branch on.
namespace collateral_position {
   constexpr auto account_name       = "CollateralPosition";
   constexpr auto field_operator     = "operator";
   constexpr auto field_token_code   = "token_code";
   constexpr auto field_custody_mint = "custody_mint";
   constexpr auto field_amount       = "amount";
} // namespace collateral_position

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

/// Assert the loaded IDL declares `CollateralPosition` with the four fields
/// its on-chain settlement path binds together. Full contract on the header
/// declaration.
void assert_collateral_position_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const auto& fields = declared_account_fields(program, collateral_position::account_name);

   bool has_operator   = false;
   bool has_token_code = false;
   bool has_mint       = false;
   bool has_amount     = false;
   for (const auto& field : fields) {
      const bool is_pubkey =
         field.type.is_primitive() && field.type.primitive == idl::primitive_type::pubkey;
      const bool is_u64 =
         field.type.is_primitive() && field.type.primitive == idl::primitive_type::u64;
      if (field.name == collateral_position::field_operator) {
         FC_ASSERT(is_pubkey, "CollateralPosition '{}' must be declared pubkey, got '{}'",
                   collateral_position::field_operator, describe_idl_type(field.type));
         has_operator = true;
      } else if (field.name == collateral_position::field_token_code) {
         FC_ASSERT(is_u64, "CollateralPosition '{}' must be declared u64, got '{}'",
                   collateral_position::field_token_code, describe_idl_type(field.type));
         has_token_code = true;
      } else if (field.name == collateral_position::field_custody_mint) {
         FC_ASSERT(is_pubkey, "CollateralPosition '{}' must be declared pubkey, got '{}'",
                   collateral_position::field_custody_mint, describe_idl_type(field.type));
         has_mint = true;
      } else if (field.name == collateral_position::field_amount) {
         FC_ASSERT(is_u64, "CollateralPosition '{}' must be declared u64, got '{}'",
                   collateral_position::field_amount, describe_idl_type(field.type));
         has_amount = true;
      }
   }
   FC_ASSERT(has_operator && has_token_code && has_mint && has_amount,
             "CollateralPosition IDL missing '{}' / '{}' / '{}' / '{}' "
             "(found {} / {} / {} / {}); a live collateral position's pinned custody cannot be "
             "resolved safely without this declaration",
             collateral_position::field_operator, collateral_position::field_token_code,
             collateral_position::field_custody_mint, collateral_position::field_amount,
             has_operator, has_token_code, has_mint, has_amount);
}

/// Assert the loaded IDL declares `Reserve` with the three fields the terminal
/// manifest resolves from it. Full contract on the header declaration.
void assert_reserve_shape(const fc::network::solana::idl::program& program) {
   namespace idl = fc::network::solana::idl;
   const auto& fields = declared_account_fields(program, reserve_account::account_name);

   bool has_creator       = false;
   bool has_mint          = false;
   bool has_decimals      = false;
   bool has_token_program = false;
   for (const auto& field : fields) {
      // Compare against the optional member directly, as the sibling shape
      // checks do: a malformed type object must reach the per-field diagnostic
      // rather than `get_primitive()`'s generic throw.
      const bool is_pubkey =
         field.type.is_primitive() && field.type.primitive == idl::primitive_type::pubkey;
      if (field.name == reserve_account::field_creator) {
         FC_ASSERT(is_pubkey, "Reserve '{}' must be declared pubkey, got '{}'",
                   reserve_account::field_creator, describe_idl_type(field.type));
         has_creator = true;
      } else if (field.name == reserve_account::field_custody_mint) {
         FC_ASSERT(is_pubkey, "Reserve '{}' must be declared pubkey, got '{}'",
                   reserve_account::field_custody_mint, describe_idl_type(field.type));
         has_mint = true;
      } else if (field.name == reserve_account::field_custody_decimals) {
         FC_ASSERT(field.type.is_primitive() && field.type.primitive == idl::primitive_type::u8,
                   "Reserve '{}' must be declared u8, got '{}'",
                   reserve_account::field_custody_decimals, describe_idl_type(field.type));
         has_decimals = true;
      } else if (field.name == reserve_account::field_custody_token_program) {
         FC_ASSERT(is_pubkey, "Reserve '{}' must be declared pubkey, got '{}'",
                   reserve_account::field_custody_token_program, describe_idl_type(field.type));
         has_token_program = true;
      }
   }
   // One message for all four: they are written together at reserve creation,
   // and any one missing costs the manifest the same way -- the effect account
   // the program's branch requires cannot be derived, so its dispatch window
   // aborts and the cursor never moves past it.
   FC_ASSERT(has_creator && has_mint && has_decimals && has_token_program,
             "Reserve IDL missing '{}' / '{}' / '{}' / '{}' (found {} / {} / {} / {}); the terminal "
             "manifest resolves custody, the cancel-refund target and the ATA-deriving token "
             "program from these, and a reserve-backed dispatch window cannot be built without them",
             reserve_account::field_creator, reserve_account::field_custody_mint,
             reserve_account::field_custody_decimals,
             reserve_account::field_custody_token_program,
             has_creator, has_mint, has_decimals, has_token_program);
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

} // anonymous namespace (within outpost_solana_client_detail)

/// Derive the per-reserve `Reserve` PDA. Full contract on the header
/// declaration.
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

/// Derive the per-reserve `reserve_vault` PDA. Full contract on the header
/// declaration.
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

/// Derive the per-`(operator, token_code)` `CollateralPosition` PDA. Full
/// contract on the header declaration.
fc::network::solana::solana_public_key derive_collateral_position_pda(
   const fc::network::solana::solana_public_key& program_id,
   const fc::network::solana::solana_public_key& operator_key,
   uint64_t token_code) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>(COLLATERAL_POSITION_SEED.begin(), COLLATERAL_POSITION_SEED.end()),
       pubkey_seed(operator_key),
       u64_seed(token_code)},
      program_id).first;
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
constexpr uint8_t  SEED_INSTRUCTION_DATA    = 2;
constexpr uint8_t  SEED_ACCOUNT_KEY         = 3;

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
      if (ext_type == 0) break;                              // Uninitialized terminator
      if (value + ext_len > mint_account_data.size()) break;  // truncated: stop, do not guess
      if (ext_type == EXT_TYPE_TRANSFER_HOOK && ext_len >= 64) {
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
   std::vector<extra_account_meta> metas;
   if (validation_account_data.size() < 12) return metas;
   const auto* bytes = validation_account_data.data();

   size_t cursor = 0;
   while (cursor + 12 <= validation_account_data.size()) {
      const bool is_execute =
         std::memcmp(bytes + cursor, EXECUTE_TLV_DISCRIMINATOR.data(), 8) == 0;
      const uint32_t entry_len = le_u32(bytes + cursor + 8);
      const size_t   value     = cursor + 12;
      if (value + entry_len > validation_account_data.size()) break;
      if (is_execute) {
         if (entry_len < 4) break;
         const uint32_t count = le_u32(bytes + value);
         for (uint32_t i = 0; i < count; ++i) {
            const size_t at = value + 4 + static_cast<size_t>(i) * EXTRA_ACCOUNT_META_SIZE;
            if (at + EXTRA_ACCOUNT_META_SIZE > validation_account_data.size()) break;
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
   return metas;
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

/// Derive the per-`token_code` `collateral_vault` PDA. Full contract on the
/// header declaration.
fc::network::solana::solana_public_key derive_collateral_vault_pda(
   const fc::network::solana::solana_public_key& program_id,
   uint64_t token_code) {
   return fc::network::solana::system::find_program_address(
      {std::vector<uint8_t>(COLLATERAL_VAULT_SEED.begin(), COLLATERAL_VAULT_SEED.end()),
       u64_seed(token_code)},
      program_id).first;
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
            // The collateral-settling operator actions (SOL-379/380). Both
            // resolve the per-(operator, token_code) CollateralPosition PDA
            // out of remaining_accounts, so both carry the amount's
            // token_code alongside the operator key.
            case sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION: {
               sysio::opp::attestations::OperatorAction oa;
               if (!oa.ParseFromString(entry.data())) continue;
               std::optional<effect_shape> shape;
               if (oa.action_type() ==
                     sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT) {
                  shape = effect_shape::withdraw_remit;
               } else if (oa.action_type() ==
                     sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_SLASH) {
                  shape = effect_shape::slash;
               }
               if (!shape.has_value()) continue;
               if (auto pk = sol_pubkey_from_chain_address(oa.op_address())) {
                  effects.push_back(inbound_effect{
                     at, *shape, *pk, std::nullopt, oa.amount().token_code()});
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_DEPOSIT_REVERT: {
               sysio::opp::attestations::DepositRevert dr;
               if (!dr.ParseFromString(entry.data())) continue;
               if (auto pk = sol_pubkey_from_chain_address(dr.depositor())) {
                  effects.push_back(inbound_effect{
                     at, effect_shape::deposit_revert, *pk, std::nullopt,
                     dr.refund_amount().token_code()});
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

reserve_terminal_info reserve_info_from_account(const fc::variant_object& reserve) {
   // Required, not defaulted: a Reserve without custody is one this relay
   // cannot build an account-consistent manifest for, and a defaulted
   // (all-zero) mint would silently mean "native" -- the exact branch
   // divergence that makes the on-chain call abort for good.
   FC_ASSERT(reserve.contains(reserve_account::field_creator),
             "Reserve account missing '{}' field", reserve_account::field_creator);
   FC_ASSERT(reserve.contains(reserve_account::field_custody_mint),
             "Reserve account missing '{}' field — custody must come from the Reserve the "
             "on-chain handlers branch on, never from the mutable OutpostConfig token maps",
             reserve_account::field_custody_mint);
   FC_ASSERT(reserve.contains(reserve_account::field_custody_decimals),
             "Reserve account missing '{}' field", reserve_account::field_custody_decimals);
   FC_ASSERT(reserve.contains(reserve_account::field_custody_token_program),
             "Reserve account missing '{}' field — the canonical ATA differs per token program, "
             "so deriving with the legacy default would name an account the program never asks for",
             reserve_account::field_custody_token_program);

   const auto decimals = reserve[reserve_account::field_custody_decimals].as_uint64();
   FC_ASSERT(decimals <= std::numeric_limits<uint8_t>::max(),
             "Reserve '{}' = {} is out of byte range",
             reserve_account::field_custody_decimals, decimals);

   return reserve_terminal_info{
      fc::network::solana::solana_public_key::from_base58_string(
         reserve[reserve_account::field_creator].as_string()),
      fc::network::solana::solana_public_key::from_base58_string(
         reserve[reserve_account::field_custody_mint].as_string()),
      static_cast<uint8_t>(decimals),
      fc::network::solana::solana_public_key::from_base58_string(
         reserve[reserve_account::field_custody_token_program].as_string())};
}

std::vector<std::vector<fc::network::solana::account_meta>> build_dispatch_manifests(
   const fc::network::solana::solana_public_key& program_id,
   const std::vector<inbound_effect>&            effects,
   uint32_t                                      total_attestations,
   const std::function<void()>&                  throw_if_past_deadline,
   const reserve_info_reader&                    read_reserve_info,
   const collateral_custody_reader&              read_collateral_custody,
   const transfer_hook_reader&                   read_transfer_hook,
   const fc::network::solana::solana_public_key& reserve_aggregate,
   const std::string&                            log_label) {
   const auto& token_program_id = fc::network::solana::system::program_ids::TOKEN_PROGRAM;
   const auto& associated_token_program_id =
      fc::network::solana::system::program_ids::ASSOCIATED_TOKEN_PROGRAM;
   const auto& system_program_id = fc::network::solana::system::program_ids::SYSTEM_PROGRAM;

   // `NATIVE_TOKEN_MARKER` on the program side: an all-zero custody mint means
   // the reserve custodies lamports, which is the branch the handler takes.
   auto is_native_custody = [&](const fc::network::solana::solana_public_key& mint) {
      return mint == system_program_id;
   };

   // One read per DISTINCT reserve for the whole build -- the same reserve can
   // back many attestations of one envelope, and a degraded (empty) read is
   // memoised too so a permanently unreadable reserve costs exactly one
   // round-trip, not one per attestation.
   std::map<std::pair<uint64_t, uint64_t>, std::optional<reserve_terminal_info>> reserve_cache;
   auto reserve_info = [&](uint64_t token_code,
                           uint64_t reserve_code) -> const std::optional<reserve_terminal_info>& {
      const auto cache_key = std::make_pair(token_code, reserve_code);
      auto       it        = reserve_cache.find(cache_key);
      if (it != reserve_cache.end()) return it->second;
      return reserve_cache.emplace(cache_key, read_reserve_info(token_code, reserve_code))
         .first->second;
   };

   // One read per DISTINCT collateral position for the whole build. Custody
   // is pinned per `(operator, token_code)`, so both values are load-bearing
   // cache keys; absent/empty reads are memoised too.
   using collateral_key = std::pair<fc::network::solana::solana_public_key, uint64_t>;
   std::map<collateral_key, std::optional<token_custody_info>> collateral_custody_cache;
   auto collateral_custody =
      [&](const fc::network::solana::solana_public_key& operator_key,
          uint64_t token_code) -> const std::optional<token_custody_info>& {
      const auto cache_key = std::make_pair(operator_key, token_code);
      auto it = collateral_custody_cache.find(cache_key);
      if (it != collateral_custody_cache.end()) return it->second;
      return collateral_custody_cache
         .emplace(cache_key, read_collateral_custody(operator_key, token_code))
         .first->second;
   };

   std::vector<std::vector<fc::network::solana::account_meta>> per_attestation(total_attestations);
   for (const auto& effect : effects) {
      // The deadline is probed per effect, BEFORE its reserve read: a build
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

      // Collateral-settling shapes (SOL-379/380). Every one resolves the
      // per-(operator, token_code) `CollateralPosition` PDA out of
      // remaining_accounts and settles in the asset the position escrows.
      if (effect.shape == effect_shape::withdraw_remit || effect.shape == effect_shape::slash ||
          effect.shape == effect_shape::deposit_revert) {
         if (!effect.recipient || !effect.collateral_token_code) continue;
         const auto collateral_token_code = *effect.collateral_token_code;
         // WITHDRAW_REMIT pays the operator and DEPOSIT_REVERT refunds the
         // depositor; SLASH pays nobody directly -- its native seizure lands
         // in the named `reserve_aggregate`.
         if (effect.shape != effect_shape::slash) add(*effect.recipient, true);
         add(derive_collateral_position_pda(program_id, *effect.recipient,
                                            collateral_token_code),
             true);

         const auto& custody_opt = collateral_custody(*effect.recipient, collateral_token_code);
         if (!custody_opt.has_value()) continue;   // custody lookup already wlogged
         const auto& custody = *custody_opt;
         if (is_native_custody(custody.mint)) continue;

         // SPL custody: the collateral vault drains into the destination's
         // canonical ATA -- the operator's for WITHDRAW_REMIT, the
         // depositor's for DEPOSIT_REVERT, the `reserve_aggregate`'s for
         // SLASH.
         //
         // LOCK-STEP: which shapes settle SPL here must match the program's
         // handlers (`resolve_collateral_vault_transfer` callers in
         // wire-solana `inbound.rs`) exactly -- the relay's shape decisions
         // are unversioned against the program, and a handler that
         // `require_remaining_account`s an account this manifest omits aborts
         // the dispatch round and re-packs the identical window from the same
         // cursor on every retry. A program-side settlement-policy change and
         // this manifest must move together.
         const auto settlement_owner =
            effect.shape == effect_shape::slash ? reserve_aggregate : *effect.recipient;
         add(derive_collateral_vault_pda(program_id, collateral_token_code), true);
         add(fc::network::solana::system::get_associated_token_address(
                settlement_owner, custody.mint),
             true);
         add(token_program_id, false);
         continue;
      }

      if (!effect.reserve) continue;

      const auto token_code   = effect.reserve->token_code;
      const auto reserve_code = effect.reserve->reserve_code;

      // Every reserve-backed handler loads the Reserve PDA first.
      add(derive_reserve_pda(program_id, token_code, reserve_code), true);
      if (effect.shape == effect_shape::reserve_ready) continue;

      const auto vault_pda = derive_reserve_vault_pda(program_id, token_code, reserve_code);

      const auto& info_opt = reserve_info(token_code, reserve_code);
      if (!info_opt.has_value()) {
         // ONE cause reaches here: the Reserve account is ABSENT or EMPTY (see
         // `reserve_info_for_codes`, which throws on every other cause rather
         // than degrade). That case is benign by construction — the program's
         // `load_reserve_from_remaining` finds the Reserve PDA we DO pass,
         // fails to deserialize an uninitialized account, and returns
         // `Ok(None)`, which every reserve-backed handler turns into a logged
         // skip. No account we omit below is ever reached.
         //
         // The accounts are still passed because the reserve may be created
         // between this read and the crank landing. If it is, this manifest is
         // complete only for a NATIVE swap remit/revert (Reserve PDA + recipient);
         // an SPL one is short the recipient ATA, the custody mint and the custody
         // token program — all three are read off the Reserve we could not decode,
         // and the ATA is not even derivable without the token program (SOL-396) —
         // and a RESERVE_CREATE_CANCELLED is short the `creator` on EITHER custody
         // kind, since the creator is read off the Reserve we could not decode.
         // Those windows abort and the next tick rebuilds them against
         // a now-readable Reserve.
         //
         // The degrade is PER-ATTESTATION: the manifests are built for every
         // other attestation too, so the envelope's healthy PREFIX — the
         // attestations ahead of this one in dispatch order — still settles.
         // Nothing behind an aborting window can settle until it clears, since
         // `drive_dispatch_rounds` repacks each window from the on-chain
         // cursor.
         wlog("outpost_solana_client[{}]: Reserve({}, {}) absent or empty while building the "
              "terminal manifest for attestation {}; passing derivable accounts only -- the "
              "handler will log-and-skip an uninitialized reserve",
              log_label, token_code, reserve_code, effect.attestation_index);
         add(vault_pda, true);
         if (effect.recipient) add(*effect.recipient, true);
         // Best-effort only, and right solely for a legacy-SPL reserve: the real
         // token program is pinned on the Reserve we could not read. Not the
         // sanctioned derivation — see `custody_token_program` below.
         add(token_program_id, false);
         continue;
      }
      const auto& info = *info_opt;

      // SOL-396 LOCK-STEP: every reserve-backed SPL settlement routes through the
      // program's `reserve_vault_transfer`, which requires BOTH the reserve's
      // `custody_token_program` AND its `custody_mint` in remaining_accounts, and
      // derives the destination ATA with that token program. Both facts come from
      // the Reserve — never the legacy SPL-Token default — because the same
      // (owner, mint) pair has different canonical ATAs under each program.
      const auto& custody_token_program = info.custody_token_program;
      const auto  custody_ata           = [&](const fc::network::solana::solana_public_key& owner) {
         return fc::network::solana::system::get_associated_token_address(
            owner, info.custody_mint, custody_token_program);
      };

      // SOL-396, one layer deeper: `reserve_vault_transfer` routes through
      // `spl_token_2022::onchain::invoke_transfer_checked`, which for a mint
      // carrying the TransferHook extension resolves the hook program, its
      // validation PDA and every account that PDA declares OUT OF
      // `remaining_accounts`. A hook mint whose extras are missing aborts the
      // CPI before the transfer -- the same wedge, with the cursor pinned.
      //
      // No-op for every non-hook mint, which is all of them today: the reader
      // returns nullopt and nothing is appended.
      const auto add_hook_accounts = [&](const fc::network::solana::solana_public_key& source,
                                         const fc::network::solana::solana_public_key& destination,
                                         const fc::network::solana::solana_public_key& authority) {
         const auto hook = read_transfer_hook(info.custody_mint);
         if (!hook.has_value()) return;
         const auto validation_pda =
            derive_extra_account_metas_pda(hook->program, info.custody_mint);
         add(hook->program, false);
         add(validation_pda, false);
         for (const auto& meta : resolve_hook_metas(hook->declared, hook->program, source,
                                                    info.custody_mint, destination, authority,
                                                    validation_pda)) {
            add(meta.key, meta.is_writable);
         }
      };

      switch (effect.shape) {
         case effect_shape::swap_remit: {
            if (!effect.recipient) break;
            if (is_native_custody(info.custody_mint)) { add(*effect.recipient, true); break; }
            add(vault_pda, true);
            add(custody_ata(*effect.recipient), true);
            add_hook_accounts(vault_pda, custody_ata(*effect.recipient), vault_pda);
            // The mint is REQUIRED here, not optional: `reserve_vault_transfer`
            // `require_remaining_account`s it, and the CPI's `transfer_checked`
            // needs the mint account to verify the decimals against — the decimals
            // themselves come from the Reserve record, not from this account.
            // Omitting it is what aborted every SPL swap remit with
            // `EffectAccountMissing` after SOL-396 — the one reserve-backed shape
            // that had never carried it.
            add(info.custody_mint, false);
            add(custody_token_program, false);
            break;
         }
         case effect_shape::swap_revert: {
            if (!effect.recipient) break;
            if (is_native_custody(info.custody_mint)) { add(*effect.recipient, true); break; }
            add(vault_pda, true);
            add(info.custody_mint, false);
            add(*effect.recipient, true);
            add(custody_ata(*effect.recipient), true);
            add(custody_token_program, false);
            add_hook_accounts(vault_pda, custody_ata(*effect.recipient), vault_pda);
            add(associated_token_program_id, false);
            add(system_program_id, false);
            break;
         }
         case effect_shape::reserve_create_cancelled: {
            if (is_native_custody(info.custody_mint)) { add(info.creator, true); break; }
            add(vault_pda, true);
            add(info.creator, false);
            add(custody_ata(info.creator), true);
            add(info.custody_mint, false);
            add(custody_token_program, false);
            add_hook_accounts(vault_pda, custody_ata(info.creator), vault_pda);
            add(associated_token_program_id, false);
            add(system_program_id, false);
            break;
         }
         default:
            break;
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
      // The instruction existing is not enough: the crank is driven from the
      // `EpochDeliveries` cursor, so a PRESENT but drifted declaration (a
      // renamed or dropped `consensus_reached` / `dispatched_count`) would
      // leave every drain reading "consensus not reached, cursor at 0" behind
      // a dlog -- the outpost would stop settling with no error anywhere.
      // Fail at boot instead, where the IDL is still fixable.
      outpost_solana_client_detail::assert_epoch_deliveries_shape(*_program_client->get_program());
      // Same reasoning one level down: the crank's manifests resolve custody
      // and the cancel-refund target from `Reserve`, and a drifted declaration
      // makes a LIVE reserve unreadable to this relay only. In flight that is
      // unrecoverable -- the window aborts on the effect account it could not
      // derive, and every later window repacks from the same cursor -- so it
      // has to be a boot failure, not a first-drain surprise.
      outpost_solana_client_detail::assert_reserve_shape(*_program_client->get_program());
      // A drifted CollateralPosition declaration makes a LIVE position's
      // pinned custody unreadable and would wedge every collateral dispatch
      // window on the same unadvanced cursor.
      outpost_solana_client_detail::assert_collateral_position_shape(
         *_program_client->get_program());
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

std::optional<outpost_solana_client_detail::reserve_terminal_info>
outpost_solana_client::reserve_info_for_codes(uint64_t token_code, uint64_t reserve_code) {
   const auto reserve_pda =
      outpost_solana_client_detail::derive_reserve_pda(_program_id, token_code, reserve_code);
   const auto pda_label = reserve_pda.to_string(fc::yield_function_t{});

   // The two ways this can fail are NOT interchangeable, and conflating them
   // is what turns a bad reserve into a wedged epoch:
   //
   //   * ABSENT / EMPTY  -> degrade. The program's `load_reserve_from_remaining`
   //     fails to deserialize an uninitialized account and returns `Ok(None)`,
   //     which every reserve-backed handler turns into a logged skip. Passing a
   //     partial manifest costs nothing, because no omitted account is reached.
   //
   //   * PRESENT BUT UNREADABLE BY US -> throw. The account exists and the
   //     PROGRAM decodes it fine (its Borsh is authoritative), so it takes the
   //     real branch and demands the accounts that branch needs -- the custody
   //     ATA for an SPL remit/revert, the `creator` for a cancel with an escrow
   //     to refund, on EITHER custody kind. A degraded manifest is then
   //     guaranteed to hit `require_remaining_account` -> EffectAccountMissing
   //     and abort. That is not a retryable blip: `drive_dispatch_rounds`
   //     repacks every window from the on-chain cursor, so the same unreadable
   //     attestation heads every future window and the epoch never closes.
   //     Failing the tick loudly leaves the cursor untouched and keeps the
   //     envelope re-drivable once the cause (realistically IDL drift, which
   //     `assert_reserve_shape` now catches at boot) is fixed.
   //
   // `get_account_info` sits OUTSIDE the try on purpose: an RPC or deadline
   // exception is not a statement about this reserve, and `deadline_scope`
   // MUST propagate so an over-deadline build stops instead of degrading every
   // remaining attestation into an aborting manifest.
   const auto account_info = _entry->client->get_account_info(reserve_pda);
   if (!account_info.has_value() || account_info->data.empty()) {
      wlog("outpost_solana_client[{}]: Reserve({}, {}) absent or empty at {}; terminal manifest "
           "will omit branch-specific accounts for this reserve -- the handler log-and-skips an "
           "uninitialized reserve",
           to_string(), token_code, reserve_code, pda_label);
      return std::nullopt;
   }

   try {
      const auto reserve_v =
         _program_client->decode_account_info_data(reserve_account::account_name, account_info->data);
      const auto& reserve = reserve_v.get_object();

      // Custody is read from the RESERVE, never from the mutable
      // `OutpostConfig` token maps: `handle_swap_remit`, `handle_swap_revert`
      // and `handle_reserve_create_cancelled` all branch on
      // `reserve.custody_mint`, so resolving it anywhere else lets an admin
      // re-point of a token address between reserve creation and dispatch put
      // the relay on the native branch while the program takes the SPL one --
      // the missing vault/ATA accounts then abort the call permanently.
      return outpost_solana_client_detail::reserve_info_from_account(reserve);
   } catch (const fc::exception& e) {
      elog("outpost_solana_client[{}]: Reserve({}, {}) at {} EXISTS ({} bytes) but this relay "
           "cannot read it; refusing to build a manifest the program is guaranteed to abort on. "
           "The dispatch cursor is left untouched and this epoch cannot settle until the cause is "
           "fixed -- check the loaded IDL against the deployed program: {}",
           to_string(), token_code, reserve_code, pda_label, account_info->data.size(),
           e.to_detail_string());
      throw;
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
      fc::network::solana::system::compute_budget::request_heap_frame(SOLANA_DISPATCH_HEAP_FRAME_BYTES));
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
   // empty entry so the indices stay aligned. The build reads ONE account per
   // distinct Reserve or CollateralPosition -- the exact PDA the handlers
   // themselves branch on -- probes the deadline per effect, and degrades a
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
      [&](uint64_t token_code, uint64_t reserve_code) {
         return reserve_info_for_codes(token_code, reserve_code);
      },
      [&](const fc::network::solana::solana_public_key& operator_key, uint64_t token_code) {
         return collateral_position_custody(operator_key, token_code);
      },
      [&](const fc::network::solana::solana_public_key& custody_mint) {
         return mint_transfer_hook_for(custody_mint);
      },
      _program_client->reserve_pda,
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
/// Absent mint, no TransferHook extension, or a hook explicitly disabled (an
/// all-zero program id) all return `nullopt` and leave the manifest unchanged
/// -- which is every mint in the system today.
///
/// A hook that IS configured but whose validation PDA cannot be read is NOT
/// degraded to `nullopt`: that would silently emit a manifest short the very
/// accounts the CPI is about to demand, wedging the epoch with no diagnostic.
/// Throwing surfaces it against the account we could not read.
std::optional<outpost_solana_client_detail::mint_transfer_hook>
outpost_solana_client::mint_transfer_hook_for(
   const fc::network::solana::solana_public_key& custody_mint) {
   const auto mint_info = _entry->client->get_account_info(custody_mint);
   if (!mint_info.has_value() || mint_info->data.empty()) return std::nullopt;

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

std::optional<outpost_solana_client_detail::token_custody_info>
outpost_solana_client::collateral_position_custody(
   const fc::network::solana::solana_public_key& operator_key, uint64_t token_code) {
   const auto position_pda = outpost_solana_client_detail::derive_collateral_position_pda(
      _program_id, operator_key, token_code);
   const auto pda_label = position_pda.to_string(fc::yield_function_t{});

   // An RPC/deadline exception is not evidence that this position is absent,
   // so the read deliberately sits outside the decode-only try/catch.
   const auto account_info = _entry->client->get_account_info(position_pda);
   if (!account_info.has_value() || account_info->data.empty()) {
      wlog("outpost_solana_client[{}]: CollateralPosition({}, {}) absent or empty at {}; "
           "terminal manifest will omit branch-specific accounts for this position -- the "
           "handler log-and-skips an uninitialized position",
           to_string(), operator_key.to_string(fc::yield_function_t{}), token_code, pda_label);
      return std::nullopt;
   }

   try {
      const auto position_v = _program_client->decode_account_info_data(
         collateral_position::account_name, account_info->data);
      const auto& position = position_v.get_object();
      FC_ASSERT(position.contains(collateral_position::field_custody_mint),
                "CollateralPosition account missing '{}' field",
                collateral_position::field_custody_mint);
      return outpost_solana_client_detail::token_custody_info{
         fc::network::solana::solana_public_key::from_base58_string(
            position[collateral_position::field_custody_mint].as_string())};
   } catch (const fc::exception& e) {
      elog("outpost_solana_client[{}]: CollateralPosition({}, {}) at {} EXISTS ({} bytes) but "
           "this relay cannot read its pinned custody; refusing to build a manifest the program "
           "is guaranteed to abort on. The dispatch cursor is left untouched and this epoch "
           "cannot settle until the cause is fixed -- check the loaded IDL against the deployed "
           "program: {}",
           to_string(), operator_key.to_string(fc::yield_function_t{}), token_code, pda_label,
           account_info->data.size(), e.to_detail_string());
      throw;
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
