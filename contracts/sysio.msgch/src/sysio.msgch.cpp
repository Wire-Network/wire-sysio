#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
namespace sysio {

using opp::types::ChainRequestStatus;
using opp::types::MessageDirection;
using opp::types::MessageStatus;
using opp::types::EnvelopeStatus;
using opp::types::AttestationType;

namespace {
constexpr auto EPOCH_ACCOUNT = "sysio.epoch"_n;
constexpr auto UWRIT_ACCOUNT = "sysio.uwrit"_n;
constexpr auto CHALG_ACCOUNT = "sysio.chalg"_n;

}

// ---------------------------------------------------------------------------
//  crank — main depot crank
// ---------------------------------------------------------------------------
void msgch::crank() {
   // Permissionless — anyone can call to advance processing.
   // In practice, batch operators call this at the start of each epoch cycle.
   // TODO: Read epoch state from sysio.epoch to determine current epoch,
   //       process pending outbound messages, build envelopes.
}

// ---------------------------------------------------------------------------
//  createreq
// ---------------------------------------------------------------------------
void msgch::createreq(uint64_t outpost_id) {
   require_auth(get_self());

   inchainreq_t requests(get_self(), get_self().value);
   requests.emplace(get_self(), [&](auto& r) {
      r.id = requests.available_primary_key();
      r.outpost_id = outpost_id;
      r.epoch_index = 0; // TODO: read from sysio.epoch
      r.status = ChainRequestStatus::CHAIN_REQUEST_STATUS_PENDING;
      r.delivery_count = 0;
   });
}

// ---------------------------------------------------------------------------
//  deliver
// ---------------------------------------------------------------------------
void msgch::deliver(name operator_acct,
                    uint64_t req_id,
                    checksum256 chain_hash,
                    checksum256 merkle_root,
                    uint32_t msg_count,
                    std::vector<char> raw_messages) {
   require_auth(operator_acct);

   inchainreq_t requests(get_self(), get_self().value);
   auto req_it = requests.find(req_id);
   check(req_it != requests.end(), "chain request not found");
   check(req_it->status == ChainRequestStatus::CHAIN_REQUEST_STATUS_PENDING || req_it->status == ChainRequestStatus::CHAIN_REQUEST_STATUS_COLLECTING,
         "chain request is not accepting deliveries");

   deliveries_t deliveries(get_self(), get_self().value);

   // Check for duplicate delivery from same operator
   auto op_idx = deliveries.get_index<"byoperator"_n>();
   for (auto it = op_idx.lower_bound(operator_acct.value);
        it != op_idx.end() && it->operator_account == operator_acct; ++it) {
      check(it->chain_request_id != req_id,
            "operator already delivered for this request");
   }

   deliveries.emplace(get_self(), [&](auto& d) {
      d.id = deliveries.available_primary_key();
      d.chain_request_id = req_id;
      d.operator_account = operator_acct;
      d.chain_hash = chain_hash;
      d.merkle_root = merkle_root;
      d.message_count = msg_count;
      d.delivered_at = current_time_point();
      d.matches_consensus = false;
   });

   // Update request status and delivery count
   requests.modify(req_it, same_payer, [&](auto& r) {
      r.delivery_count+=1;
      if (r.status == ChainRequestStatus::CHAIN_REQUEST_STATUS_PENDING) {
         r.status = ChainRequestStatus::CHAIN_REQUEST_STATUS_COLLECTING;
      }
   });

   // Store raw messages as individual message entries
   // TODO: Deserialize protobuf messages from raw_messages and
   //       create message_entry rows with proper attestation types.
   //       For now, store the batch as a single entry.
   if (!raw_messages.empty()) {
      messages_t messages(get_self(), get_self().value);
      messages.emplace(get_self(), [&](auto& m) {
         m.id = messages.available_primary_key();
         m.outpost_id = req_it->outpost_id;
         m.epoch_index = req_it->epoch_index;
         m.direction = MessageDirection::MESSAGE_DIRECTION_INBOUND;
         m.status = MessageStatus::MESSAGE_STATUS_PENDING;
         m.raw_payload = raw_messages;
         m.received_at = current_time_point();
      });
   }
}

// ---------------------------------------------------------------------------
//  evalcons — evaluate consensus
// ---------------------------------------------------------------------------
void msgch::evalcons(uint64_t req_id) {
   require_auth(get_self());

   inchainreq_t requests(get_self(), get_self().value);
   auto req_it = requests.find(req_id);
   check(req_it != requests.end(), "chain request not found");
   check(req_it->status == ChainRequestStatus::CHAIN_REQUEST_STATUS_COLLECTING, "request not in collecting state");

   deliveries_t deliveries(get_self(), get_self().value);
   auto req_idx = deliveries.get_index<"byrequest"_n>();

   // Collect all deliveries for this request, group by chain_hash
   // Using parallel vectors instead of std::map (CDT-compatible)
   std::vector<checksum256> seen_hashes;
   std::vector<std::vector<uint64_t>> hash_id_groups;
   uint32_t total_deliveries = 0;
   for (auto it = req_idx.lower_bound(req_id);
        it != req_idx.end() && it->chain_request_id == req_id; ++it) {
      // Find or create group for this hash
      bool found = false;
      for (size_t g = 0; g < seen_hashes.size(); ++g) {
         if (seen_hashes[g] == it->chain_hash) {
            hash_id_groups[g].push_back(it->id);
            found = true;
            break;
         }
      }
      if (!found) {
         seen_hashes.push_back(it->chain_hash);
         hash_id_groups.push_back({it->id});
      }
      total_deliveries++;
   }

   // Option A: all 7 identical
   // TODO: read operators_per_epoch from sysio.epoch config
   constexpr uint32_t OPERATORS_PER_EPOCH = 7;
   bool consensus_reached = false;

   for (size_t g = 0; g < seen_hashes.size(); ++g) {
      auto& ids = hash_id_groups[g];
      if (ids.size() == OPERATORS_PER_EPOCH && total_deliveries == OPERATORS_PER_EPOCH) {
         // Option A: all operators delivered identical hashes
         consensus_reached = true;
         for (auto delivery_id : ids) {
            auto d_it = deliveries.find(delivery_id);
            if (d_it != deliveries.end()) {
               deliveries.modify(d_it, same_payer, [&](auto& d) {
                  d.matches_consensus = true;
               });
            }
         }
         break;
      }

      // Option B: 4+ majority at epoch boundary
      if (ids.size() >= 4) {
         // TODO: Check if we're at epoch boundary (current_time >= next_epoch_start)
         //       For now, accept 4+ as consensus
         consensus_reached = true;
         for (auto delivery_id : ids) {
            auto d_it = deliveries.find(delivery_id);
            if (d_it != deliveries.end()) {
               deliveries.modify(d_it, same_payer, [&](auto& d) {
                  d.matches_consensus = true;
               });
            }
         }
         // Mark non-matching as false (already default)
         break;
      }
   }

   if (consensus_reached) {
      requests.modify(req_it, same_payer, [&](auto& r) {
         r.status = ChainRequestStatus::CHAIN_REQUEST_STATUS_CONSENSUS_OK;
      });
   } else {
      requests.modify(req_it, same_payer, [&](auto& r) {
         r.status = ChainRequestStatus::CHAIN_REQUEST_STATUS_CONSENSUS_FAIL;
      });
      // Notify sysio.chalg to initiate challenge
      require_recipient(CHALG_ACCOUNT);
   }
}

// ---------------------------------------------------------------------------
//  processmsg
// ---------------------------------------------------------------------------
void msgch::processmsg(uint64_t msg_id) {
   require_auth(get_self());

   messages_t messages(get_self(), get_self().value);
   auto it = messages.find(msg_id);
   check(it != messages.end(), "message not found");
   check(it->status == MessageStatus::MESSAGE_STATUS_READY, "message not in READY state");

   // TODO: Deserialize raw_payload into MessagePayload (protobuf via opp_cdt_models).
   //       For each AttestationEntry, classify and route:
   //       - OPERATOR_ACTION -> inline to sysio.epoch::regoperator
   //       - STAKE_UPDATE -> inline to sysio.uwrit
   //       - RESERVE_BALANCE_SHEET -> update outpost reserve tracking
   //       - CHALLENGE_RESPONSE -> inline to sysio.chalg
   //       - SWAP -> requires underwriting -> status = PENDING
   //       For now, mark as processed.

   messages.modify(it, same_payer, [&](auto& m) {
      m.status = MessageStatus::MESSAGE_STATUS_PROCESSED;
      m.processed_at = current_time_point();
   });
}

// ---------------------------------------------------------------------------
//  queueout
// ---------------------------------------------------------------------------
void msgch::queueout(uint64_t outpost_id,
                     opp::types::AttestationType attest_type,
                     std::vector<char> data) {
   require_auth(get_self());

   messages_t messages(get_self(), get_self().value);
   messages.emplace(get_self(), [&](auto& m) {
      m.id = messages.available_primary_key();
      m.outpost_id = outpost_id;
      m.epoch_index = 0; // TODO: read from sysio.epoch
      m.direction = MessageDirection::MESSAGE_DIRECTION_OUTBOUND;
      m.status = MessageStatus::MESSAGE_STATUS_PENDING;
      m.attestation_type = attest_type;
      m.raw_payload = data;
      m.received_at = current_time_point();
   });
}

// ---------------------------------------------------------------------------
//  buildenv
// ---------------------------------------------------------------------------
void msgch::buildenv(name batch_op_name, uint64_t outpost_id) {
   is_batch_operator_active(batch_op_name);

   // TODO: Collect all PENDING outbound messages for this outpost,
   //       build an OPP Envelope with merkle root, chain message IDs,
   //       and store as outbound_envelope entry.
   //       For now, create a placeholder envelope.

   outenvelopes_t envelopes(get_self(), get_self().value);
   envelopes.emplace(get_self(), [&](auto& e) {
      e.id = envelopes.available_primary_key();
      e.outpost_id = outpost_id;
      e.epoch_index = 0; // TODO: read from sysio.epoch
      e.status = EnvelopeStatus::ENVELOPE_STATUS_PENDING_DELIVERY;
   });
}

} // namespace sysio
