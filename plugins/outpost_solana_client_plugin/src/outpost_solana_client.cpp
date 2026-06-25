#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <bit>
#include <cstring>
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

std::vector<fc::network::solana::solana_public_key>
extract_inbound_recipient_pubkeys(const std::vector<char>& envelope_bytes) {
   std::vector<fc::network::solana::solana_public_key> recipients;

   sysio::opp::Envelope env;
   if (!env.ParseFromArray(envelope_bytes.data(),
                           static_cast<int>(envelope_bytes.size()))) {
      wlog("outpost_solana_client: envelope decode for remaining-accounts "
           "extraction failed; submitting epoch_in with no extras "
           "(WITHDRAW_REMIT/DEPOSIT_REVERT lamport transfers may "
           "log-and-skip on-chain if any are present)");
      return recipients;
   }

   auto record_unique = [&recipients](const fc::network::solana::solana_public_key& pk) {
      if (std::find(recipients.begin(), recipients.end(), pk) == recipients.end()) {
         recipients.push_back(pk);
      }
   };

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
                  record_unique(*pk);
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_DEPOSIT_REVERT: {
               sysio::opp::attestations::DepositRevert dr;
               if (!dr.ParseFromString(entry.data())) continue;
               if (auto pk = sol_pubkey_from_chain_address(dr.depositor())) {
                  record_unique(*pk);
               }
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_SWAP_REMIT: {
               sysio::opp::attestations::SwapRemit sr;
               if (!sr.ParseFromString(entry.data())) continue;
               if (auto pk = sol_pubkey_from_chain_address(sr.recipient())) {
                  record_unique(*pk);
               }
               break;
            }
            default:
               break;
         }
      }
   }

   return recipients;
}

std::vector<swap_remit_spl_target>
extract_inbound_swap_remit_spl_targets(const std::vector<char>& envelope_bytes) {
   std::vector<swap_remit_spl_target> targets;

   sysio::opp::Envelope env;
   if (!env.ParseFromArray(envelope_bytes.data(),
                           static_cast<int>(envelope_bytes.size()))) {
      wlog("outpost_solana_client: envelope decode for SPL swap-remit "
           "target extraction failed; submitting epoch_in with no SPL "
           "extras (any SPL SwapRemit attestations present will reject "
           "with `recipient ATA not in remaining_accounts`)");
      return targets;
   }

   for (const auto& message : env.messages()) {
      for (const auto& entry : message.payload().attestations()) {
         if (entry.type() != sysio::opp::types::ATTESTATION_TYPE_SWAP_REMIT) continue;
         sysio::opp::attestations::SwapRemit sr;
         if (!sr.ParseFromString(entry.data())) continue;
         auto recipient = sol_pubkey_from_chain_address(sr.recipient());
         if (!recipient) continue;  // not an SVM recipient
         targets.push_back(swap_remit_spl_target{
            sr.amount().token_code(),
            sr.reserve_code(),
            *recipient
         });
      }
   }

   return targets;
}


std::vector<reserve_pda_seeds>
extract_inbound_swap_remit_reserve_seeds(const std::vector<char>& envelope_bytes) {
   std::vector<reserve_pda_seeds> seeds;

   sysio::opp::Envelope env;
   if (!env.ParseFromArray(envelope_bytes.data(),
                           static_cast<int>(envelope_bytes.size()))) {
      wlog("outpost_solana_client: envelope decode for swap-remit reserve "
           "seeds extraction failed; submitting epoch_in with no Reserve "
           "PDAs (SWAP_REMIT lamport transfers will log-and-skip on-chain "
           "if any are present)");
      return seeds;
   }

   auto record_unique = [&seeds](uint64_t token_code, uint64_t reserve_code) {
      auto matches = [&](const reserve_pda_seeds& s) {
         return s.token_code == token_code && s.reserve_code == reserve_code;
      };
      if (std::find_if(seeds.begin(), seeds.end(), matches) == seeds.end()) {
         seeds.push_back(reserve_pda_seeds{token_code, reserve_code});
      }
   };

   for (const auto& message : env.messages()) {
      for (const auto& entry : message.payload().attestations()) {
         switch (entry.type()) {
            case sysio::opp::types::ATTESTATION_TYPE_SWAP_REMIT: {
               sysio::opp::attestations::SwapRemit sr;
               if (!sr.ParseFromString(entry.data())) continue;
               record_unique(sr.amount().token_code(), sr.reserve_code());
               break;
            }
            // The reserve-lifecycle round-trips need the per-(token, reserve)
            // Reserve PDA in remaining_accounts too: `handle_reserve_ready`
            // flips its status field, and `handle_reserve_create_cancelled`
            // reads the refund amount/creator off it. RESERVE_READY rides
            // exactly ONE envelope (queued once at `matchreserve`), so a
            // missing PDA here doesn't defer the flip — it strands the
            // reserve in PENDING permanently. (The cancel path's refund
            // additionally needs creator/vault accounts on-chain; those are
            // looked up from the PDA at dispatch and remain log-and-skip
            // until a SOL-side cancel flow lands.)
            case sysio::opp::types::ATTESTATION_TYPE_RESERVE_READY: {
               sysio::opp::attestations::ReserveReady rr;
               if (!rr.ParseFromString(entry.data())) continue;
               record_unique(rr.token_code(), rr.reserve_code());
               break;
            }
            case sysio::opp::types::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED: {
               sysio::opp::attestations::ReserveCreateCancelled rcc;
               if (!rcc.ParseFromString(entry.data())) continue;
               record_unique(rcc.token_code(), rcc.reserve_code());
               break;
            }
            default:
               break;
         }
      }
   }

   return seeds;
}

} // namespace

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

