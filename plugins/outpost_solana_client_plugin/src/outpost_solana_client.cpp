#include <sysio/outpost_solana_client_plugin/outpost_solana_client.hpp>

#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/variant_object.hpp>

#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

namespace sysio {

namespace {

// ── Op labels used for deadline-exceeded error messages ──────────────────
constexpr std::string_view OP_EPOCH_IN    = "deliver_outbound_envelope:epoch_in";
constexpr std::string_view OP_READ_LATEST = "read_inbound_envelope:get_account_info";

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

/// Read a little-endian u32 from `buf` at `off`. Borsh is little-endian on the wire; the native-endian `memcpy` below
/// is correct only on a little-endian host. Wire is x86_64-only today; the static_assert documents that dependency and
/// fails to compile if a future port to a big-endian host removes the implicit guarantee.
uint32_t read_u32_le(const std::vector<uint8_t>& buf, size_t off) {
   static_assert(std::endian::native == std::endian::little, "read_u32_le assumes a little-endian host");
   if (off + 4 > buf.size()) FC_THROW("LatestOutboundEnvelope: truncated u32 at {}", off);
   uint32_t v;
   std::memcpy(&v, buf.data() + off, 4);
   return v;
}

} // namespace

outpost_solana_client::outpost_solana_client(
   solana_client_entry_ptr                        entry,
   fc::network::solana::solana_public_key         program_id,
   std::vector<fc::network::solana::idl::program> program_idls,
   uint64_t                                       outpost_id,
   uint32_t                                       chain_id,
   fc::network::solana::commitment_t              inbound_read_commitment)
   : _entry(std::move(entry))
   , _program_id(program_id)
   , _outpost_id(outpost_id)
   , _chain_id(chain_id)
   , _inbound_read_commitment(inbound_read_commitment) {
   FC_ASSERT(_entry && _entry->client,
             "solana_client_entry must carry a client");
   FC_ASSERT(!program_idls.empty(),
             "Solana outpost requires at least one IDL for program '{}'",
             OPP_SOLANA_OUTPOST_PROGRAM_NAME);

   _program_client = std::make_shared<opp_solana_outpost_client>(
      _entry->client, _program_id, program_idls);
}

sysio::opp::types::ChainKind outpost_solana_client::chain_kind() const {
   return sysio::opp::types::CHAIN_KIND_SOLANA;
}

std::string outpost_solana_client::deliver_outbound_envelope(
   uint32_t                 epoch_index,
   const std::vector<char>& envelope_bytes,
   fc::microseconds         deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;

   const size_t total = envelope_bytes.size();
   FC_ASSERT(total > 0,
             "outpost_solana_client: refusing to deliver an empty envelope");
   FC_ASSERT(total <= SOLANA_MAX_ENVELOPE_BYTES,
             "outpost_solana_client: envelope ({} bytes) exceeds Solana hard "
             "cap of {} bytes; the program will reject it",
             total, SOLANA_MAX_ENVELOPE_BYTES);

   const uint16_t total_chunks = static_cast<uint16_t>(
      (total + SOLANA_MAX_CHUNK_BYTES - 1) / SOLANA_MAX_CHUNK_BYTES);

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

      last_sig = _program_client->epoch_in(
         epoch_index,
         i,
         total_chunks,
         static_cast<uint32_t>(total),
         chunk);
      ilog("outpost_solana_client[{}]: epoch_in chunk sent epoch={} chunk={}/{} bytes={} sig={}",
           to_string(), epoch_index, i, total_chunks, len, last_sig);
   }

   return last_sig;
}

std::vector<char> outpost_solana_client::read_inbound_envelope(
   uint32_t         epoch_index,
   fc::microseconds deadline) {
   const auto deadline_abs = fc::time_point::now() + deadline;
   throw_if_past_deadline(deadline_abs, OP_READ_LATEST);

   // Single RPC: fetch the `latest_outbound_envelope` PDA. The Solana program overwrites this account with the most
   // recent emitted envelope's bytes. The OPP cycle is atomic across actors — at any time only the most-recent emitted
   // envelope is in flight — so a single-slot PDA is sufficient and historical reads are out of scope (off-chain audit
   // tooling owns them). Commitment level is operator-configurable via `--solana-inbound-read-commitment`; the default
   // is `confirmed`.
   auto info = _entry->client->get_account_info(
      _program_client->latest_outbound_envelope_pda,
      _inbound_read_commitment);
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

} // namespace sysio
