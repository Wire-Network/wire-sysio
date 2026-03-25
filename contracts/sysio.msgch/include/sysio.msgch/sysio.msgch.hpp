#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>

namespace sysio {


   class [[sysio::contract("sysio.msgch")]] msgch : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Batch operator delivers inbound OPP message data (protobuf wire format).
      [[sysio::action]]
      void deliver(name batch_op_name, std::vector<char> data);

      /// Evaluate consensus on a chain request.
      [[sysio::action]]
      void evalcons(name batch_op_name, uint64_t req_id);

      /// Process a READY message (unpack attestations, route).
      [[sysio::action]]
      void processmsg(uint64_t msg_id);

      /// Queue an outbound message to an outpost.
      [[sysio::action]]
      void queueout(uint64_t outpost_id,
                    opp::types::AttestationType attest_type,
                    std::vector<char> data);

      /// Build outbound envelope from queued messages.
      [[sysio::action]]
      void buildenv(uint64_t outpost_id);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Inbound chain request table.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] inbound_chain_request {
         uint64_t    id;
         uint64_t    outpost_id;
         uint32_t    epoch_index;
         checksum256 expected_start_msg;
         opp::types::ChainRequestStatus status;
         uint32_t    delivery_count = 0;

         uint64_t primary_key() const { return id; }
         uint64_t by_outpost_epoch() const {
            return (outpost_id << 32) | epoch_index;
         }
      };

      using inchainreq_t = multi_index<"inchainreq"_n, inbound_chain_request,
         indexed_by<"byoutepoch"_n, const_mem_fun<inbound_chain_request, uint64_t,
                    &inbound_chain_request::by_outpost_epoch>>
      >;

      /// Chain delivery table.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] chain_delivery {
         uint64_t    id;
         uint64_t    chain_request_id;
         name        operator_account;
         checksum256 chain_hash;
         checksum256 merkle_root;
         uint32_t    message_count;
         time_point  delivered_at;
         bool        matches_consensus = false;

         uint64_t primary_key() const { return id; }
         uint64_t by_request() const { return chain_request_id; }
         uint64_t by_operator() const { return operator_account.value; }
      };

      using deliveries_t = multi_index<"deliveries"_n, chain_delivery,
         indexed_by<"byrequest"_n, const_mem_fun<chain_delivery, uint64_t, &chain_delivery::by_request>>,
         indexed_by<"byoperator"_n, const_mem_fun<chain_delivery, uint64_t, &chain_delivery::by_operator>>
      >;

      /// Message table.
      struct [[sysio::table, sysio::contract("sysio.msgch")]] message_entry {
         uint64_t    id;
         uint64_t    outpost_id;
         uint32_t    epoch_index;
         checksum256 message_id;
         checksum256 previous_message_id;
         opp::types::MessageDirection direction;
         opp::types::MessageStatus   status;
         opp::types::AttestationType attestation_type;
         std::vector<char> raw_payload;
         time_point  received_at;
         time_point  processed_at;

         uint64_t primary_key() const { return id; }
         uint64_t by_status() const { return static_cast<uint64_t>(status); }
         checksum256 by_msg_id() const { return message_id; }
      };

      using messages_t = multi_index<"messages"_n, message_entry,
         indexed_by<"bystatus"_n, const_mem_fun<message_entry, uint64_t, &message_entry::by_status>>,
         indexed_by<"bymsgid"_n, const_mem_fun<message_entry, checksum256, &message_entry::by_msg_id>>
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
      };

      using outenvelopes_t = multi_index<"outenvelopes"_n, outbound_envelope,
         indexed_by<"byoutpost"_n, const_mem_fun<outbound_envelope, uint64_t, &outbound_envelope::by_outpost>>
      >;

   private:

      using ChainRequestStatus = opp::types::ChainRequestStatus;
      using MessageDirection   = opp::types::MessageDirection;
      using MessageStatus      = opp::types::MessageStatus;
      using EnvelopeStatus     = opp::types::EnvelopeStatus;
      using AttestationType    = opp::types::AttestationType;
   };

} // namespace sysio
