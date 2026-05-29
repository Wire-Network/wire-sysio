#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
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

      /// Challenge primary key (auto-incrementing id).
      struct challenge_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(challenge_key, (id))
      };

      /// Challenge state table.
      struct [[sysio::table("challenges")]] challenge_entry {
         uint64_t    id;
         uint64_t    chain_request_id;
         uint32_t    epoch_index;
         uint8_t     round;            // 1 or 2
         opp::types::ChallengeStatus status;
         checksum256 challenge_hash;
         checksum256 response_hash;
         std::vector<name> correct_operators;
         std::vector<name> faulty_operators;
         time_point  challenged_at{};
         time_point  responded_at{};

         uint64_t by_request() const { return chain_request_id; }
         uint64_t by_epoch() const { return epoch_index; }

         SYSLIB_SERIALIZE(challenge_entry,
            (id)(chain_request_id)(epoch_index)(round)(status)(challenge_hash)(response_hash)
            (correct_operators)(faulty_operators)(challenged_at)(responded_at))
      };

      using challenges_t = sysio::kv::table<"challenges"_n, challenge_key, challenge_entry,
         sysio::kv::index<"byrequest"_n,
            sysio::const_mem_fun<challenge_entry, uint64_t, &challenge_entry::by_request>>,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<challenge_entry, uint64_t, &challenge_entry::by_epoch>>
      >;

      /// Manual resolution primary key (auto-incrementing id).
      struct resolution_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(resolution_key, (id))
      };

      /// Manual resolution table.
      struct [[sysio::table("resolutions")]] manual_resolution {
         uint64_t    id;
         uint64_t    challenge_id;
         checksum256 original_chain_hash;
         checksum256 round1_chain_hash;
         checksum256 round2_chain_hash;
         name        msig_proposal;
         bool        is_resolved = false;

         SYSLIB_SERIALIZE(manual_resolution,
            (id)(challenge_id)(original_chain_hash)(round1_chain_hash)
            (round2_chain_hash)(msig_proposal)(is_resolved))
      };

      using resolutions_t = sysio::kv::table<"resolutions"_n, resolution_key, manual_resolution>;

   private:
      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;
      static constexpr name OPREG_ACCOUNT = "sysio.opreg"_n;
      static constexpr name UWRIT_ACCOUNT = "sysio.uwrit"_n;
      static constexpr name MSIG_ACCOUNT  = "sysio.msig"_n;

      // ChallengeStatus constants (match protobuf values)
      using ChallengeStatus = opp::types::ChallengeStatus;

      static constexpr uint8_t MAX_AUTOMATIC_ROUNDS = 2;
   };

} // namespace sysio
