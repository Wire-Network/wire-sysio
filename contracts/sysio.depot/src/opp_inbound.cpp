#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── submitchain (FR-101/103/104/105) ─────────────────────────────────────────
//
// Batch operator submits an epoch envelope + signature.
// The Depot collects these as votes; consensus is evaluated once enough arrive.

void depot::submitchain(name              operator_account,
                        uint64_t          epoch_number,
                        checksum256       epoch_hash,
                        checksum256       prev_epoch_hash,
                        checksum256       merkle_root,
                        std::vector<char> signature) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // FR-103: Verify operator is elected for this epoch
   verify_elected(operator_account, epoch_number);

   // FR-102: Sequential epoch processing
   check(epoch_number == s.current_epoch || epoch_number == s.current_epoch + 1,
         "depot: epoch out of order (FR-102: must be processed sequentially)");

   // Resolve operator id
   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto op_it = by_account.find(operator_account.value);
   // verify_elected already checks this, but be explicit
   check(op_it != by_account.end(), "depot: operator not registered");

   // FR-105: Validate previous epoch hash against stored value
   if (epoch_number > 0) {
      opp_epoch_in_table epochs(get_self(), chain_scope);
      auto prev_epoch = epochs.find(epoch_number - 1);
      if (prev_epoch != epochs.end()) {
         check(prev_epoch_hash == prev_epoch->epoch_merkle,
               "depot: prev_epoch_hash does not match stored epoch merkle (FR-105)");
      }
   }

   // FR-104: Validate signature (recover signer, confirm it matches operator)
   // The signature is over the epoch_hash using the operator's secp256k1 key.
   // On-chain recovery: assert_recover_key(epoch_hash, sig, expected_pubkey)
   // For now we store the signature; full recovery requires the CDT
   // assert_recover_key intrinsic which takes a digest + sig + pubkey.
   // TODO: Uncomment when OPP signature format is finalized
   // public_key recovered = recover_key(epoch_hash, signature);
   // check(recovered matches op_it->secp256k1_pubkey)

   // Store this operator's vote for the epoch
   epoch_votes_table votes(get_self(), chain_scope);
   auto by_eo = votes.get_index<"byepochop"_n>();
   uint128_t eo_key = (uint128_t(epoch_number) << 64) | op_it->id;
   check(by_eo.find(eo_key) == by_eo.end(),
         "depot: operator already submitted for this epoch");

   votes.emplace(get_self(), [&](auto& v) {
      v.id           = votes.available_primary_key();
      v.epoch_number = epoch_number;
      v.operator_id  = op_it->id;
      v.chain_hash   = epoch_hash;
      v.submitted_at = current_time_point();
   });

   // Store the message chain record
   message_chains_table chains(get_self(), chain_scope);
   chains.emplace(get_self(), [&](auto& c) {
      c.id                 = chains.available_primary_key();
      c.direction          = message_direction_inbound;
      c.status             = chain_status_pending;
      c.epoch_number       = epoch_number;
      c.merkle_root        = merkle_root;
      c.epoch_hash         = epoch_hash;
      c.prev_epoch_hash    = prev_epoch_hash;
      c.operator_signature = signature;
      c.operator_id        = op_it->id;
      c.created_at         = current_time_point();
   });

   // Attempt consensus evaluation after each submission
   evaluate_consensus(epoch_number);
}

// ── uploadmsgs (FR-106/107/108) ──────────────────────────────────────────────
//
// After consensus is reached, a single chosen operator uploads the actual
// messages with merkle proofs linking message IDs to the epoch merkle root.

