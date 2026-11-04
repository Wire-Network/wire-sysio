#pragma once

#include <sysio/chain/controller.hpp>

#include <fc/log/logger.hpp>

#include <optional>
#include <span>

namespace sysio {

/// Where a block that a producer signed during its round ended up, judged against the chain as applied now.
enum class round_block_status {
   on_head,     ///< The applied chain holds this block at its height.
   orphaned,    ///< The applied chain holds a different block at its height, or none and the height is above LIB.
   unverifiable ///< The height is final, but this node keeps no block log copy to compare against.
};

/// A block signed during a producing round: its id and the id of the block it was built on.
struct round_signed_block {
   chain::block_id_type id;
   chain::block_id_type previous;
};

/// How many of a producing round's signed blocks ended up in each round_block_status.
struct round_block_counts {
   size_t on_head      = 0;
   size_t orphaned     = 0;
   size_t unverifiable = 0;
};

/**
 * Classifies one signed block by looking it up on the applied chain.
 *
 * Reversible heights are looked up on the applied head's branch in the fork database and final heights in the
 * block log, so a block that has become irreversible since it was signed counts as on head. Under Savanna LIB
 * trails head by about two blocks, so by the next round start most of a round's blocks are already final.
 *
 * Not thread-safe: uses controller::chain_block_id_for_num, so call it on the main thread. Each call takes the fork
 * database mutex, and the block log mutex for a final height. Throws if the block log read fails.
 */
inline round_block_status classify_round_block(const chain::controller& chain, const chain::block_id_type& id) {
   const chain::block_num_type                block_num      = chain::block_header::num_from_id(id);
   const std::optional<chain::block_id_type> chain_block_id = chain.chain_block_id_for_num(block_num);
   if (chain_block_id)
      return *chain_block_id == id ? round_block_status::on_head : round_block_status::orphaned;
   if (block_num <= chain.fork_db_root().block_num())
      return round_block_status::unverifiable;
   return round_block_status::orphaned;
}

/**
 * Classifies a producing round's signed blocks, given oldest first, for the round-complete summary.
 *
 * Walks the round newest to oldest. A block that is the `previous` of an on-head signed block is on head too, so only
 * a block whose newer signed block is not known to be on head is looked up with classify_round_block. A round of
 * consecutive blocks therefore costs one locked lookup instead of one per block. The inference also proves a final
 * block on head when the node keeps no block log, as long as its child is still in the fork database.
 *
 * A failed lookup counts that block as unverifiable and logs a warning, so the summary never fails block production.
 * Not thread-safe: call it on the main thread.
 */
inline round_block_counts classify_round_blocks(const chain::controller&           chain,
                                                std::span<const round_signed_block> blocks) {
   round_block_counts                  counts;
   std::optional<chain::block_id_type> previous_of_on_head; // `previous` of the newest block found on head so far
   for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
      round_block_status status = round_block_status::unverifiable;
      if (previous_of_on_head && *previous_of_on_head == it->id) {
         status = round_block_status::on_head;
      } else {
         try {
            status = classify_round_block(chain, it->id);
         } catch (const fc::exception& e) {
            wlog("Round summary could not look up signed block {}: {}", it->id, e.to_detail_string());
         }
      }
      switch (status) {
         case round_block_status::on_head:      ++counts.on_head;      break;
         case round_block_status::orphaned:     ++counts.orphaned;     break;
         case round_block_status::unverifiable: ++counts.unverifiable; break;
      }
      previous_of_on_head = status == round_block_status::on_head ? std::optional{it->previous} : std::nullopt;
   }
   return counts;
}

} // namespace sysio