std::optional<fc::network::solana::solana_public_key>
outpost_solana_client::spl_mint_for_token_code(uint64_t token_code) {
   std::lock_guard<std::mutex> guard(_token_address_mutex);

   if (_token_address_cache_loaded) {
      auto it = _mint_by_token_code.find(token_code);
      if (it != _mint_by_token_code.end()) return it->second;
      // Not seen at load time; record a negative entry to avoid
      // re-querying. set_token_address(...) calls at runtime would
      // require a relay restart (matches the on-chain
      // `token_addresses_by_code` cap — they're set once at bootstrap).
      _mint_by_token_code.emplace(token_code, std::nullopt);
      return std::nullopt;
   }

   // First miss: fetch + parse OutpostConfig.token_addresses_by_code.
   // Uses the program_client's IDL-driven decoder so a future struct
   // change picks up via wire-solana's regenerated IDL without C++
   // edits.
   try {
      auto cfg = _program_client->get_account_data<fc::variant>(
         "OutpostConfig", _program_client->config_pda);
      const auto& cfg_obj = cfg.get_object();
      if (cfg_obj.contains("token_addresses_by_code")) {
         const auto& entries = cfg_obj["token_addresses_by_code"].get_array();
         for (const auto& entry_v : entries) {
            const auto& entry = entry_v.get_object();
            if (!entry.contains("token_code") || !entry.contains("mint")) continue;
            uint64_t tc = entry["token_code"].as_uint64();
            const std::string mint_b58 = entry["mint"].as_string();
            // All-zeroes base58 ("11111111111111111111111111111111") is
            // the native marker — store as nullopt so the relay skips
            // SPL account derivation for native-token SwapRemits.
            constexpr std::string_view NATIVE_MARKER_B58 =
               "11111111111111111111111111111111";
            if (mint_b58 == NATIVE_MARKER_B58) {
               _mint_by_token_code.emplace(tc, std::nullopt);
            } else {
               _mint_by_token_code.emplace(
                  tc,
                  fc::network::solana::solana_public_key::from_base58_string(mint_b58)
               );
            }
         }
      }
      _token_address_cache_loaded = true;
      ilog("outpost_solana_client[{}]: token_address cache loaded with {} entries from OutpostConfig",
           to_string(), _mint_by_token_code.size());
   } catch (const std::exception& e) {
      wlog("outpost_solana_client[{}]: token_address cache load failed: {} "
           "— SPL SwapRemits will fail until cache loads on next attempt",
           to_string(), e.what());
      return std::nullopt;
   }

   auto it = _mint_by_token_code.find(token_code);
   if (it != _mint_by_token_code.end()) return it->second;
   return std::nullopt;
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

   // Decode the envelope ONCE up front and collect every operator /
   // depositor SOL pubkey referenced by an inbound WITHDRAW_REMIT or
   // DEPOSIT_REVERT attestation. These get appended past the IDL's
   // declared accounts on the **final** chunk submission — the chunk
   // that triggers `finalize_envelope` on-chain, which is where the
   // CPI transfers fire. Non-final chunks don't process attestations
   // so they don't need the extras and skipping them keeps each
   // chunk-write tx as small as possible (closer to the 1 232-byte MTU).
   auto recipient_pubkeys =
      outpost_solana_client_detail::extract_inbound_recipient_pubkeys(envelope_bytes);

   // SWAP_REMIT: the on-chain `handle_swap_remit` needs the per-(token,
   // reserve) Reserve PDA in `remaining_accounts` to drain lamports out
   // to the recipient. Derive the PDA seeds from each inbound SWAP_REMIT
   // attestation and append the resolved PDA past the user's recipient
   // pubkey. The deduped recipient list above already includes the
   // SWAP_REMIT recipient (handled by the same extractor switch).
   const auto reserve_seeds =
      outpost_solana_client_detail::extract_inbound_swap_remit_reserve_seeds(envelope_bytes);
   for (const auto& seeds : reserve_seeds) {
      std::vector<uint8_t> seed1 = {'r','e','s','e','r','v','e'};
      std::vector<uint8_t> seed2(8);
      std::vector<uint8_t> seed3(8);
      for (size_t i = 0; i < 8; ++i) {
         seed2[i] = static_cast<uint8_t>((seeds.token_code   >> (i * 8)) & 0xff);
         seed3[i] = static_cast<uint8_t>((seeds.reserve_code >> (i * 8)) & 0xff);
      }
      auto pda = fc::network::solana::system::find_program_address(
         {seed1, seed2, seed3}, _program_id).first;
      if (std::find(recipient_pubkeys.begin(), recipient_pubkeys.end(), pda)
          == recipient_pubkeys.end()) {
         recipient_pubkeys.push_back(pda);
      }
   }

   // SWAP_REMIT with non-native dst: the on-chain `handle_swap_remit_spl`
   // additionally needs `[reserve_vault PDA, recipient ATA, SPL mint,
   // spl_token_program]` in remaining_accounts so the signed
   // `token::transfer` CPI can resolve all referenced AccountInfos. The
   // mint comes from `OutpostConfig.token_addresses_by_code` (cached on
   // first miss); the recipient ATA is the deterministic
   // `get_associated_token_address(recipient, mint)`; the reserve_vault
   // PDA mirrors the Reserve PDA seed shape with `reserve_vault` prefix.
   //
   // Native-SOL dst attestations naturally skip this block because the
   // mint lookup returns nullopt (native marker), and the on-chain
   // native branch doesn't reference any of these accounts so absence
   // doesn't matter.
   const auto spl_targets =
      outpost_solana_client_detail::extract_inbound_swap_remit_spl_targets(envelope_bytes);
   size_t spl_accounts_added = 0;
   const auto& token_program_id =
      fc::network::solana::system::program_ids::TOKEN_PROGRAM;
   const auto& associated_token_program_id =
      fc::network::solana::system::program_ids::ASSOCIATED_TOKEN_PROGRAM;
   for (const auto& target : spl_targets) {
      auto mint_opt = spl_mint_for_token_code(target.token_code);
      if (!mint_opt) continue;  // native or unknown token_code

      // reserve_vault PDA (separate seed prefix from Reserve PDA).
      std::vector<uint8_t> vault_seed1 = {'r','e','s','e','r','v','e','_','v','a','u','l','t'};
      std::vector<uint8_t> vault_seed2(8);
      std::vector<uint8_t> vault_seed3(8);
      for (size_t i = 0; i < 8; ++i) {
         vault_seed2[i] = static_cast<uint8_t>((target.token_code   >> (i * 8)) & 0xff);
         vault_seed3[i] = static_cast<uint8_t>((target.reserve_code >> (i * 8)) & 0xff);
      }
      auto vault_pda = fc::network::solana::system::find_program_address(
         {vault_seed1, vault_seed2, vault_seed3}, _program_id).first;

      // recipient ATA = find_program_address(
      //   [recipient, TOKEN_PROGRAM, mint],
      //   ASSOCIATED_TOKEN_PROGRAM).
      // Same seed shape as `Pubkey::find_program_address` on the Rust
      // side via `spl_associated_token_account::get_associated_token_address`.
      const auto recipient_arr      = target.recipient.serialize();
      const auto token_program_arr  = token_program_id.serialize();
      const auto mint_arr           = mint_opt->serialize();
      std::vector<uint8_t> recipient_bytes(recipient_arr.begin(), recipient_arr.end());
      std::vector<uint8_t> token_program_bytes(token_program_arr.begin(), token_program_arr.end());
      std::vector<uint8_t> mint_bytes(mint_arr.begin(), mint_arr.end());
      auto recipient_ata = fc::network::solana::system::find_program_address(
         {recipient_bytes, token_program_bytes, mint_bytes},
         associated_token_program_id).first;

      // Dedup-append each pubkey. The on-chain handler uses
      // `find_remaining_account(remaining_accounts, &pubkey)` which is
      // order-independent, so any append position works.
      for (const auto& pk : {vault_pda, recipient_ata, *mint_opt, token_program_id}) {
         if (std::find(recipient_pubkeys.begin(), recipient_pubkeys.end(), pk)
             == recipient_pubkeys.end()) {
            recipient_pubkeys.push_back(pk);
            ++spl_accounts_added;
         }
      }
   }

   if (!recipient_pubkeys.empty()) {
      ilog("outpost_solana_client[{}]: epoch={} found {} inbound REMIT/REVERT "
           "recipient(s)/reserve(s) ({} SPL extras) — passing as remaining_accounts on final chunk",
           to_string(), epoch_index, recipient_pubkeys.size(), spl_accounts_added);
   }

   // Stream the envelope into the per-(epoch, signer) chunk buffer. Each
   // call goes through `solana_program_client::execute_tx_and_confirm`,
   // which serialises submission + waits for `processed`-commitment
   // confirmation before returning. Chunks are submitted sequentially —
   // the **batch operator's only Solana-side tx is `epoch_in`**: the
   // last-chunk call triggers the program's `finalize_envelope`, which
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

      const bool is_final = (i == total_chunks - 1);
      auto chunk_extras = is_final
         ? recipient_pubkeys
         : std::vector<fc::network::solana::solana_public_key>{};

      last_sig = _program_client->epoch_in(
         epoch_index,
         i,
         total_chunks,
         static_cast<uint32_t>(total),
         chunk,
         std::move(chunk_extras));
      ilog("outpost_solana_client[{}]: epoch_in chunk sent epoch={} chunk={}/{} bytes={} extras={} sig={}",
           to_string(), epoch_index, i, total_chunks, len,
           is_final ? recipient_pubkeys.size() : 0, last_sig);
   }

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
