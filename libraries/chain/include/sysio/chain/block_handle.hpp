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
};

} // namespace sysio::chain

FC_REFLECT(sysio::chain::block_handle, (_bsp))
