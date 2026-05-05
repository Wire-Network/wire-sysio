#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio/opp/opp.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::ChainKind;
using opp::types::MessageDirection;
using opp::types::MessageStatus;
using opp::types::EnvelopeStatus;
using opp::types::AttestationType;
using opp::types::AttestationStatus;

namespace {

constexpr auto     EPOCH_ACCOUNT  = "sysio.epoch"_n;
constexpr auto     UWRIT_ACCOUNT  = "sysio.uwrit"_n;
constexpr auto     CHALG_ACCOUNT  = "sysio.chalg"_n;

/// WIRE chain numeric id used in `opp::Endpoints` rows on the audit log.
/// One end of every cross-chain envelope is always WIRE.
constexpr uint32_t WIRE_CHAIN_ID  = 1;

uint32_t current_epoch_index() {
   epoch::epochstate_t tbl(EPOCH_ACCOUNT);
   return tbl.exists() ? tbl.get().current_epoch_index : 0;
}

uint32_t epoch_operators_per_group() {
   epoch::epochcfg_t tbl(EPOCH_ACCOUNT);
   return tbl.exists() ? tbl.get().operators_per_epoch : 7;
}

/// Insert a metadata row into `envelope_log` and, if the table has grown
/// past its derived cap, evict the oldest full epoch (one
/// `per_epoch_record_count`'s worth of rows) from the head.
///
/// Cap derivation:
///   active_outposts        = sysio.epoch.outposts.size()
///   per_epoch_record_count = active_outposts * 2     // 1 inbound + 1 outbound per outpost
///   cap                    = per_epoch * cfg.epoch_retention_envelope_log_count
///
/// `live_count` is computed in O(1) from id arithmetic
/// (`available_primary_key()` and `tbl.begin()->id`) — no full-table scan
/// and no per-endpoint walk. Eviction at most touches one full epoch's
/// rows per write, since the slice can only grow by one per insert.
void write_envelope_log(name self,
                        const sysio::opp::Endpoints& endpoints,
                        uint32_t                     epoch_index,
                        const checksum256&           checksum) {
   sysio::msgch::envelope_log_t tbl(self);
   const uint64_t new_id = tbl.available_primary_key();
   tbl.emplace(self, sysio::msgch::id_key{new_id}, sysio::msgch::envelope_log_entry{
      .id          = new_id,
      .endpoints   = endpoints,
      .epoch_index = epoch_index,
      .checksum    = checksum,
      .emitted_at  = current_time_point(),
   });

   epoch::outposts_t outposts(EPOCH_ACCOUNT);
   uint32_t active_outposts = 0;
   for (auto it = outposts.begin(); it != outposts.end(); ++it) ++active_outposts;
   if (active_outposts == 0) return;                  // nothing to bound against

   epoch::epochcfg_t cfg_tbl(EPOCH_ACCOUNT);
   if (!cfg_tbl.exists()) return;                     // no config yet, no cap
   const auto cfg = cfg_tbl.get();
   const uint32_t per_epoch = active_outposts * 2;    // 1 inbound + 1 outbound
   const uint64_t cap =
      static_cast<uint64_t>(per_epoch) * cfg.epoch_retention_envelope_log_count;

   auto first_it = tbl.begin();
   if (first_it == tbl.end()) return;                 // defensive: just inserted
   const uint64_t oldest_id  = first_it.key().id;
   const uint64_t live_count = (new_id + 1) - oldest_id;
   if (live_count <= cap) return;

   uint32_t dropped = 0;
   for (auto it = tbl.begin();
        it != tbl.end() && dropped < per_epoch; ) {
      it = tbl.erase(std::move(it));
      ++dropped;
   }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  bootstrap — trigger first advance at epoch 0
// ---------------------------------------------------------------------------
void msgch::bootstrap() {
   require_auth(get_self());
   uint32_t epoch = current_epoch_index();
   check(epoch == 0, "bootstrap can only be called at epoch 0");
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "advance"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  deliver — batch operator delivers inbound OPP data for a specific outpost
// ---------------------------------------------------------------------------
void msgch::deliver(name batch_op_name, uint64_t outpost_id, std::vector<char> data) {
   is_batch_operator_active(batch_op_name);
   check(!data.empty(), "delivery data cannot be empty");

   // Verify outpost exists
   epoch::outposts_t outposts(EPOCH_ACCOUNT);
   auto outpost_pk = epoch::outpost_key{outpost_id};
   check(outposts.contains(outpost_pk), "outpost not found");
   auto op_row = outposts.get(outpost_pk);

   // Decode envelope to validate epoch_index matches current WIRE epoch
   uint32_t epoch = current_epoch_index();
   {
      opp::Envelope env_check;
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto result = in(env_check);
      check(result == zpp::bits::errc{}, "failed to decode inbound envelope");
      uint32_t env_epoch = static_cast<uint32_t>(env_check.epoch_index);
      check(env_epoch == epoch,
         "envelope epoch_index mismatch: envelope=" + std::to_string(env_epoch) +
         " current=" + std::to_string(epoch));
   }

   // Compute checksum trustlessly inside the contract
   checksum256 cs = sha256(data.data(), data.size());

   // Prevent duplicate delivery from same operator for same outpost+epoch
   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint64_t composite = (static_cast<uint64_t>(outpost_id) << 32) | epoch;
   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      if (it->batch_op_name == batch_op_name) {
         sysio::print_f("operator already delivered for this outpost+epoch: %s", batch_op_name.to_string().c_str());
         return;
      }
   }

   // Store envelope
   uint64_t env_id = envs.available_primary_key();

   envs.emplace(get_self(), id_key{env_id}, envelope_entry{
      .id            = env_id,
      .outpost_id    = outpost_id,
      .epoch_index   = epoch,
      .batch_op_name = batch_op_name,
      .chain_kind    = op_row.chain_kind,
      .checksum      = cs,
      .raw_data      = data,
      .received_at   = current_time_point(),
   });

   // Evaluate consensus inline
   action(
      permission_level{get_self(), "active"_n},
      get_self(),
      "evalcons"_n,
      std::make_tuple(outpost_id, epoch)
   ).send();
}

// ---------------------------------------------------------------------------
//  evalcons — evaluate consensus on inbound envelopes for outpost+epoch
// ---------------------------------------------------------------------------
void msgch::evalcons(uint64_t outpost_id, uint32_t epoch_index) {
   require_auth(get_self());

   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint64_t composite = (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;

   // Group envelopes by checksum (CDT-compatible parallel vectors)
   std::vector<checksum256>       seen_checksums;
   std::vector<uint32_t>          checksum_counts;
   std::vector<std::vector<char>> checksum_data;
   uint32_t total_deliveries = 0;

   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      bool found = false;
      for (size_t g = 0; g < seen_checksums.size(); ++g) {
         if (seen_checksums[g] == it->checksum) {
            checksum_counts[g]++;
            found = true;
            break;
         }
      }
      if (!found) {
         seen_checksums.push_back(it->checksum);
         checksum_counts.push_back(1);
         checksum_data.push_back(it->raw_data);
      }
      total_deliveries++;
   }

   uint32_t operators_per_group = epoch_operators_per_group();

   // Consensus check
   bool consensus_reached = false;
   size_t consensus_group = 0;

   for (size_t g = 0; g < seen_checksums.size(); ++g) {
      // Option A: ALL operators delivered identical data
      if (checksum_counts[g] == operators_per_group &&
          total_deliveries == operators_per_group) {
         consensus_reached = true;
         consensus_group = g;
         break;
      }
      // Option B: Majority at epoch boundary (current time >= next_epoch_start)
      if (checksum_counts[g] > operators_per_group / 2) {
         epoch::epochstate_t state_tbl(EPOCH_ACCOUNT);
         if (state_tbl.exists()) {
            auto state = state_tbl.get();
            if (current_time_point() >= state.next_epoch_start) {
               consensus_reached = true;
               consensus_group = g;
               break;
            }
         }
      }
   }

   if (!consensus_reached) return;

   // === CONSENSUS REACHED ===
   auto& raw    = checksum_data[consensus_group];
   auto  now    = current_time_point();
   auto  now_sec = static_cast<uint64_t>(now.sec_since_epoch());
   uint32_t epoch = current_epoch_index();

   // Decode protobuf Envelope from the consensus data (raw protobuf, no size prefix)
   opp::Envelope envelope;
   auto in = zpp::bits::in{std::span{raw.data(), raw.size()}, zpp::bits::no_size{}};
   auto decode_result = in(envelope);
   check(decode_result == zpp::bits::errc{}, "failed to decode inbound OPP Envelope");

   // Store the raw envelope as an inbound message
   messages_t msgs(get_self());
   uint64_t msg_id = msgs.available_primary_key();

   msgs.emplace(get_self(), id_key{msg_id}, message_entry{
      .id           = msg_id,
      .outpost_id   = outpost_id,
      .epoch_index  = epoch,
      .direction    = MessageDirection::MESSAGE_DIRECTION_INBOUND,
      .status       = MessageStatus::MESSAGE_STATUS_PROCESSED,
      .raw_payload  = raw,
      .received_at  = now,
      .processed_at = now,
   });

   // Extract individual AttestationEntries from each Message in the Envelope
   attestations_t atts(get_self());
   for (auto& msg : envelope.messages) {
      for (auto& entry : msg.payload.attestations) {
         uint64_t att_id = atts.available_primary_key();
         atts.emplace(get_self(), id_key{att_id}, attestation_entry{
            .id                  = att_id,
            .outpost_id          = outpost_id,
            .epoch_index         = epoch,
            .type                = entry.type,
            .status              = AttestationStatus::ATTESTATION_STATUS_READY,
            .data                = entry.data,
            .pending_timestamp   = 0,
            .ready_timestamp     = now_sec,
            .processed_timestamp = 0,
         });
      }
   }

   // === AUDIT LOG + INLINE CLEANUP OF WORKING STATE ===
   //
   // The envelope's bytes have served their purpose at this point:
   // consensus is reached, attestations are extracted and queued for
   // outbound delivery via `buildenv`. The durable trail is the
   // metadata-only `envelope_log` row written below; the four working
   // tables are drained inline so they don't grow without bound.
   {
      const auto& op_row = [&]() {
         epoch::outposts_t outposts(EPOCH_ACCOUNT);
         return outposts.get(epoch::outpost_key{outpost_id});
      }();

      sysio::opp::Endpoints endpoints;
      endpoints.start.kind = op_row.chain_kind;
      endpoints.start.id   = op_row.chain_id;
      endpoints.end.kind   = ChainKind::CHAIN_KIND_WIRE;
      endpoints.end.id     = WIRE_CHAIN_ID;

      write_envelope_log(get_self(), endpoints, epoch, seen_checksums[consensus_group]);

      // Drop the per-batch-op `envelopes` rows for this consensus event —
      // raw_data is dead weight once consensus is reached.
      auto evict_idx = envs.get_index<"byoutepoch"_n>();
      for (auto it = evict_idx.lower_bound(composite);
           it != evict_idx.end() && it->by_outpost_epoch() == composite; ) {
         it = evict_idx.erase(std::move(it));
      }

      // Drop the just-inserted `messages` row. Its raw_payload mirrors
      // the envelope bytes we already discarded; downstream consumers
      // read the audit log for trail and the attestations table for
      // queued outbound work.
      if (msgs.contains(id_key{msg_id})) {
         msgs.erase(id_key{msg_id});
      }
   }

   // === RECORD PER-OUTPOST CONSENSUS ===
   outpost_consensus_t opcons(get_self());
   auto opc_pk = outpost_consensus_key{outpost_id};
   if (!opcons.contains(opc_pk)) {
      opcons.emplace(get_self(), opc_pk, outpost_consensus_entry{
         .outpost_id        = outpost_id,
         .epoch_index       = epoch_index,
         .consensus_reached = true,
      });
   } else {
      opcons.modify(same_payer, opc_pk, [&](auto& r) {
         r.epoch_index       = epoch_index;
         r.consensus_reached = true;
      });
   }

   // Consensus state recorded — advance is triggered by chkcons
   // once next_epoch_start has passed.
}

// ---------------------------------------------------------------------------
//  chkcons — check all-outpost consensus + time gate, trigger advance
// ---------------------------------------------------------------------------
void msgch::chkcons() {
   uint32_t epoch = current_epoch_index();

   // Check all outposts have consensus for the current epoch
   outpost_consensus_t opcons(get_self());
   epoch::outposts_t outposts(EPOCH_ACCOUNT);
   bool all_consensus = true;
   uint32_t outpost_count = 0;

   for (auto it = outposts.begin(); it != outposts.end(); ++it) {
      ++outpost_count;
      auto opc_pk = outpost_consensus_key{it->id};
      if (!opcons.contains(opc_pk)) {
         all_consensus = false;
         break;
      }
      auto opc = opcons.get(opc_pk);
      if (!opc.consensus_reached || opc.epoch_index != epoch) {
         all_consensus = false;
         break;
      }
   }

   if (outpost_count == 0 || !all_consensus) return;

   // Check wall-clock: next_epoch_start must be in the past
   epoch::epochstate_t estate(EPOCH_ACCOUNT);
   if (!estate.exists()) return;
   auto state = estate.get();
   if (current_time_point() < state.next_epoch_start) return;

   // All conditions met — reset consensus and advance
   for (auto it = opcons.begin(); it != opcons.end(); ++it) {
      auto opc_pk = outpost_consensus_key{it.key().outpost_id};
      opcons.modify(same_payer, opc_pk, [&](auto& r) { r.consensus_reached = false; });
   }

   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "advance"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  queueout — queue outbound attestation for an outpost
// ---------------------------------------------------------------------------
void msgch::queueout(uint64_t outpost_id,
                     opp::types::AttestationType attest_type,
                     std::vector<char> data) {
   auto now_sec = static_cast<uint64_t>(current_time_point().sec_since_epoch());

   attestations_t atts(get_self());
   uint64_t att_id = atts.available_primary_key();

   atts.emplace(get_self(), id_key{att_id}, attestation_entry{
      .id                  = att_id,
      .outpost_id          = outpost_id,
      .epoch_index         = current_epoch_index(),
      .type                = attest_type,
      .status              = AttestationStatus::ATTESTATION_STATUS_READY,
      .data                = data,
      .pending_timestamp   = 0,
      .ready_timestamp     = now_sec,
      .processed_timestamp = 0,
   });
}

// ---------------------------------------------------------------------------
//  buildenv — build outbound envelope from READY attestations
// ---------------------------------------------------------------------------
void msgch::buildenv(uint64_t outpost_id) {
   require_auth(EPOCH_ACCOUNT);

   uint32_t epoch = current_epoch_index();
   attestations_t atts(get_self());
   auto now_sec = static_cast<uint64_t>(current_time_point().sec_since_epoch());

   // Collect READY attestations for this outpost
   std::vector<opp::AttestationEntry> entries;
   std::vector<uint64_t> att_ids;

   auto status_idx = atts.get_index<"bystatus"_n>();
   for (auto it = status_idx.lower_bound(
           static_cast<uint64_t>(AttestationStatus::ATTESTATION_STATUS_READY));
        it != status_idx.end() &&
        it->status == AttestationStatus::ATTESTATION_STATUS_READY; ++it) {
      if (it->outpost_id != outpost_id) continue;

      opp::AttestationEntry entry;
      entry.type = it->type;
      entry.data_size = zpp::bits::vuint32_t{static_cast<uint32_t>(it->data.size())};
      entry.data = it->data;
      entries.push_back(std::move(entry));
      att_ids.push_back(it->id);
   }

   if (entries.empty()) return;

   // Mark collected attestations as PROCESSED
   att_ids.erase(std::unique(att_ids.begin(), att_ids.end()), att_ids.end());
   for (uint64_t aid : att_ids) {
      auto att_pk = id_key{aid};
      if (atts.contains(att_pk)) {
         atts.modify(same_payer, att_pk, [&](auto& a) {
            a.status              = AttestationStatus::ATTESTATION_STATUS_PROCESSED;
            a.processed_timestamp = now_sec;
         });
      }
   }

   // Build a Message containing the collected attestations
   opp::MessagePayload payload;
   payload.version = zpp::bits::vuint32_t{1};
   payload.attestations = std::move(entries);

   opp::MessageHeader header;
   header.timestamp = zpp::bits::vuint64_t{now_sec};

   opp::Message msg;
   msg.header = std::move(header);
   msg.payload = std::move(payload);

   // Build OPP Envelope wrapping the message
   opp::Envelope env;
   env.epoch_index = zpp::bits::vuint32_t{epoch};
   env.epoch_timestamp = zpp::bits::vuint64_t{now_sec};
   env.messages.push_back(std::move(msg));

   // Serialize envelope (no size prefix — raw protobuf wire format)
   std::vector<char> packed;
   auto out = zpp::bits::out{packed, zpp::bits::no_size{}};
   (void)out(env);

   // Store outbound envelope
   outenvelopes_t envelopes(get_self());
   uint64_t out_id = envelopes.available_primary_key();

   envelopes.emplace(get_self(), id_key{out_id}, outbound_envelope{
      .id            = out_id,
      .outpost_id    = outpost_id,
      .epoch_index   = epoch,
      .envelope_hash = sha256(packed.data(), packed.size()),
      .status        = EnvelopeStatus::ENVELOPE_STATUS_PENDING_DELIVERY,
      .raw_envelope  = packed,
   });

   // === AUDIT LOG + INLINE CLEANUP OF WORKING STATE ===
   //
   // Audit-log row mirrors the outbound emit (WIRE → outpost). Followed
   // by inline drains of the previous-epoch outenvelopes row (one-deep
   // retention; the batch op only ever reads the most-recent emit) and
   // the just-PROCESSED attestations for this outpost (their bytes are
   // now baked into `packed` above).
   {
      const auto& op_row = [&]() {
         epoch::outposts_t outposts(EPOCH_ACCOUNT);
         return outposts.get(epoch::outpost_key{outpost_id});
      }();

      sysio::opp::Endpoints endpoints;
      endpoints.start.kind = ChainKind::CHAIN_KIND_WIRE;
      endpoints.start.id   = WIRE_CHAIN_ID;
      endpoints.end.kind   = op_row.chain_kind;
      endpoints.end.id     = op_row.chain_id;

      write_envelope_log(get_self(), endpoints, epoch,
                         sha256(packed.data(), packed.size()));

      // Drop previous outpost emits — keep only the row we just inserted.
      auto by_outpost = envelopes.get_index<"byoutpost"_n>();
      for (auto it = by_outpost.lower_bound(outpost_id);
           it != by_outpost.end() && it->outpost_id == outpost_id; ) {
         if (it->id == out_id) { ++it; continue; }
         it = by_outpost.erase(std::move(it));
      }

      // Drop the attestations we just consumed (status PROCESSED rows
      // for this outpost). They've been bundled into `packed`; the
      // bytes are dead weight on chain.
      auto processed_idx = atts.get_index<"bystatus"_n>();
      for (auto it = processed_idx.lower_bound(
                        static_cast<uint64_t>(AttestationStatus::ATTESTATION_STATUS_PROCESSED));
           it != processed_idx.end() &&
           it->status == AttestationStatus::ATTESTATION_STATUS_PROCESSED; ) {
         if (it->outpost_id != outpost_id) { ++it; continue; }
         it = processed_idx.erase(std::move(it));
      }
   }
}


} // namespace sysio
