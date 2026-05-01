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

   // Returns true if `id` is in this block's ancestry (or is this block
   // itself within the finality_core's tracking range).
   bool extends(const block_id_type& id) const {
      return _bsp && _bsp->core.extends(id);
   }

   // Returns true if this block carries a strong QC for a block that is not
   // in `head_handle`'s ancestry.
   //
   // Under Savanna a strong QC for some block B implies that at least 2/3 of
   // finalizer weight voted strong on B. The safety rule for strong votes
   // locks those finalizers on B, so they cannot subsequently vote in a way
   // that would let any branch not extending B form its own QC. Therefore if
   // `head_handle` is on a branch that does not include the QC target, no
   // block built on `head_handle` can ever be covered by a future QC, and
   // `head_handle`'s branch cannot win fork-choice -- it is permanently
   // locked out.
   //
   // Returns false when no strong QC is present.
   //
   // Thread-safety: safe to call concurrently with block production / apply.
   // `block_state_ptr` is a shared_ptr (its copy is atomic) and the
   // `finality_core` it references is immutable after construction. The
   // accessors used (`latest_qc_claim`, `get_block_reference`, `extends`)
   // are const reads against that immutable state.
   bool locks_out_branch_of(const block_handle& head_handle) const {
      if (!_bsp || !head_handle._bsp)
         return false;

      const auto qc = _bsp->core.latest_qc_claim();
      if (!qc.is_strong_qc)
         return false;

      const auto& this_id = _bsp->id();
      const auto& head_id = head_handle._bsp->id();

      // If head is on this block's branch (head is this block, or this block extends head),
      // they share the QC's chain -- head's branch can produce blocks that include the QC
      // target as an ancestor. Not locked out.
      if (head_id == this_id || _bsp->core.extends(head_id))
         return false;

      // If the QC target is in head's ancestry (or is head itself), head's branch already
      // includes the block the QC was formed for. Not locked out.
      const auto& qc_target_id = _bsp->core.get_block_reference(qc.block_num).block_id;
      if (head_id == qc_target_id || head_handle._bsp->core.extends(qc_target_id))
         return false;

      // Otherwise head's branch and the QC target are on incompatible branches: any block
      // built on head conflicts with the QC target, and no QC can ever be formed on head's
      // branch. Locked out.
      return true;
   }
};

} // namespace sysio::chain

FC_REFLECT(sysio::chain::block_handle, (_bsp))
