#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
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

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Auto-incrementing primary key shared by id-keyed msgch tables.
      struct id_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(id_key, (id))
      };

      /// Inbound envelope delivery — one row per batch-op per outpost per epoch.
      /// Consensus is evaluated by comparing checksums across operators.
      struct [[sysio::table("envelopes")]] envelope_entry {
         uint64_t                  id;
         uint64_t                  outpost_id;
         uint32_t                  epoch_index;
         name                      batch_op_name;
         opp::types::ChainKind     chain_kind;
         checksum256               checksum;        ///< sha256(raw_data)
         std::vector<char>         raw_data;
         time_point                received_at{};

         uint64_t by_outpost_epoch() const {
            return (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;
         }
         uint64_t by_batch_op() const { return batch_op_name.value; }

         SYSLIB_SERIALIZE(envelope_entry,
            (id)(outpost_id)(epoch_index)(batch_op_name)(chain_kind)
            (checksum)(raw_data)(received_at))
      };

      using envelopes_t = sysio::kv::table<"envelopes"_n, id_key, envelope_entry,
         sysio::kv::index<"byoutepoch"_n,
            sysio::const_mem_fun<envelope_entry, uint64_t, &envelope_entry::by_outpost_epoch>>,
         sysio::kv::index<"bybatchop"_n,
            sysio::const_mem_fun<envelope_entry, uint64_t, &envelope_entry::by_batch_op>>
      >;

      /// Individual message extracted from a consensus-verified envelope.
      struct [[sysio::table("messages")]] message_entry {
         uint64_t                        id;
         uint64_t                        outpost_id;
         uint32_t                        epoch_index;
         checksum256                     message_id;
         checksum256                     previous_message_id;
         opp::types::MessageDirection    direction;
         opp::types::MessageStatus       status;
         std::vector<char>               raw_payload;
         time_point                      received_at{};
         time_point                      processed_at{};

         uint64_t    by_status() const { return static_cast<uint64_t>(status); }
         uint64_t    by_epoch()  const { return epoch_index; }
         checksum256 by_msg_id() const { return message_id; }

         SYSLIB_SERIALIZE(message_entry,
            (id)(outpost_id)(epoch_index)(message_id)(previous_message_id)
            (direction)(status)(raw_payload)(received_at)(processed_at))
      };

      using messages_t = sysio::kv::table<"messages"_n, id_key, message_entry,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<message_entry, uint64_t, &message_entry::by_status>>,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<message_entry, uint64_t, &message_entry::by_epoch>>,
         sysio::kv::index<"bymsgid"_n,
            sysio::const_mem_fun<message_entry, checksum256, &message_entry::by_msg_id>>
      >;

      /// Attestation extracted from a consensus-verified message, or
      /// queued outbound attestation awaiting envelope packing.
      struct [[sysio::table("attestations")]] attestation_entry {
         uint64_t                        id;
         uint64_t                        outpost_id;
         uint32_t                        epoch_index;
         opp::types::AttestationType     type;
         opp::types::AttestationStatus   status;
         std::vector<char>               data;
         uint64_t                        pending_timestamp;
         uint64_t                        ready_timestamp;
         uint64_t                        processed_timestamp;

         uint64_t by_status() const { return static_cast<uint64_t>(status); }
         uint64_t by_type()   const { return static_cast<uint64_t>(type); }
         uint64_t by_epoch()  const { return epoch_index; }

         SYSLIB_SERIALIZE(attestation_entry,
            (id)(outpost_id)(epoch_index)(type)(status)(data)
            (pending_timestamp)(ready_timestamp)(processed_timestamp))
      };

      using attestations_t = sysio::kv::table<"attestations"_n, id_key, attestation_entry,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<attestation_entry, uint64_t, &attestation_entry::by_status>>,
         sysio::kv::index<"bytype"_n,
            sysio::const_mem_fun<attestation_entry, uint64_t, &attestation_entry::by_type>>,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<attestation_entry, uint64_t, &attestation_entry::by_epoch>>
      >;

      /// Outbound envelope table.
      struct [[sysio::table("outenvelopes")]] outbound_envelope {
         uint64_t    id;
         uint64_t    outpost_id;
         uint32_t    epoch_index;
         checksum256 envelope_hash;
         checksum256 merkle_root;
         checksum256 start_message_id;
         checksum256 end_message_id;
         opp::types::EnvelopeStatus status;
         std::vector<char> raw_envelope;

         uint64_t by_outpost() const { return outpost_id; }
         uint64_t by_outpost_epoch() const {
            return (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;
         }

         SYSLIB_SERIALIZE(outbound_envelope,
            (id)(outpost_id)(epoch_index)(envelope_hash)(merkle_root)
            (start_message_id)(end_message_id)(status)(raw_envelope))
      };

      using outenvelopes_t = sysio::kv::table<"outenvelopes"_n, id_key, outbound_envelope,
         sysio::kv::index<"byoutpost"_n,
            sysio::const_mem_fun<outbound_envelope, uint64_t, &outbound_envelope::by_outpost>>,
         sysio::kv::index<"byoutepoch"_n,
            sysio::const_mem_fun<outbound_envelope, uint64_t, &outbound_envelope::by_outpost_epoch>>
      >;

      /// Per-outpost consensus primary key.
      struct outpost_consensus_key {
         uint64_t outpost_id;
         SYSLIB_SERIALIZE(outpost_consensus_key, (outpost_id))
      };

      /// Per-outpost consensus tracking for the current epoch.
      /// One row per outpost. Rows reused (not erased) to avoid RAM churn.
      struct [[sysio::table("outpcons")]] outpost_consensus_entry {
         uint64_t outpost_id;
         uint32_t epoch_index;
         bool     consensus_reached;

         SYSLIB_SERIALIZE(outpost_consensus_entry,
            (outpost_id)(epoch_index)(consensus_reached))
      };

      using outpost_consensus_t =
         sysio::kv::table<"outpcons"_n, outpost_consensus_key, outpost_consensus_entry>;

      /// Audit-trail row for the durable envelope log. Pure metadata —
      /// `endpoints` (start/end ChainId pair from the inbound or outbound
      /// envelope), the `epoch_index` it corresponds to, the keccak/sha256
      /// `checksum` of the encoded envelope bytes, and the `emitted_at`
      /// timestamp. Raw payload is consumed inline by the consensus +
      /// dispatch path and never stored. Off-chain audit reconstruction is
      /// out of scope.
      ///
      /// Total row count is capped at
      /// `active_outposts * 2 * epoch_retention_envelope_log_count`
      /// (one inbound + one outbound row per active outpost per epoch).
      /// Eviction is head-first on overflow — see
      /// `write_envelope_log` in `src/sysio.msgch.cpp`.
      struct [[sysio::table("envlog")]] envelope_log_entry {
         uint64_t        id;                ///< monotonic auto-increment PK
         opp::Endpoints  endpoints;         ///< start + end ChainId
         uint32_t        epoch_index;
         checksum256     checksum;          ///< sha256/keccak of envelope bytes
         time_point      emitted_at;

         uint64_t primary_key() const { return id; }

         SYSLIB_SERIALIZE(envelope_log_entry,
            (id)(endpoints)(epoch_index)(checksum)(emitted_at))
      };

      using envelope_log_t = sysio::kv::table<"envlog"_n, id_key, envelope_log_entry>;

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
