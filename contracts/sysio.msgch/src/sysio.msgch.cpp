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

uint32_t current_epoch_index() {
   epoch::epochstate_t tbl(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
   return tbl.exists() ? tbl.get().current_epoch_index : 0;
}

uint32_t epoch_operators_per_group() {
   epoch::epochcfg_t tbl(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
   return tbl.exists() ? tbl.get().operators_per_epoch : 7;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  deliver — batch operator delivers inbound OPP data for a specific outpost
// ---------------------------------------------------------------------------
void msgch::deliver(name batch_op_name, uint64_t outpost_id, std::vector<char> data) {
   is_batch_operator_active(batch_op_name);
   check(!data.empty(), "delivery data cannot be empty");

   // Verify outpost exists
   epoch::outposts_t outposts(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
   auto op_it = outposts.find(outpost_id);
   check(op_it != outposts.end(), "outpost not found");

   // Compute checksum trustlessly inside the contract
   checksum256 cs = sha256(data.data(), data.size());
   uint32_t epoch = current_epoch_index();

   // Prevent duplicate delivery from same operator for same outpost+epoch
   envelopes_t envs(get_self(), get_self().value);
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint64_t composite = (static_cast<uint64_t>(outpost_id) << 32) | epoch;
   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      check(it->batch_op_name != batch_op_name,
            "operator already delivered for this outpost+epoch");
   }

   // Store envelope
   envs.emplace(get_self(), [&](auto& e) {
      e.id            = envs.available_primary_key();
      e.outpost_id    = outpost_id;
      e.epoch_index   = epoch;
      e.batch_op_name = batch_op_name;
      e.chain_kind    = op_it->chain_kind;
      e.checksum      = cs;
      e.raw_data      = data;
      e.received_at   = current_time_point();
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

   envelopes_t envs(get_self(), get_self().value);
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
         epoch::epochstate_t state_tbl(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
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

   // Store as inbound message
   messages_t msgs(get_self(), get_self().value);
   msgs.emplace(get_self(), [&](auto& m) {
      m.id            = msgs.available_primary_key();
      m.outpost_id    = outpost_id;
      m.epoch_index   = epoch;
      m.direction     = MessageDirection::MESSAGE_DIRECTION_INBOUND;
      m.status        = MessageStatus::MESSAGE_STATUS_PROCESSED;
      m.raw_payload   = raw;
      m.received_at   = now;
      m.processed_at  = now;
   });

   // Store as attestation
   attestations_t atts(get_self(), get_self().value);
   uint64_t att_id = atts.available_primary_key();
   atts.emplace(get_self(), [&](auto& a) {
      a.id                  = att_id;
      a.outpost_id          = outpost_id;
      a.epoch_index         = epoch;
      a.type                = AttestationType::ATTESTATION_TYPE_UNSPECIFIED;
      a.status              = AttestationStatus::ATTESTATION_STATUS_READY;
      a.data                = raw;
      a.pending_timestamp   = 0;
      a.ready_timestamp     = now_sec;
      a.processed_timestamp = 0;
   });

   // If the attestation type requires underwriting, create uwreq
   // (deferred — needs per-AttestationEntry parsing first)
}

// ---------------------------------------------------------------------------
//  queueout — queue outbound attestation for an outpost
// ---------------------------------------------------------------------------
void msgch::queueout(uint64_t outpost_id,
                     opp::types::AttestationType attest_type,
                     std::vector<char> data) {
   auto now_sec = static_cast<uint64_t>(current_time_point().sec_since_epoch());

   attestations_t atts(get_self(), get_self().value);
   atts.emplace(get_self(), [&](auto& a) {
      a.id                  = atts.available_primary_key();
      a.outpost_id          = outpost_id;
      a.epoch_index         = current_epoch_index();
      a.type                = attest_type;
      a.status              = AttestationStatus::ATTESTATION_STATUS_READY;
      a.data                = data;
      a.pending_timestamp   = 0;
      a.ready_timestamp     = now_sec;
      a.processed_timestamp = 0;
   });
}

// ---------------------------------------------------------------------------
//  buildenv — build outbound envelope from READY attestations
// ---------------------------------------------------------------------------
void msgch::buildenv(uint64_t outpost_id) {
   require_auth(EPOCH_ACCOUNT);

   uint32_t epoch = current_epoch_index();
   attestations_t atts(get_self(), get_self().value);
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
      auto it = atts.find(aid);
      if (it != atts.end()) {
         atts.modify(it, same_payer, [&](auto& a) {
            a.status = AttestationStatus::ATTESTATION_STATUS_PROCESSED;
            a.processed_timestamp = now_sec;
         });
      }
   }

   // Build OPP Envelope
   opp::Envelope env;
   env.epoch_index = zpp::bits::vuint32_t{epoch};
   env.epoch_timestamp = zpp::bits::vuint64_t{now_sec};

   // Serialize envelope
   auto [packed, out] = zpp::bits::data_out<char>();
   (void)out(env);

   // Store outbound envelope
   outenvelopes_t envelopes(get_self(), get_self().value);
   envelopes.emplace(get_self(), [&](auto& e) {
      e.id            = envelopes.available_primary_key();
      e.outpost_id    = outpost_id;
      e.epoch_index   = epoch;
      e.envelope_hash = sha256(packed.data(), packed.size());
      e.status        = EnvelopeStatus::ENVELOPE_STATUS_PENDING_DELIVERY;
      e.raw_envelope  = packed;
   });
}

// ---------------------------------------------------------------------------
//  cleanup — remove old attestations and envelopes
// ---------------------------------------------------------------------------
void msgch::cleanup(uint32_t before_epoch) {
   attestations_t atts(get_self(), get_self().value);
   auto epoch_idx = atts.get_index<"byepoch"_n>();
   uint32_t removed = 0;
   for (auto it = epoch_idx.begin();
        it != epoch_idx.end() && it->epoch_index < before_epoch;) {
      it = epoch_idx.erase(it);
      if (++removed >= 50) break;
   }

   envelopes_t envs(get_self(), get_self().value);
   auto env_oe_idx = envs.get_index<"byoutepoch"_n>();
   removed = 0;
   for (auto it = env_oe_idx.begin();
        it != env_oe_idx.end() && it->epoch_index < before_epoch;) {
      it = env_oe_idx.erase(it);
      if (++removed >= 50) break;
   }
}

} // namespace sysio
