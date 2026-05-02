#pragma once

#include <sysio/chain/block_state.hpp>
#include <fc/reflect/reflect.hpp>

namespace sysio::chain {

// Created via controller::accept_block(const block_id_type& id, const signed_block_ptr& b)
// Valid to request id and signed_block_ptr it was created from.
struct block_handle {
private:
   block_state_ptr _bsp;

   friend struct fc::reflector<block_handle>;
   friend class controller;             // for `internal()` access from controller
   friend struct controller_impl;       // for `internal()` access from controller
   friend struct block_handle_accessor; // for `internal()` access from controller or tests

   // Avoid using internal block_state as those types are internal to controller.
   const auto& internal() const { return _bsp; }
   
public:
   block_handle() = default;
   explicit block_handle(block_state_ptr bsp) : _bsp(std::move(bsp)) {}

   bool is_valid() const { return !!_bsp; }

   block_num_type          block_num() const { return _bsp->block_num(); }
   block_num_type          irreversible_blocknum() const { return _bsp->irreversible_blocknum(); }
   block_timestamp_type    timestamp() const { return _bsp->timestamp(); }
   time_point              block_time() const { return time_point{_bsp->timestamp()}; }
   const block_id_type&    id() const { return _bsp->id(); }
   const block_id_type&    previous() const { return _bsp->previous(); }
   const signed_block_ptr& block() const { return _bsp->block; }
   const block_header&     header() const { return _bsp->header; }
   account_name            producer() const { return _bsp->producer(); }

   void write(const std::filesystem::path& state_file);
   bool read(const std::filesystem::path& state_file);

   // Returns true iff this block carries a strong QC whose target is not in `head_handle`'s ancestry. Under Savanna's
   // strong-vote locking, finalizers locked on the QC target cannot later vote on any branch that does not extend it,
   // so a head whose branch does not include the QC target can never be covered by a future QC; it is permanently
   // locked out of fork-choice.
   //
   // Returns false when no strong QC is present, when head extends this block, or when head extends the QC target.
   // Designed for `this == fork_db_head()` (the block with the newest QC); other callers are handled correctly too.
   //
   // Thread-safe: `finality_core` is immutable after block_state construction.
   bool locks_out_branch_of(const block_handle& head_handle) const {
      if (!_bsp || !head_handle._bsp)
         return false;

      const auto qc = _bsp->core.latest_qc_claim();
      if (!qc.is_strong_qc)
         return false;

      const auto& this_id = _bsp->id();
      const auto& head_id = head_handle._bsp->id();

      // Head is on this block's branch (head == this, or this extends head); they share the QC's chain.
      if (head_id == this_id || _bsp->core.extends(head_id))
         return false;

      // QC target is in head's ancestry; head's branch shares the QC's anchor. The block_num check covers qc_target
      // older than head's last_final (outside head's tracking window): finalization past qc_target is only possible
      // when qc_target is on head's branch.
      const auto& qc_target_id = _bsp->core.get_block_reference(qc.block_num).block_id;
      if (head_id == qc_target_id || head_handle._bsp->core.extends(qc_target_id) ||
          qc.block_num < head_handle._bsp->core.last_final_block_num())
         return false;

      // Head's branch and the QC target are incompatible; locked out.
      return true;
   }
};

} // namespace sysio::chain

FC_REFLECT(sysio::chain::block_handle, (_bsp))
