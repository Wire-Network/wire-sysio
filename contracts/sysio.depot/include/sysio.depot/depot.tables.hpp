#pragma once

#include <sysio/sysio.hpp>

#include <sysio.depot/depot.types.hpp>
#include <sysio/singleton.hpp>
#include <sysio/crypto.hpp>

#include <cmath>
#include <limits>


namespace sysio {

   // ═══════════════════════════════════════════════════════════════════════════
   // Global depot state (singleton)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("depotstate"), sysio::contract("sysio.depot")]] depot_global_state {
      depot_state_t  state       = depot_state_active;
      chain_kind_t   chain_id    = fc::crypto::chain_kind_unknown;
      uint64_t       current_epoch  = 0;
      uint64_t       next_epoch     = 1;
      uint64_t       next_msg_out   = 0; // monotonic outbound message counter
      time_point_sec last_crank_time;
      name           token_contract; // sysio.token or similar
      bool           initialized = false;

      SYSLIB_SERIALIZE(depot_global_state,
         (state)(chain_id)(current_epoch)(next_epoch)(next_msg_out)
         (last_crank_time)(token_contract)(initialized))
   };
   using depot_state_singleton = sysio::singleton<"depotstate"_n, depot_global_state>;

   // ═══════════════════════════════════════════════════════════════════════════
   // Known operators — scoped by chain_kind_t  (FR-601)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("knownops"), sysio::contract("sysio.depot")]] known_operator {
      uint64_t          id;
      operator_type_t   op_type;
      operator_status_t status;
      name              wire_account;
      std::vector<char> secp256k1_pubkey; // 33 bytes compressed
      std::vector<char> ed25519_pubkey;   // 32 bytes
      asset             collateral;
      time_point_sec    registered_at;
      time_point_sec    status_changed_at;

      uint64_t    primary_key()    const { return id; }
      uint128_t   by_type_status() const { return (uint128_t(op_type) << 64) | uint64_t(status); }
      uint64_t    by_account()     const { return wire_account.value; }
      checksum256 by_secp_pubkey() const {
         return sha256(secp256k1_pubkey.data(), secp256k1_pubkey.size());
      }
   };

   using known_operators_table = multi_index<"knownops"_n, known_operator,
      indexed_by<"bytypestatus"_n, const_mem_fun<known_operator, uint128_t, &known_operator::by_type_status>>,
      indexed_by<"byaccount"_n,    const_mem_fun<known_operator, uint64_t,  &known_operator::by_account>>,
      indexed_by<"bysecppub"_n,    const_mem_fun<known_operator, checksum256, &known_operator::by_secp_pubkey>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // Operator election schedule — scoped by chain_kind_t  (FR-604)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("opschedule"), sysio::contract("sysio.depot")]] op_schedule {
      uint64_t              epoch_number;
      std::vector<uint64_t> elected_operator_ids; // MAX_BATCH_OPERATORS_PER_EPOCH entries
      time_point_sec        created_at;

      uint64_t primary_key() const { return epoch_number; }
   };

   using op_schedule_table = multi_index<"opschedule"_n, op_schedule>;

   // ═══════════════════════════════════════════════════════════════════════════
   // Message chain — scoped by chain_kind_t  (FR-101)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("msgchains"), sysio::contract("sysio.depot")]] message_chain {
      uint64_t            id;
      message_direction_t direction;
      chain_status_t      status;
      uint64_t            epoch_number;
      checksum256         merkle_root;
      checksum256         epoch_hash;
      checksum256         prev_epoch_hash;
      std::vector<char>   payload;
      std::vector<char>   operator_signature;
      uint64_t            operator_id; // ref → known_operator
      time_point_sec      created_at;

      uint64_t  primary_key()  const { return id; }
      uint128_t by_epoch_dir() const { return (uint128_t(epoch_number) << 8) | uint64_t(direction); }
      uint64_t  by_status()    const { return uint64_t(status); }
   };

   using message_chains_table = multi_index<"msgchains"_n, message_chain,
      indexed_by<"byepochdir"_n, const_mem_fun<message_chain, uint128_t, &message_chain::by_epoch_dir>>,
      indexed_by<"bystatus"_n,   const_mem_fun<message_chain, uint64_t,  &message_chain::by_status>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // Reserve balance state — scoped by chain_kind_t  (FR-701)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("reserves"), sysio::contract("sysio.depot")]] reserve_balance {
      asset reserve_total;
      asset wire_equivalent;

      uint64_t primary_key() const { return reserve_total.symbol.code().raw(); }
   };

   using reserves_table = multi_index<"reserves"_n, reserve_balance>;

   // ═══════════════════════════════════════════════════════════════════════════
   // Underwriting ledger — scoped by chain_kind_t  (FR-401)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("uwledger"), sysio::contract("sysio.depot")]] underwrite_entry {
      uint64_t            id;
      uint64_t            operator_id;
      underwrite_status_t status;
      asset               source_amount;
      asset               target_amount;
      chain_kind_t        source_chain;
      chain_kind_t        target_chain;
      uint64_t            exchange_rate_bps;
      time_point_sec      unlock_at;
      time_point_sec      created_at;
      checksum256         source_tx_hash;
      checksum256         target_tx_hash;

      uint64_t primary_key()     const { return id; }
      uint64_t by_underwriter()  const { return operator_id; }
      uint64_t by_status()       const { return uint64_t(status); }
      uint64_t by_expiry()       const { return unlock_at.sec_since_epoch(); }
   };

   using underwrite_table = multi_index<"uwledger"_n, underwrite_entry,
      indexed_by<"byuw"_n,     const_mem_fun<underwrite_entry, uint64_t, &underwrite_entry::by_underwriter>>,
      indexed_by<"bystatus"_n, const_mem_fun<underwrite_entry, uint64_t, &underwrite_entry::by_status>>,
      indexed_by<"byexpiry"_n, const_mem_fun<underwrite_entry, uint64_t, &underwrite_entry::by_expiry>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // Inbound epoch tracking — scoped by chain_kind_t
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("oppepochin"), sysio::contract("sysio.depot")]] opp_epoch_in {
      uint64_t    epoch_number;
      uint64_t    start_message;
      uint64_t    end_message;
      checksum256 epoch_merkle;
      bool        challenge_flag = false;

      uint64_t primary_key() const { return epoch_number; }
   };

   using opp_epoch_in_table = multi_index<"oppepochin"_n, opp_epoch_in>;

   // ═══════════════════════════════════════════════════════════════════════════
   // Outbound epoch tracking — scoped by chain_kind_t
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("oppepochout"), sysio::contract("sysio.depot")]] opp_epoch_out {
      uint64_t    epoch_number;
      uint64_t    start_message;
      uint64_t    end_message;
      checksum256 merkle_root;

      uint64_t primary_key() const { return epoch_number; }
   };

   using opp_epoch_out_table = multi_index<"oppepochout"_n, opp_epoch_out>;

   // ═══════════════════════════════════════════════════════════════════════════
   // Inbound message queue — scoped by chain_kind_t
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("oppin"), sysio::contract("sysio.depot")]] opp_message_in {
      uint64_t          message_number;
      assertion_type_t  assertion_type;
      message_status_t  status;
      std::vector<char> payload;

      uint64_t primary_key() const { return message_number; }
      uint64_t by_status()   const { return uint64_t(status); }
   };

   using opp_in_table = multi_index<"oppin"_n, opp_message_in,
      indexed_by<"bystatus"_n, const_mem_fun<opp_message_in, uint64_t, &opp_message_in::by_status>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // Outbound message queue — scoped by chain_kind_t
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("oppout"), sysio::contract("sysio.depot")]] opp_message_out {
      uint64_t          message_number;
      assertion_type_t  assertion_type;
      std::vector<char> payload;

      uint64_t primary_key() const { return message_number; }
   };

   using opp_out_table = multi_index<"oppout"_n, opp_message_out>;

   // ═══════════════════════════════════════════════════════════════════════════
   // Challenge tracking — scoped by chain_kind_t  (FR-500)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("challenges"), sysio::contract("sysio.depot")]] challenge_info {
      uint64_t           id;
      uint64_t           epoch_number;
      challenge_status_t status;
      uint8_t            round;
      std::vector<char>  challenge_data;

      uint64_t primary_key() const { return id; }
      uint64_t by_epoch()    const { return epoch_number; }
   };

   using challenges_table = multi_index<"challenges"_n, challenge_info,
      indexed_by<"byepoch"_n, const_mem_fun<challenge_info, uint64_t, &challenge_info::by_epoch>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // Epoch consensus vote tracking — scoped by chain_kind_t  (FR-301)
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("epochvotes"), sysio::contract("sysio.depot")]] epoch_vote {
      uint64_t       id;
      uint64_t       epoch_number;
      uint64_t       operator_id;
      checksum256    chain_hash;
      time_point_sec submitted_at;

      uint64_t  primary_key()  const { return id; }
      uint128_t by_epoch_op()  const { return (uint128_t(epoch_number) << 64) | operator_id; }
      uint64_t  by_epoch()     const { return epoch_number; }
   };

   using epoch_votes_table = multi_index<"epochvotes"_n, epoch_vote,
      indexed_by<"byepochop"_n, const_mem_fun<epoch_vote, uint128_t, &epoch_vote::by_epoch_op>>,
      indexed_by<"byepoch"_n,   const_mem_fun<epoch_vote, uint64_t,  &epoch_vote::by_epoch>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // OPP fork tracking — scoped by chain_kind_t
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("oppforks"), sysio::contract("sysio.depot")]] opp_fork {
      uint64_t    fork_id;
      uint64_t    epoch_number;
      uint64_t    end_message_id;
      checksum256 merkle_root;

      uint64_t primary_key() const { return fork_id; }
      uint64_t by_epoch()    const { return epoch_number; }
   };

   using opp_forks_table = multi_index<"oppforks"_n, opp_fork,
      indexed_by<"byepoch"_n, const_mem_fun<opp_fork, uint64_t, &opp_fork::by_epoch>>
   >;

   // ═══════════════════════════════════════════════════════════════════════════
   // OPP fork votes — scoped by chain_kind_t
   // ═══════════════════════════════════════════════════════════════════════════
   struct [[sysio::table("oppforkvote"), sysio::contract("sysio.depot")]] opp_fork_vote {
      uint64_t id;
      uint64_t fork_id;
      name     voter;
      uint8_t  vote_state; // 0=pending, 1=accept, 2=reject

      uint64_t  primary_key()   const { return id; }
      uint128_t by_fork_user()  const { return (uint128_t(fork_id) << 64) | voter.value; }
   };

   using opp_fork_votes_table = multi_index<"oppforkvote"_n, opp_fork_vote,
      indexed_by<"byforkuser"_n, const_mem_fun<opp_fork_vote, uint128_t, &opp_fork_vote::by_fork_user>>
   >;

} // namespace sysio
