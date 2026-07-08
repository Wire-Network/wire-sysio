#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

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
constexpr std::string_view OP_READ_LATEST = "read_inbound_envelope:get_account_info";
constexpr std::string_view OP_UW_COMMIT   = "uw_commit:commit_underwrite";
constexpr std::string_view RESERVE_ACCOUNT_NAME = "Reserve";
constexpr const char* RESERVE_FIELD_CREATOR = "creator";
constexpr const char* RESERVE_FIELD_CUSTODY_MINT = "custody_mint";
constexpr const char* RESERVE_FIELD_CUSTODY_DECIMALS = "custody_decimals";

/// 8-byte Anchor discriminator that prefixes every `#[account]`-tagged
/// account's serialized form.
constexpr size_t ANCHOR_DISCRIMINATOR_LEN = 8;

/// Borsh layout of `LatestOutboundEnvelope`:
///   epoch_index: u32         (4)
///   checksum:    [u8; 32]    (32)
///   data:        Vec<u8>     (4 + N)
///   bump:        u8          (1)
constexpr size_t LATEST_HEADER_LEN  = ANCHOR_DISCRIMINATOR_LEN + 4 + 32;
constexpr size_t LATEST_VEC_LEN_OFF = LATEST_HEADER_LEN;
constexpr size_t LATEST_DATA_OFF    = LATEST_HEADER_LEN + 4;
constexpr size_t LATEST_EPOCH_OFF   = ANCHOR_DISCRIMINATOR_LEN;

/// Read a little-endian u32 from `buf` at `off`. Borsh is little-endian on the wire; the native-endian
/// `memcpy` below is correct only on a little-endian host. WIRE is x86_64-only today; the static_assert
/// documents that dependency and fails the build if a future port to a big-endian host removes it.
uint32_t read_u32_le(const std::vector<uint8_t>& buf, size_t off) {
   static_assert(std::endian::native == std::endian::little, "read_u32_le assumes a little-endian host");
   if (off + 4 > buf.size()) FC_THROW("LatestOutboundEnvelope: truncated u32 at {}", off);
   uint32_t v;
   std::memcpy(&v, buf.data() + off, 4);
   return v;
}

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

/// Record a terminal recipient pubkey once, preserving first-seen order.
void record_unique_pubkey(std::vector<fc::network::solana::solana_public_key>& pubkeys,
                          const fc::network::solana::solana_public_key& pk) {
   if (std::find(pubkeys.begin(), pubkeys.end(), pk) == pubkeys.end()) {
      pubkeys.push_back(pk);
   }
}

/// Record a terminal Reserve PDA seed pair once, preserving first-seen order.
void record_unique_reserve_seed(std::vector<reserve_pda_seeds>& seeds,
                                uint64_t token_code,
                                uint64_t reserve_code) {
   const reserve_pda_seeds candidate{token_code, reserve_code};
   if (std::find(seeds.begin(), seeds.end(), candidate) == seeds.end()) {
      seeds.push_back(candidate);
   }
}

} // anonymous namespace (within outpost_solana_client_detail)

std::vector<reserve_terminal_lookup>
reserve_terminal_lookups_for_seeds(const fc::network::solana::solana_public_key& program_id,
                                   const std::vector<reserve_pda_seeds>& seeds) {
   std::vector<reserve_terminal_lookup> lookups;
   lookups.reserve(seeds.size());
   // Linear de-dupe preserves first-seen account order; the manifest input is bounded by the OPP envelope cap.
   for (const auto& seeds_for_reserve : seeds) {
      auto matches = [&](const reserve_terminal_lookup& lookup) {
         return lookup.seeds == seeds_for_reserve;
      };
      if (std::find_if(lookups.begin(), lookups.end(), matches) != lookups.end()) continue;

      lookups.push_back(reserve_terminal_lookup{
         seeds_for_reserve,
         derive_reserve_pda(program_id, seeds_for_reserve.token_code, seeds_for_reserve.reserve_code)
      });
   }
   return lookups;
}

