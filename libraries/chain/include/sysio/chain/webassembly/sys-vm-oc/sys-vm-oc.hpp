#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/sys-vm-oc.h>

#include <exception>

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#include <vector>
#include <list>

namespace sysio::chain::sysvmoc {

struct no_offset{};
struct code_offset {
   size_t offset; 
};    
struct intrinsic_ordinal { 
   size_t ordinal; 
};

using sysvmoc_optional_offset_or_import_t = std::variant<no_offset, code_offset, intrinsic_ordinal>;

struct code_descriptor {
   digest_type code_hash;
   uint8_t vm_version;
   uint8_t codegen_version;
   size_t code_begin;
   sysvmoc_optional_offset_or_import_t start;
   unsigned apply_offset;
   int starting_memory_pages;
   size_t initdata_begin;
   unsigned initdata_size;
   unsigned initdata_prologue_size;
};

enum sysvmoc_exitcode : int {
   SYSVMOC_EXIT_CLEAN_EXIT = 1,
   SYSVMOC_EXIT_CHECKTIME_FAIL,
   SYSVMOC_EXIT_SEGV,
   SYSVMOC_EXIT_EXCEPTION
};

static constexpr uint8_t current_codegen_version = 1;

}

FC_REFLECT(sysio::chain::sysvmoc::no_offset, );
FC_REFLECT(sysio::chain::sysvmoc::code_offset, (offset));
FC_REFLECT(sysio::chain::sysvmoc::intrinsic_ordinal, (ordinal));
FC_REFLECT(sysio::chain::sysvmoc::code_descriptor, (code_hash)(vm_version)(codegen_version)(code_begin)(start)(apply_offset)(starting_memory_pages)(initdata_begin)(initdata_size)(initdata_prologue_size));

#define SYSVMOC_INTRINSIC_INIT_PRIORITY __attribute__((init_priority(198)))
