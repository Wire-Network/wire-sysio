#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>

namespace sysio {

   class [[sysio::contract("sysio.chalg")]] chalg : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Initiate challenge (called by sysio.msgch on consensus failure).
      [[sysio::action]]
      void initchal(uint64_t chain_req_id);

      /// Process challenge response from outpost.
      [[sysio::action]]
      void submitresp(uint64_t challenge_id,
                      checksum256 response_hash,
                      std::vector<name> correct_ops,
                      std::vector<name> faulty_ops);

      /// Escalate to round 2 or manual resolution.
      [[sysio::action]]
      void escalate(uint64_t challenge_id);

      /// T1/T2 submits manual resolution referencing sysio.msig proposal.
      [[sysio::action]]
      void submitres(name submitter,
                     uint64_t challenge_id,
                     checksum256 orig_hash,
                     checksum256 r1_hash,
                     checksum256 r2_hash);

      /// Enforce resolution after sysio.msig approval (2/3 vote).
      [[sysio::action]]
      void enforce(uint64_t resolution_id);

      /// Execute slash on an operator.
      [[sysio::action]]
      void slashop(name operator_acct, std::string reason);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Challenge state table.
      struct [[sysio::table, sysio::contract("sysio.chalg")]] challenge_entry {
         uint64_t    id;
         uint64_t    chain_request_id;
         uint32_t    epoch_index;
         uint8_t     round;            // 1 or 2
         opp::types::ChallengeStatus status;
         checksum256 challenge_hash;
         checksum256 response_hash;
         std::vector<name> correct_operators;
         std::vector<name> faulty_operators;
         time_point  challenged_at;
         time_point  responded_at;

         uint64_t primary_key() const { return id; }
         uint64_t by_request() const { return chain_request_id; }
         uint64_t by_epoch() const { return epoch_index; }
      };

      using challenges_t = multi_index<"challenges"_n, challenge_entry,
         indexed_by<"byrequest"_n, const_mem_fun<challenge_entry, uint64_t, &challenge_entry::by_request>>,
         indexed_by<"byepoch"_n, const_mem_fun<challenge_entry, uint64_t, &challenge_entry::by_epoch>>
      >;

      /// Manual resolution table.
      struct [[sysio::table, sysio::contract("sysio.chalg")]] manual_resolution {
         uint64_t    id;
         uint64_t    challenge_id;
         checksum256 original_chain_hash;
         checksum256 round1_chain_hash;
         checksum256 round2_chain_hash;
         name        msig_proposal;
         bool        is_resolved = false;

         uint64_t primary_key() const { return id; }
      };

      using resolutions_t = multi_index<"resolutions"_n, manual_resolution>;

   private:
      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;
      static constexpr name UWRIT_ACCOUNT = "sysio.uwrit"_n;
      static constexpr name MSIG_ACCOUNT  = "sysio.msig"_n;

      // ChallengeStatus constants (match protobuf values)
      using ChallengeStatus = opp::types::ChallengeStatus;

      static constexpr uint8_t MAX_AUTOMATIC_ROUNDS = 2;
   };

} // namespace sysio