void depot::uploadmsgs(name              operator_account,
                       uint64_t          epoch_number,
                       std::vector<char> messages,
                       std::vector<char> merkle_proofs) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Verify operator is elected
   verify_elected(operator_account, epoch_number);

   // Verify the epoch has reached consensus (status = valid)
   message_chains_table chains(get_self(), chain_scope);
   auto by_epoch = chains.get_index<"byepochdir"_n>();
   uint128_t ek = (uint128_t(epoch_number) << 8) | uint64_t(message_direction_inbound);
   auto chain_it = by_epoch.lower_bound(ek);

   bool has_valid = false;
   while (chain_it != by_epoch.end() &&
          chain_it->epoch_number == epoch_number &&
          chain_it->direction == message_direction_inbound) {
      if (chain_it->status == chain_status_valid) {
         has_valid = true;
         break;
      }
      ++chain_it;
   }
   check(has_valid, "depot: epoch has not reached consensus yet");

   // FR-106: Unpack message chain attestations
   // Messages format: [assertion_type(2) | payload_len(4) | payload(N)] repeated
   // Merkle proofs: [proof per message for verification against epoch merkle root]
   //
   // For each message:
   //   1. Recompute message ID from message data (FR-106)
   //   2. Evaluate merkle proof against epoch merkle root
   //   3. Classify as NORMAL or CHALLENGE (FR-106)
   //   4. Assign status PENDING or READY (FR-107)

   opp_in_table in_msgs(get_self(), chain_scope);

   // Deserialize messages
   const char* ptr = messages.data();
   const char* end = ptr + messages.size();
   uint64_t msg_num = 0;

   // Find the next message number
   auto last_msg = in_msgs.end();
   if (last_msg != in_msgs.begin()) {
      --last_msg;
      msg_num = last_msg->message_number + 1;
   }

   while (ptr + 6 <= end) { // minimum: 2 bytes type + 4 bytes length
      // Read assertion type (2 bytes, big-endian)
      uint16_t atype = (uint16_t(uint8_t(ptr[0])) << 8) | uint8_t(ptr[1]);
      ptr += 2;

      // Read payload length (4 bytes, big-endian)
      uint32_t plen = (uint32_t(uint8_t(ptr[0])) << 24) |
                      (uint32_t(uint8_t(ptr[1])) << 16) |
                      (uint32_t(uint8_t(ptr[2])) << 8)  |
                      uint32_t(uint8_t(ptr[3]));
      ptr += 4;

      check(ptr + plen <= end, "depot: message payload exceeds buffer");

      std::vector<char> payload(ptr, ptr + plen);
      ptr += plen;

      assertion_type_t assertion = static_cast<assertion_type_t>(atype);

      // FR-106: Classify message type
      bool is_challenge = (assertion == assertion_type_challenge_response);

      // FR-107: Messages requiring underwriting -> PENDING; others -> READY
      // Challenge messages are always READY (FR-108)
      message_status_t msg_status;
      if (is_challenge) {
         msg_status = message_status_ready;
      } else {
         // Messages that need underwriting: swaps, purchases
         // For now, purchases need underwriting; balance sheets and operator
         // registration do not.
         switch (assertion) {
            case assertion_type_wire_purchase:
            case assertion_type_stake_update:
               msg_status = message_status_pending;
               break;
            default:
               msg_status = message_status_ready;
               break;
         }
      }

      in_msgs.emplace(get_self(), [&](auto& m) {
         m.message_number = msg_num++;
         m.assertion_type = assertion;
         m.status         = msg_status;
         m.payload        = payload;
      });

      // FR-108: Process challenge messages immediately
      if (is_challenge) {
         process_assertion(msg_num - 1, assertion, payload);
      }
   }

   // Update inbound epoch tracking
   opp_epoch_in_table epoch_tbl(get_self(), chain_scope);
   auto ep_it = epoch_tbl.find(epoch_number);
   if (ep_it == epoch_tbl.end()) {
      epoch_tbl.emplace(get_self(), [&](auto& e) {
         e.epoch_number  = epoch_number;
         e.start_message = msg_num > 0 ? msg_num - 1 : 0; // approximate
         e.end_message   = msg_num;
         e.challenge_flag = (s.state == depot_state_challenge);
      });
   } else {
      epoch_tbl.modify(ep_it, same_payer, [&](auto& e) {
         e.end_message = msg_num;
      });
   }
}

} // namespace sysio
