#include <sysio/chain/webassembly/sys-vm-oc/memory.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/intrinsic.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/intrinsic_mapping.hpp>

#include <fc/scoped_exit.hpp>

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/memfd.h>

namespace sysio { namespace chain { namespace sysvmoc {

memory::memory(uint64_t sliced_pages) {
   uint64_t number_slices = sliced_pages + 1;
   uint64_t wasm_memory_size = sliced_pages * wasm_constraints::wasm_page_size;
   int fd = syscall(SYS_memfd_create, "sysvmoc_mem", MFD_CLOEXEC);
   FC_ASSERT(fd >= 0, "Failed to create memory memfd");
   auto cleanup_fd = fc::make_scoped_exit([&fd](){close(fd);});
   int ret = ftruncate(fd, wasm_memory_size+memory_prologue_size);
   FC_ASSERT(!ret, "Failed to grow memory memfd");

   mapsize = total_memory_per_slice*number_slices;
   mapbase = (uint8_t*)mmap(nullptr, mapsize, PROT_NONE, MAP_PRIVATE|MAP_ANON, 0, 0);
   FC_ASSERT(mapbase != MAP_FAILED, "Failed to mmap memory");

   uint8_t* next_slice = mapbase;
   uint8_t* last = nullptr;

   for(unsigned int p = 0; p < number_slices; ++p) {
      last = (uint8_t*)mmap(next_slice, memory_prologue_size+64u*1024u*p, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0);
      FC_ASSERT(last != MAP_FAILED, "Failed to mmap memory");
      next_slice += total_memory_per_slice;
   }

   FC_ASSERT(last != nullptr, "expected last not nullptr");
   zeropage_base = mapbase + memory_prologue_size;
   fullpage_base = last + memory_prologue_size;

   //layout the intrinsic jump table
   uintptr_t* const intrinsic_jump_table = reinterpret_cast<uintptr_t* const>(zeropage_base - first_intrinsic_offset);
   const intrinsic_map_t& intrinsics = get_intrinsic_map();
   for(const auto& intrinsic : intrinsics)
      intrinsic_jump_table[-intrinsic.second.ordinal] = (uintptr_t)intrinsic.second.function_ptr;
}

memory::~memory() {
   munmap(mapbase, mapsize);
}

}}}
