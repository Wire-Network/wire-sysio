#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── emitchain (FR-201/202/203) ──────────────────────────────────────────────
//
// An elected batch operator builds the outbound message chain for an epoch.
// Collects all queued outbound messages, computes a merkle root, signs,
// and stores the chain for the Outpost to fetch.

void depot::emitchain(name operator_account, uint64_t epoch_number) {
   require_auth(operator_account);

   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   // Verify operator is elected for this epoch
   verify_elected(operator_account, epoch_number);

   // Resolve operator id
   known_operators_table ops(get_self(), chain_scope);
   auto by_account = ops.get_index<"byaccount"_n>();
   auto op_it = by_account.find(operator_account.value);
   check(op_it != by_account.end(), "depot: operator not registered");

   // Check that outbound epoch hasn't already been emitted
   opp_epoch_out_table epoch_tbl(get_self(), chain_scope);
   auto ep_it = epoch_tbl.find(epoch_number);
   check(ep_it == epoch_tbl.end(), "depot: outbound chain already emitted for this epoch");

   // Collect outbound messages for this epoch
   // Messages are queued via queue_outbound_message() during crank processing
   opp_out_table out(get_self(), chain_scope);

   // Determine message range for this epoch
   // Use previous epoch's end_message as start, or 0 if first epoch
   uint64_t start_msg = 0;
   if (epoch_number > 0) {
      auto prev_ep = epoch_tbl.find(epoch_number - 1);
      if (prev_ep != epoch_tbl.end()) {
         start_msg = prev_ep->end_message;
      }
   }

   // FR-202: Build the outbound message chain payload
   // Serialize all outbound messages into a single chain envelope
   std::vector<char> chain_payload;
   uint64_t end_msg = start_msg;
   auto it = out.lower_bound(start_msg);

   while (it != out.end()) {
      // Append: assertion_type (2 bytes BE) + payload_len (4 bytes BE) + payload
      uint16_t atype = uint16_t(it->assertion_type);
      chain_payload.push_back(char(atype >> 8));
      chain_payload.push_back(char(atype & 0xFF));

      uint32_t plen = it->payload.size();
      chain_payload.push_back(char((plen >> 24) & 0xFF));
      chain_payload.push_back(char((plen >> 16) & 0xFF));
      chain_payload.push_back(char((plen >> 8)  & 0xFF));
      chain_payload.push_back(char(plen & 0xFF));

      chain_payload.insert(chain_payload.end(), it->payload.begin(), it->payload.end());

      end_msg = it->message_number + 1;
      ++it;
   }

   // Compute merkle root over the outbound messages
   // For a single-element chain, the merkle root is the hash of the payload
   checksum256 merkle = sha256(chain_payload.data(), chain_payload.size());

   // FR-203: Compute epoch hash (hash of merkle_root + prev_epoch_hash)
   checksum256 prev_epoch_hash = checksum256();
   if (epoch_number > 0) {
      message_chains_table chains(get_self(), chain_scope);
      auto by_epoch = chains.get_index<"byepochdir"_n>();
      uint128_t ek = (uint128_t(epoch_number - 1) << 8) | uint64_t(message_direction_outbound);
      auto chain_it = by_epoch.lower_bound(ek);
      if (chain_it != by_epoch.end() &&
          chain_it->epoch_number == epoch_number - 1 &&
          chain_it->direction == message_direction_outbound) {
         prev_epoch_hash = chain_it->epoch_hash;
      }
   }

   // epoch_hash = H(merkle_root || prev_epoch_hash)
   std::vector<char> hash_input;
   auto merkle_bytes = merkle.extract_as_byte_array();
   auto prev_bytes   = prev_epoch_hash.extract_as_byte_array();
   hash_input.insert(hash_input.end(), merkle_bytes.begin(), merkle_bytes.end());
   hash_input.insert(hash_input.end(), prev_bytes.begin(), prev_bytes.end());
   checksum256 epoch_hash = sha256(hash_input.data(), hash_input.size());

   // Store message chain record
   message_chains_table chains(get_self(), chain_scope);
   chains.emplace(get_self(), [&](auto& c) {
      c.id                 = chains.available_primary_key();
      c.direction          = message_direction_outbound;
      c.status             = chain_status_valid; // outbound is always valid
      c.epoch_number       = epoch_number;
      c.merkle_root        = merkle;
      c.epoch_hash         = epoch_hash;
      c.prev_epoch_hash    = prev_epoch_hash;
      c.payload            = chain_payload;
      c.operator_signature = {}; // TODO: operator signs with secp256k1 key
      c.operator_id        = op_it->id;
      c.created_at         = current_time_point();
   });

   // Store epoch tracking record
   epoch_tbl.emplace(get_self(), [&](auto& e) {
      e.epoch_number = epoch_number;
      e.start_message = start_msg;
      e.end_message   = end_msg;
      e.merkle_root   = merkle;
   });
}

} // namespace sysio
