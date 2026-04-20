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

constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
constexpr auto UWRIT_ACCOUNT = "sysio.uwrit"_n;
constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;
constexpr uint32_t CLEANUP_BATCH_SIZE = 50;

uint32_t current_epoch_index() {
   epoch::epochstate_t tbl(EPOCH_ACCOUNT);
   return tbl.exists() ? tbl.get().current_epoch_index : 0;
}

uint32_t epoch_operators_per_group() {
   epoch::epochcfg_t tbl(EPOCH_ACCOUNT);
   return tbl.exists() ? tbl.get().operators_per_epoch : 7;
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
}

// ---------------------------------------------------------------------------
//  cleanup — remove old attestations and envelopes
// ---------------------------------------------------------------------------
void msgch::cleanup(uint32_t before_epoch) {
   attestations_t atts(get_self());
   auto epoch_idx = atts.get_index<"byepoch"_n>();
   uint32_t removed = 0;
   for (auto it = epoch_idx.begin();
        it != epoch_idx.end() && it->epoch_index < before_epoch;) {
      it = epoch_idx.erase(std::move(it));
      if (++removed >= CLEANUP_BATCH_SIZE) break;
   }

   envelopes_t envs(get_self());
   auto env_oe_idx = envs.get_index<"byoutepoch"_n>();
   removed = 0;
   for (auto it = env_oe_idx.begin();
        it != env_oe_idx.end() && it->epoch_index < before_epoch;) {
      it = env_oe_idx.erase(std::move(it));
      if (++removed >= CLEANUP_BATCH_SIZE) break;
   }

   outenvelopes_t outenvs(get_self());
   auto out_oe_idx = outenvs.get_index<"byoutepoch"_n>();
   removed = 0;
   for (auto it = out_oe_idx.begin();
        it != out_oe_idx.end() && it->epoch_index < before_epoch;) {
      it = out_oe_idx.erase(std::move(it));
      if (++removed >= CLEANUP_BATCH_SIZE) break;
   }
}

} // namespace sysio
