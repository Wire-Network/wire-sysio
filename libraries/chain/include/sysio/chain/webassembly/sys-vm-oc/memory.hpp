#pragma once

#include <sysio/chain/wasm_sysio_constraints.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/sys-vm-oc.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/intrinsic_mapping.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/gs_seg_helpers.h>

#include <stdint.h>
#include <stddef.h>

namespace sysio { namespace chain { namespace sysvmoc {

class memory {
      static constexpr uint64_t intrinsic_count                   = intrinsic_table_size();
      //warning: changing the following 3 params will invalidate existing PIC
      static constexpr uint64_t mutable_global_size               = 8u  * sysio::chain::wasm_constraints::maximum_mutable_globals/4u;
      static constexpr uint64_t table_size                        = 16u * sysio::chain::wasm_constraints::maximum_table_elements;
      static constexpr size_t   wcb_allowance                     = 512u;
      static_assert(sizeof(control_block) <= wcb_allowance, "SYS VM OC memory doesn't set aside enough memory for control block");

      //round up the prologue to multiple of 4K page
      static constexpr uint64_t memory_prologue_size = ((memory::wcb_allowance + mutable_global_size + table_size + intrinsic_count*UINT64_C(8))+UINT64_C(4095))/UINT64_C(4096)*UINT64_C(4096);
      //prologue + 8GB fault buffer + 4096 addtional buffer for safety
      static constexpr uint64_t total_memory_per_slice = memory_prologue_size + UINT64_C(0x200000000) + UINT64_C(4096);

   public:
      explicit memory(uint64_t sliced_pages);
      ~memory();
      memory(const memory&) = delete;
      memory& operator=(const memory&) = delete;

      uint8_t* const zero_page_memory_base() const { return zeropage_base; }
      uint8_t* const full_page_memory_base() const { return fullpage_base; }

      control_block* const get_control_block() const { return reinterpret_cast<control_block* const>(zeropage_base - cb_offset);}

      //these two are really only inteded for SEGV handling
      uint8_t* const start_of_memory_slices() const { return mapbase; }
      size_t size_of_memory_slice_mapping() const { return mapsize; }

      //to obtain memory protected for n wasm-pages, use the pointer computed from:
      //   zero_page_memory_base()+stride*n
      static constexpr size_t stride = total_memory_per_slice;

      //offsets to various interesting things in the memory
      static constexpr uintptr_t linear_memory = 0;
      static constexpr uintptr_t cb_offset = wcb_allowance + mutable_global_size + table_size;
      static constexpr uintptr_t first_intrinsic_offset = cb_offset + 8u;
      // The maximum amount of data that PIC code can include in the prologue
      static constexpr uintptr_t max_prologue_size = mutable_global_size + table_size;
      // Number of slices for read-only threads.
      // Use a small number to save upfront virtual memory consumption.
      // Memory uses beyond this limit will be handled by mprotect.
      static constexpr uint32_t sliced_pages_for_ro_thread = 10;

      // Changed from -cb_offset == SYS_VM_OC_CONTROL_BLOCK_OFFSET to get around
      // of compile warning about comparing integers of different signedness
      static_assert(SYS_VM_OC_CONTROL_BLOCK_OFFSET + cb_offset == 0, "SYS VM OC control block offset has slid out of place somehow");
      static_assert(stride == SYS_VM_OC_MEMORY_STRIDE, "SYS VM OC memory stride has slid out of place somehow");

   private:
      uint8_t* mapbase;
      uint64_t mapsize;

      uint8_t* zeropage_base;
      uint8_t* fullpage_base;
};

}}}

#define OFFSET_OF_CONTROL_BLOCK_MEMBER(M) (-(int)sysio::chain::sysvmoc::memory::cb_offset + (int)offsetof(sysio::chain::sysvmoc::control_block, M))
#define OFFSET_OF_FIRST_INTRINSIC ((int)-sysio::chain::sysvmoc::memory::first_intrinsic_offset)
