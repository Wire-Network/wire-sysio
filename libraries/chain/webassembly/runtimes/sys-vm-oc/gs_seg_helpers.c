#include <sysio/chain/webassembly/sys-vm-oc/gs_seg_helpers.h>

#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <elf.h>
#include <immintrin.h>
#include <stdlib.h>

int arch_prctl(int code, unsigned long* addr);

#ifndef HWCAP2_FSGSBASE
#define HWCAP2_FSGSBASE (1 << 1)
#endif

#define SYSVMOC_MEMORY_PTR_cb_ptr GS_PTR struct sys_vm_oc_control_block* const cb_ptr = ((GS_PTR struct sys_vm_oc_control_block* const)(SYS_VM_OC_CONTROL_BLOCK_OFFSET));

int32_t sys_vm_oc_grow_memory(int32_t grow, int32_t max) {
   SYSVMOC_MEMORY_PTR_cb_ptr;
   uint64_t previous_page_count = cb_ptr->current_linear_memory_pages;
   int32_t grow_amount = grow;
   uint64_t max_pages = max;
   if(max_pages > cb_ptr->max_linear_memory_pages)
      max_pages = cb_ptr->max_linear_memory_pages;
   if(grow == 0)
      return (int32_t)cb_ptr->current_linear_memory_pages;
   if(previous_page_count + grow_amount > max_pages)
      return (int32_t)-1;

   int64_t max_segments = cb_ptr->execution_thread_memory_length / SYS_VM_OC_MEMORY_STRIDE - 1;
   int was_extended = previous_page_count > max_segments;
   int will_be_extended = previous_page_count + grow_amount > max_segments;
   char* extended_memory_start = cb_ptr->full_linear_memory_start + max_segments * 64*1024;
   int64_t gs_diff;
   if(will_be_extended && grow_amount > 0) {
      uint64_t skip = was_extended ? previous_page_count - max_segments : 0;
      gs_diff = was_extended ? 0 : max_segments - previous_page_count;
      mprotect(extended_memory_start + skip * 64*1024, (grow_amount - gs_diff) * 64*1024, PROT_READ | PROT_WRITE);
   } else if (was_extended && grow_amount < 0) {
      uint64_t skip = will_be_extended ? previous_page_count + grow_amount - max_segments : 0;
      gs_diff = will_be_extended ? 0 : previous_page_count + grow_amount - max_segments;
      mprotect(extended_memory_start + skip * 64*1024, (-grow_amount + gs_diff) * 64*1024, PROT_NONE);
   } else {
      gs_diff = grow_amount;
   }

   uint64_t current_gs = sys_vm_oc_getgs();
   current_gs += gs_diff * SYS_VM_OC_MEMORY_STRIDE;
   sys_vm_oc_setgs(current_gs);
   cb_ptr->current_linear_memory_pages += grow_amount;
   cb_ptr->first_invalid_memory_address += grow_amount*64*1024;

   if(grow_amount > 0)
      memset(cb_ptr->full_linear_memory_start + previous_page_count*64u*1024u, 0, grow_amount*64u*1024u);

   return (int32_t)previous_page_count;
}

sigjmp_buf* sys_vm_oc_get_jmp_buf() {
   SYSVMOC_MEMORY_PTR_cb_ptr;
   return cb_ptr->jmp;
}

void* sys_vm_oc_get_exception_ptr() {
   SYSVMOC_MEMORY_PTR_cb_ptr;
   return cb_ptr->eptr;
}

void* sys_vm_oc_get_bounce_buffer_list() {
   SYSVMOC_MEMORY_PTR_cb_ptr;
   return cb_ptr->bounce_buffers;
}

uint64_t sys_vm_oc_getgs_syscall() {
   uint64_t gs;
   arch_prctl(ARCH_GET_GS, &gs);
   return gs;
}

uint64_t __attribute__ ((__target__ ("fsgsbase"))) sys_vm_oc_getgs_fsgsbase() {
   return _readgsbase_u64();
}

void sys_vm_oc_setgs_syscall(uint64_t gs) {
   arch_prctl(ARCH_SET_GS, (unsigned long*)gs); //cast to a (unsigned long*) to match local declaration above
}

void __attribute__ ((__target__ ("fsgsbase"))) sys_vm_oc_setgs_fsgsbase(uint64_t gs) {
   return _writegsbase_u64(gs);
}

// Choose between the fsgsbase instructions and the arch_prctl syscall for
// accessing the GS base. SYSIO_DISABLE_FSGSBASE forces the syscall path, which
// is useful where userspace fsgsbase is advertised in HWCAP2 but not actually
// usable (some VMs). Resolved from the constructor below, after the dynamic
// loader has finished and before any threads start, so getenv()/getauxval() are
// safe to call here.
static int sys_vm_oc_use_fsgsbase() {
   if(getenv("SYSIO_DISABLE_FSGSBASE"))
      return 0;
   //see linux Documentation/arch/x86/x86_64/fsgs.rst; check that kernel has enabled userspace fsgsbase
   return getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE;
}

// sys_vm_oc_getgs/setgs were previously GNU ifuncs, but an ifunc resolver runs
// during early dynamic relocation -- before its own GOT entries are guaranteed
// relocated. The old resolver read the external _dl_argv symbol from that
// context to scan the environment, which crashed pre-main on some libc layouts
// (observed under WSL2). Resolving the implementation from a constructor avoids
// the fragile early-relocation context entirely. These helpers are called only
// at OC execution-context setup/teardown and on memory.grow -- never per wasm
// instruction -- so the added indirect call is off the hot path.
static uint64_t (*sys_vm_oc_getgs_impl)() = 0;
static void     (*sys_vm_oc_setgs_impl)(uint64_t) = 0;

static void sys_vm_oc_resolve_gs_helpers() {
   if(sys_vm_oc_use_fsgsbase()) {
      sys_vm_oc_getgs_impl = sys_vm_oc_getgs_fsgsbase;
      sys_vm_oc_setgs_impl = sys_vm_oc_setgs_fsgsbase;
   } else {
      sys_vm_oc_getgs_impl = sys_vm_oc_getgs_syscall;
      sys_vm_oc_setgs_impl = sys_vm_oc_setgs_syscall;
   }
}

__attribute__((constructor))
static void sys_vm_oc_init_gs_helpers() {
   sys_vm_oc_resolve_gs_helpers();
}

uint64_t sys_vm_oc_getgs() {
   if(!sys_vm_oc_getgs_impl) // the constructor normally runs first; guard a stray pre-constructor call
      sys_vm_oc_resolve_gs_helpers();
   return sys_vm_oc_getgs_impl();
}

void sys_vm_oc_setgs(uint64_t gs) {
   if(!sys_vm_oc_setgs_impl)
      sys_vm_oc_resolve_gs_helpers();
   sys_vm_oc_setgs_impl(gs);
}
