#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {


   class [[sysio::contract("sysio.msgch")]] msgch : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Bootstrap: call at epoch 0 to trigger the first advance.
      [[sysio::action]]
      void bootstrap();

      /// Batch operator delivers inbound OPP data for a specific outpost.
      /// Computes sha256 checksum trustlessly, stores in envelopes table,
      /// then calls evalcons inline to check consensus.
      [[sysio::action]]
      void deliver(name batch_op_name, uint64_t outpost_id, std::vector<char> data);

      /// Evaluate consensus on inbound envelopes for an outpost+epoch.
      /// Called inline from deliver. On consensus: unpacks envelope,
      /// stores messages + attestations, records per-outpost consensus.
      [[sysio::action]]
      void evalcons(uint64_t outpost_id, uint32_t epoch_index);

      /// Check if all-outpost consensus is reached AND next_epoch_start
      /// has passed. If yes, reset consensus and call advance.
      /// Called by the batch operator after the time window elapses.
      [[sysio::action]]
      void chkcons();

      /// Queue an outbound attestation for an outpost.
      /// Writes to the attestations table with status READY.
      [[sysio::action]]
      void queueout(uint64_t outpost_id,
                    opp::types::AttestationType attest_type,
                    std::vector<char> data);

      /// Build outbound envelope from READY attestations for an outpost.
      /// Collects attestations, packs into OPP Envelope, stores in outenvelopes.
      [[sysio::action]]
      void buildenv(uint64_t outpost_id);

      /// Remove attestations and envelopes older than before_epoch.
      /// Called inline from epoch::advance().
      [[sysio::action]]
      void cleanup(uint32_t before_epoch);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Inbound envelope delivery — one row per batch-op per outpost per epoch.
      /// Consensus is evaluated by comparing checksums across operators.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] envelope_entry {
         uint64_t                  id;
         uint64_t                  outpost_id;
         uint32_t                  epoch_index;
         name                      batch_op_name;
         opp::types::ChainKind     chain_kind;
         checksum256               checksum;        ///< sha256(raw_data)
         std::vector<char>         raw_data;
         time_point                received_at;

         uint64_t primary_key() const { return id; }
         uint64_t by_outpost_epoch() const {
            return (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;
         }
         uint64_t by_batch_op() const { return batch_op_name.value; }
      };

      using envelopes_t = multi_index<"envelopes"_n, envelope_entry,
         indexed_by<"byoutepoch"_n,
            const_mem_fun<envelope_entry, uint64_t, &envelope_entry::by_outpost_epoch>>,
         indexed_by<"bybatchop"_n,
            const_mem_fun<envelope_entry, uint64_t, &envelope_entry::by_batch_op>>
      >;

      /// Individual message extracted from a consensus-verified envelope.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] message_entry {
         uint64_t                        id;
         uint64_t                        outpost_id;
         uint32_t                        epoch_index;
         checksum256                     message_id;
         checksum256                     previous_message_id;
         opp::types::MessageDirection    direction;
         opp::types::MessageStatus       status;
         std::vector<char>               raw_payload;
         time_point                      received_at;
         time_point                      processed_at;

         uint64_t    primary_key() const { return id; }
         uint64_t    by_status()   const { return static_cast<uint64_t>(status); }
         uint64_t    by_epoch()    const { return epoch_index; }
         checksum256 by_msg_id()   const { return message_id; }
      };

      using messages_t = multi_index<"messages"_n, message_entry,
         indexed_by<"bystatus"_n,
            const_mem_fun<message_entry, uint64_t, &message_entry::by_status>>,
         indexed_by<"byepoch"_n,
            const_mem_fun<message_entry, uint64_t, &message_entry::by_epoch>>,
         indexed_by<"bymsgid"_n,
            const_mem_fun<message_entry, checksum256, &message_entry::by_msg_id>>
      >;

      /// Attestation extracted from a consensus-verified message, or
      /// queued outbound attestation awaiting envelope packing.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] attestation_entry {
         uint64_t                        id;
         uint64_t                        outpost_id;
         uint32_t                        epoch_index;
         opp::types::AttestationType     type;
         opp::types::AttestationStatus   status;
         std::vector<char>               data;
         uint64_t                        pending_timestamp;
         uint64_t                        ready_timestamp;
         uint64_t                        processed_timestamp;

         uint64_t primary_key() const { return id; }
         uint64_t by_status()   const { return static_cast<uint64_t>(status); }
         uint64_t by_type()     const { return static_cast<uint64_t>(type); }
         uint64_t by_epoch()    const { return epoch_index; }
      };

      using attestations_t = multi_index<"attestations"_n, attestation_entry,
         indexed_by<"bystatus"_n,
            const_mem_fun<attestation_entry, uint64_t, &attestation_entry::by_status>>,
         indexed_by<"bytype"_n,
            const_mem_fun<attestation_entry, uint64_t, &attestation_entry::by_type>>,
         indexed_by<"byepoch"_n,
            const_mem_fun<attestation_entry, uint64_t, &attestation_entry::by_epoch>>
      >;

      /// Outbound envelope table.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] outbound_envelope {
         uint64_t    id;
         uint64_t    outpost_id;
         uint32_t    epoch_index;
         checksum256 envelope_hash;
         checksum256 merkle_root;
         checksum256 start_message_id;
         checksum256 end_message_id;
         opp::types::EnvelopeStatus status;
         std::vector<char> raw_envelope;

         uint64_t primary_key() const { return id; }
         uint64_t by_outpost() const { return outpost_id; }
         uint64_t by_outpost_epoch() const {
            return (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;
         }
      };

      using outenvelopes_t = multi_index<"outenvelopes"_n, outbound_envelope,
         indexed_by<"byoutpost"_n,
            const_mem_fun<outbound_envelope, uint64_t, &outbound_envelope::by_outpost>>,
         indexed_by<"byoutepoch"_n,
            const_mem_fun<outbound_envelope, uint64_t, &outbound_envelope::by_outpost_epoch>>
      >;

      /// Per-outpost consensus tracking for the current epoch.
      /// One row per outpost. Rows reused (not erased) to avoid RAM churn.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] outpost_consensus_entry {
         uint64_t outpost_id;
         uint32_t epoch_index;
         bool     consensus_reached;

         uint64_t primary_key() const { return outpost_id; }
      };

      using outpost_consensus_t = multi_index<"outpcons"_n, outpost_consensus_entry>;

   private:

      using ChainKind          = opp::types::ChainKind;
      using ChainRequestStatus = opp::types::ChainRequestStatus;
      using MessageDirection    = opp::types::MessageDirection;
      using MessageStatus       = opp::types::MessageStatus;
      using EnvelopeStatus      = opp::types::EnvelopeStatus;
      using AttestationType     = opp::types::AttestationType;
      using AttestationStatus   = opp::types::AttestationStatus;
   };

} // namespace sysio