reserve_terminal_info reserve_terminal_info_from_variant(const fc::variant& reserve_v) {
   const auto& reserve = reserve_v.get_object();
   FC_ASSERT(reserve.contains(RESERVE_FIELD_CREATOR), "Reserve account missing creator field");
   FC_ASSERT(reserve.contains(RESERVE_FIELD_CUSTODY_MINT), "Reserve account missing custody_mint field");
   FC_ASSERT(reserve.contains(RESERVE_FIELD_CUSTODY_DECIMALS), "Reserve account missing custody_decimals field");

   const auto custody_decimals = reserve[RESERVE_FIELD_CUSTODY_DECIMALS].as_uint64();
   FC_ASSERT(custody_decimals <= std::numeric_limits<uint8_t>::max(),
             "Reserve account custody_decimals {} exceeds uint8_t range",
             custody_decimals);

   return reserve_terminal_info{
      fc::network::solana::solana_public_key::from_base58_string(reserve[RESERVE_FIELD_CREATOR].as_string()),
      fc::network::solana::solana_public_key::from_base58_string(reserve[RESERVE_FIELD_CUSTODY_MINT].as_string()),
      static_cast<uint8_t>(custody_decimals)
   };
}

std::optional<reserve_terminal_info>
reserve_terminal_info_from_account(const reserve_pda_seeds& seeds,
                                   const fc::network::solana::solana_public_key& reserve_pda,
                                   const std::optional<fc::network::solana::account_info>& account_info,
                                   const reserve_account_decoder& decode_reserve,
                                   std::string_view client_label) {
   if (!account_info.has_value()) {
      wlog("outpost_solana_client[{}]: Reserve({}, {}) absent at {}; "
           "terminal manifest will omit branch-specific accounts for this reserve",
           client_label,
           seeds.token_code,
           seeds.reserve_code,
           reserve_pda.to_string(fc::yield_function_t{}));
      return std::nullopt;
   }

   FC_ASSERT(!account_info->data.empty(),
             "Reserve({}, {}) at {} has no account data",
             seeds.token_code,
             seeds.reserve_code,
             reserve_pda.to_string(fc::yield_function_t{}));

   return reserve_terminal_info_from_variant(decode_reserve(account_info->data));
}

reserve_terminal_info_cache reserve_terminal_info_cache_from_accounts(
   const std::vector<reserve_terminal_lookup>& lookups,
   const std::vector<std::optional<fc::network::solana::account_info>>& account_infos,
   const reserve_account_decoder& decode_reserve,
   std::string_view client_label) {
   FC_ASSERT(account_infos.size() == lookups.size(),
             "getMultipleAccounts returned {} values for {} Reserve PDA requests",
             account_infos.size(),
             lookups.size());

   reserve_terminal_info_cache infos;
   for (size_t i = 0; i < lookups.size(); ++i) {
      const auto& lookup = lookups[i];
      infos.emplace(
         lookup.seeds,
         reserve_terminal_info_from_account(lookup.seeds, lookup.reserve_pda, account_infos[i], decode_reserve,
                                            client_label));
   }
   return infos;
}

terminal_manifest_sources extract_inbound_terminal_manifest_sources(const sysio::opp::Envelope& env) {
   terminal_manifest_sources sources;

   for (const auto& message : env.messages()) {
      for (const auto& entry : message.payload().attestations()) {
         switch (entry.type()) {
            case sysio::opp::types::ATTESTATION_TYPE_OPERATOR_ACTION: {
               sysio::opp::attestations::OperatorAction oa;
               if (!oa.ParseFromString(entry.data())) continue;
               if (oa.action_type() !=
                     sysio::opp::attestations::OperatorAction_ActionType_ACTION_TYPE_WITHDRAW_REMIT) {
                  continue;
               }
               if (auto pk = sol_pubkey_from_chain_address(oa.op_address())) {
                  record_unique_pubkey(sources.recipient_pubkeys, *pk);
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_DEPOSIT_REVERT: {
               sysio::opp::attestations::DepositRevert dr;
               if (!dr.ParseFromString(entry.data())) continue;
               if (auto pk = sol_pubkey_from_chain_address(dr.depositor())) {
                  record_unique_pubkey(sources.recipient_pubkeys, *pk);
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_SWAP_REMIT: {
               sysio::opp::attestations::SwapRemit sr;
               if (!sr.ParseFromString(entry.data())) continue;
               record_unique_reserve_seed(sources.reserve_seeds, sr.amount().token_code(), sr.reserve_code());
               if (auto recipient = sol_pubkey_from_chain_address(sr.recipient())) {
                  record_unique_pubkey(sources.recipient_pubkeys, *recipient);
                  sources.swap_remit_spl_targets.push_back(swap_remit_spl_target{
                     sr.amount().token_code(),
                     sr.reserve_code(),
                     *recipient
                  });
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_SWAP_REVERT: {
               sysio::opp::attestations::SwapRevert sr;
               if (!sr.ParseFromString(entry.data())) continue;
               record_unique_reserve_seed(
                  sources.reserve_seeds, sr.refund_amount().token_code(), sr.source_reserve_code());
               if (auto depositor = sol_pubkey_from_chain_address(sr.depositor())) {
                  record_unique_pubkey(sources.recipient_pubkeys, *depositor);
                  sources.swap_revert_spl_targets.push_back(swap_remit_spl_target{
                     sr.refund_amount().token_code(),
                     sr.source_reserve_code(),
                     *depositor
                  });
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_RESERVE_READY: {
               sysio::opp::attestations::ReserveReady rr;
               if (!rr.ParseFromString(entry.data())) continue;
               record_unique_reserve_seed(sources.reserve_seeds, rr.token_code(), rr.reserve_code());
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED: {
               sysio::opp::attestations::ReserveCreateCancelled rcc;
               if (!rcc.ParseFromString(entry.data())) continue;
               record_unique_reserve_seed(sources.reserve_seeds, rcc.token_code(), rcc.reserve_code());
               record_unique_reserve_seed(sources.reserve_create_cancelled_seeds, rcc.token_code(), rcc.reserve_code());
               break;
            }
            default:
               break;
         }
      }
   }

   return sources;
}

terminal_manifest_sources extract_inbound_terminal_manifest_sources(const std::vector<char>& envelope_bytes) {
   sysio::opp::Envelope env;
   if (!env.ParseFromArray(envelope_bytes.data(),
                           static_cast<int>(envelope_bytes.size()))) {
      wlog("outpost_solana_client: envelope decode for terminal manifest "
           "extraction failed; submitting epoch_in with no extras "
           "(terminal handlers may log-and-skip if any effect accounts are missing)");
      return {};
   }

   return extract_inbound_terminal_manifest_sources(env);
}

std::vector<fc::network::solana::solana_public_key>
extract_inbound_recipient_pubkeys(const std::vector<char>& envelope_bytes) {
   return extract_inbound_terminal_manifest_sources(envelope_bytes).recipient_pubkeys;
}

std::vector<swap_remit_spl_target>
extract_inbound_swap_remit_spl_targets(const std::vector<char>& envelope_bytes) {
   return extract_inbound_terminal_manifest_sources(envelope_bytes).swap_remit_spl_targets;
}

std::vector<swap_remit_spl_target>
extract_inbound_swap_revert_spl_targets(const std::vector<char>& envelope_bytes) {
   return extract_inbound_terminal_manifest_sources(envelope_bytes).swap_revert_spl_targets;
}

std::vector<reserve_pda_seeds>
extract_inbound_swap_remit_reserve_seeds(const std::vector<char>& envelope_bytes) {
   return extract_inbound_terminal_manifest_sources(envelope_bytes).reserve_seeds;
}

std::vector<reserve_pda_seeds>
extract_inbound_reserve_create_cancelled_seeds(const std::vector<char>& envelope_bytes) {
   return extract_inbound_terminal_manifest_sources(envelope_bytes).reserve_create_cancelled_seeds;
}

} // namespace outpost_solana_client_detail

outpost_solana_client::outpost_solana_client(
   solana_client_entry_ptr                        entry,
   fc::network::solana::solana_public_key         program_id,
   std::vector<fc::network::solana::idl::program> program_idls,
   uint64_t                                       chain_code,
   uint32_t                                       chain_id)
   : _entry(std::move(entry))
   , _program_id(program_id)
   , _outpost_id(chain_code)
   , _chain_id(chain_id) {
   FC_ASSERT(_entry && _entry->client,
             "solana_client_entry must carry a client");
   FC_ASSERT(!program_idls.empty(),
             "Solana outpost requires at least one IDL for program '{}'",
             OPP_SOLANA_OUTPOST_PROGRAM_NAME);

   _program_client = std::make_shared<opp_solana_outpost_client>(
      _entry->client, _program_id, program_idls);
}

sysio::opp::types::ChainKind outpost_solana_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_SVM;
}

outpost_solana_client_detail::reserve_terminal_info_cache
outpost_solana_client::reserve_infos_for_lookups(
   const std::vector<outpost_solana_client_detail::reserve_terminal_lookup>& lookups) {
   if (lookups.empty()) return {};

   std::vector<fc::network::solana::solana_public_key> reserve_pdas;
   reserve_pdas.reserve(lookups.size());
   for (const auto& lookup : lookups) {
      reserve_pdas.push_back(lookup.reserve_pda);
   }

   const auto account_infos = _entry->client->get_multiple_accounts(reserve_pdas);

   const auto client_label = to_string();
   const auto reserve_account_name = std::string(RESERVE_ACCOUNT_NAME);
   auto decode_reserve = [&](const std::vector<uint8_t>& data) {
      return _program_client->decode_account_info_data(reserve_account_name, data);
   };

   return outpost_solana_client_detail::reserve_terminal_info_cache_from_accounts(
      lookups, account_infos, decode_reserve, client_label);
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

   // Build the zero-data terminal call's remaining-account manifest. Data
   // chunks only upload bytes; the terminal call triggers `finalize_envelope`
   // on-chain, so every account touched by effect handlers must be declared
   // here with the right writable flag. Reserve-backed effects fetch the
   // Reserve PDA first and use its pinned custody facts, not mutable
   // OutpostConfig token rows.
   const auto manifest =
      outpost_solana_client_detail::extract_inbound_terminal_manifest_sources(envelope_bytes);

   std::vector<fc::network::solana::account_meta> terminal_accounts;
   const auto reserve_lookups =
      outpost_solana_client_detail::reserve_terminal_lookups_for_seeds(_program_id, manifest.reserve_seeds);

   auto add_terminal_account = [&](const fc::network::solana::solana_public_key& key,
                                   bool is_writable) {
      outpost_solana_client_detail::record_terminal_account(terminal_accounts, key, is_writable);
   };
   auto reserve_lookup_for = [&](uint64_t token_code, uint64_t reserve_code)
      -> const outpost_solana_client_detail::reserve_terminal_lookup& {
      const outpost_solana_client_detail::reserve_pda_seeds key{token_code, reserve_code};
      auto it = std::find_if(reserve_lookups.begin(), reserve_lookups.end(), [&](const auto& lookup) {
         return lookup.seeds == key;
      });
      FC_ASSERT(it != reserve_lookups.end(),
                "Missing derived Reserve PDA lookup for Reserve({}, {})",
                token_code,
                reserve_code);
      return *it;
   };

   std::vector<outpost_solana_client_detail::reserve_terminal_lookup> reserve_info_lookups;
   auto add_reserve_info_lookup = [&](uint64_t token_code, uint64_t reserve_code) {
      const auto& lookup = reserve_lookup_for(token_code, reserve_code);
      auto matches = [&](const auto& existing) {
         return existing.seeds == lookup.seeds;
      };
      if (std::find_if(reserve_info_lookups.begin(), reserve_info_lookups.end(), matches) ==
          reserve_info_lookups.end()) {
         reserve_info_lookups.push_back(lookup);
      }
   };
   for (const auto& target : manifest.swap_remit_spl_targets) {
      add_reserve_info_lookup(target.token_code, target.reserve_code);
   }
   for (const auto& target : manifest.swap_revert_spl_targets) {
      add_reserve_info_lookup(target.token_code, target.reserve_code);
   }
   for (const auto& target : manifest.reserve_create_cancelled_seeds) {
      add_reserve_info_lookup(target.token_code, target.reserve_code);
   }

   const auto reserve_info_cache = reserve_infos_for_lookups(reserve_info_lookups);
   auto reserve_info = [&](uint64_t token_code, uint64_t reserve_code) -> std::optional<reserve_terminal_info> {
      const outpost_solana_client_detail::reserve_pda_seeds cache_key{token_code, reserve_code};
      auto it = reserve_info_cache.find(cache_key);
      FC_ASSERT(it != reserve_info_cache.end(),
                "Missing batched Reserve({}, {}) account result",
                token_code,
                reserve_code);
      return it->second;
   };
   auto is_native_custody = [](const fc::network::solana::solana_public_key& mint) {
      return mint == fc::network::solana::system::program_ids::SYSTEM_PROGRAM;
   };

   for (const auto& pk : manifest.recipient_pubkeys) {
      add_terminal_account(pk, true);
   }

   // Every reserve-backed handler now loads the Reserve PDA first so it can
   // use pinned custody facts. Declare each deduped Reserve PDA once.
   for (const auto& lookup : reserve_lookups) {
      add_terminal_account(lookup.reserve_pda, true);
   }

   size_t spl_accounts_added = 0;
   const auto& token_program_id =
      fc::network::solana::system::program_ids::TOKEN_PROGRAM;
   const auto& associated_token_program_id =
      fc::network::solana::system::program_ids::ASSOCIATED_TOKEN_PROGRAM;
   const auto& system_program_id =
      fc::network::solana::system::program_ids::SYSTEM_PROGRAM;

   for (const auto& target : manifest.swap_remit_spl_targets) {
      const auto info_opt = reserve_info(target.token_code, target.reserve_code);
      if (!info_opt.has_value()) continue;
      const auto& info = *info_opt;
      if (is_native_custody(info.custody_mint)) {
         add_terminal_account(target.recipient, true);
         continue;
      }
      const auto before = terminal_accounts.size();
      add_terminal_account(
         outpost_solana_client_detail::derive_reserve_vault_pda(
            _program_id, target.token_code, target.reserve_code),
         true);
      add_terminal_account(
         fc::network::solana::system::get_associated_token_address(
            target.recipient, info.custody_mint),
         true);
      add_terminal_account(token_program_id, false);
      spl_accounts_added += terminal_accounts.size() - before;
   }

   for (const auto& target : manifest.swap_revert_spl_targets) {
      const auto info_opt = reserve_info(target.token_code, target.reserve_code);
      if (!info_opt.has_value()) continue;
      const auto& info = *info_opt;
      if (is_native_custody(info.custody_mint)) {
         add_terminal_account(target.recipient, true);
         continue;
      }
      const auto before = terminal_accounts.size();
      add_terminal_account(
         outpost_solana_client_detail::derive_reserve_vault_pda(
            _program_id, target.token_code, target.reserve_code),
         true);
      add_terminal_account(info.custody_mint, false);
      add_terminal_account(target.recipient, true);
      add_terminal_account(
         fc::network::solana::system::get_associated_token_address(
            target.recipient, info.custody_mint),
         true);
      add_terminal_account(token_program_id, false);
      add_terminal_account(associated_token_program_id, false);
      add_terminal_account(system_program_id, false);
      spl_accounts_added += terminal_accounts.size() - before;
   }

   for (const auto& target : manifest.reserve_create_cancelled_seeds) {
      const auto info_opt = reserve_info(target.token_code, target.reserve_code);
      if (!info_opt.has_value()) continue;
      const auto& info = *info_opt;
      if (is_native_custody(info.custody_mint)) {
         add_terminal_account(info.creator, true);
         continue;
      }
      const auto before = terminal_accounts.size();
      add_terminal_account(
         outpost_solana_client_detail::derive_reserve_vault_pda(
            _program_id, target.token_code, target.reserve_code),
         true);
      add_terminal_account(info.creator, false);
      add_terminal_account(
         fc::network::solana::system::get_associated_token_address(
            info.creator, info.custody_mint),
         true);
      add_terminal_account(info.custody_mint, false);
      add_terminal_account(token_program_id, false);
      add_terminal_account(associated_token_program_id, false);
      add_terminal_account(system_program_id, false);
      spl_accounts_added += terminal_accounts.size() - before;
   }

   if (!terminal_accounts.empty()) {
      ilog("outpost_solana_client[{}]: epoch={} found {} inbound REMIT/REVERT "
           "recipient(s)/reserve(s) ({} SPL extras) — passing as remaining_accounts on terminal finalize",
           to_string(), epoch_index, terminal_accounts.size(), spl_accounts_added);
   }

   // Stream the envelope into the per-(epoch, signer) chunk buffer. Each
   // call goes through `solana_program_client::execute_tx_and_confirm`,
   // which serialises submission + waits for `processed`-commitment
   // confirmation before returning. Chunks are submitted sequentially —
   // the **batch operator's only Solana-side instruction family is `epoch_in`**:
   // all non-empty data chunks stage bytes, then one zero-data terminal
   // call triggers the program's `finalize_envelope`, which
   // (a) records the operator's delivery, (b) on consensus reach also
   // fires `emit_outbound_inner` inline (drains the queued outbound
   // attestations into a packed envelope and writes it to the
   // `latest_outbound_envelope` PDA), and (c) self-closes this
   // operator's chunk_buffer. No separate `emit_outbound_envelope` or
   // `cleanup_envelope_chunks` tx is needed in the relay.
   std::string last_sig;
   for (uint16_t i = 0; i < total_chunks; ++i) {
      throw_if_past_deadline(deadline_abs, OP_EPOCH_IN);

      const size_t off = static_cast<size_t>(i) * SOLANA_MAX_CHUNK_BYTES;
      const size_t len = std::min(SOLANA_MAX_CHUNK_BYTES, total - off);
      std::vector<uint8_t> chunk(
         reinterpret_cast<const uint8_t*>(envelope_bytes.data() + off),
         reinterpret_cast<const uint8_t*>(envelope_bytes.data() + off + len));

      last_sig = _program_client->epoch_in(
         epoch_index,
         i,
         total_chunks,
         static_cast<uint32_t>(total),
         chunk,
         {});
      ilog("outpost_solana_client[{}]: epoch_in chunk sent epoch={} chunk={}/{} bytes={} extras={} sig={}",
           to_string(), epoch_index, i, total_chunks, len, 0, last_sig);
   }

   throw_if_past_deadline(deadline_abs, OP_EPOCH_IN);
   const size_t terminal_extra_count = terminal_accounts.size();
   last_sig = _program_client->epoch_in(
      epoch_index,
      total_chunks,
      total_chunks,
      static_cast<uint32_t>(total),
      {},
      std::move(terminal_accounts));
   ilog("outpost_solana_client[{}]: epoch_in terminal finalize sent epoch={} chunk={}/{} bytes=0 extras={} sig={}",
        to_string(), epoch_index, total_chunks, total_chunks, terminal_extra_count, last_sig);

   return last_sig;
}

std::vector<char> outpost_solana_client::read_inbound_envelope(
   uint32_t         epoch_index,
   fc::microseconds deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

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
      // PDA was init'd at outpost initialize — absence here means the
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

   const auto& buf = info->data;
   dlog("outpost_solana_client[{}]: latest_outbound_envelope account_size={}",
        to_string(), buf.size());
   if (buf.size() < LATEST_DATA_OFF) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope account is "
           "smaller than expected header ({} bytes)",
           to_string(), buf.size());
      return {};
   }

   const uint32_t stored_epoch = read_u32_le(buf, LATEST_EPOCH_OFF);
   if (stored_epoch == 0) {
      // Initialized state: outpost has not emitted any envelope yet.
      // Expected during cluster warm-up; resolves on the next emit.
      dlog("outpost_solana_client[{}]: latest_outbound_envelope unwritten (epoch=0)",
           to_string());
      return {};
   }
   if (stored_epoch != epoch_index) {
      // Timing skew between the WIRE batch op and the outpost's emit
      // cadence. Resolves on the next poll once the outpost catches up.
      dlog("outpost_solana_client[{}]: latest_outbound_envelope stored_epoch={} != requested {}",
           to_string(), stored_epoch, epoch_index);
      return {};
   }

   const uint32_t data_len = read_u32_le(buf, LATEST_VEC_LEN_OFF);
   if (data_len > SOLANA_MAX_ENVELOPE_BYTES) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope data length "
           "{} exceeds envelope cap of {} bytes",
           to_string(), data_len, SOLANA_MAX_ENVELOPE_BYTES);
      return {};
   }
   if (LATEST_DATA_OFF + data_len > buf.size()) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope data length "
           "{} exceeds account size {}",
           to_string(), data_len, buf.size());
      return {};
   }

   std::vector<char> envelope_bytes(
      reinterpret_cast<const char*>(buf.data() + LATEST_DATA_OFF),
      reinterpret_cast<const char*>(buf.data() + LATEST_DATA_OFF + data_len));

   sysio::opp::Envelope envelope;
   if (!envelope.ParseFromArray(envelope_bytes.data(),
                                static_cast<int>(envelope_bytes.size()))) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope did not "
           "decode as a protobuf Envelope ({} bytes)",
           to_string(), envelope_bytes.size());
      return {};
   }
   if (static_cast<uint32_t>(envelope.epoch_index()) != epoch_index) {
      wlog("outpost_solana_client[{}]: latest_outbound_envelope inner "
           "epoch={} != requested epoch={}",
           to_string(), envelope.epoch_index(), epoch_index);
      return {};
   }

   ilog("outpost_solana_client[{}]: read inbound envelope for epoch {} ({} bytes)",
        to_string(), epoch_index, envelope_bytes.size());
   return envelope_bytes;
}

std::string outpost_solana_client::uw_commit(
   uint64_t                 uw_request_id,
   const std::vector<char>& uic_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   fc::task::deadline_scope rpc_deadline(deadline_abs);

   throw_if_past_deadline(deadline_abs, OP_UW_COMMIT);

   // `commit_underwrite(uic_bytes: bytes)` — opaque relay. The typed
   // wrapper carries the IDL-default account list (config + outbound
   // message buffer); the underwriter doesn't supply overrides.
   std::vector<uint8_t> uic_bytes_u8(uic_bytes.begin(), uic_bytes.end());
   auto signature = _program_client->commit_underwrite(std::move(uic_bytes_u8));
   ilog("outpost_solana_client[{}]: uw_commit confirmed uwreq={} sig={} bytes={}",
        to_string(), uw_request_id, signature, uic_bytes.size());
   return signature;
}

} // namespace sysio
