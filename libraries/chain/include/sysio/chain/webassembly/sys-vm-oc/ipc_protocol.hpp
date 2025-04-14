#pragma once

#include <sysio/chain/webassembly/sys-vm-oc/config.hpp>
#include <sysio/chain/webassembly/sys-vm-oc/sys-vm-oc.hpp>
#include <sysio/chain/types.hpp>

namespace sysio { namespace chain { namespace sysvmoc {

struct initialize_message {
   //Two sent fds: 1) communication socket for this instance  2) the cache file 
};

struct initalize_response_message {
   std::optional<std::string> error_message; //no error message? everything groovy
};

struct code_tuple {
   sysio::chain::digest_type code_id;
   uint8_t vm_version;
   bool operator==(const code_tuple& o) const {return o.code_id == code_id && o.vm_version == vm_version;}
};

struct compile_wasm_message {
   code_tuple code;
   sysvmoc::config sysvmoc_config;
   //Two sent fd: 1) communication socket for result, 2) the wasm to compile
};

struct evict_wasms_message {
   std::vector<code_descriptor> codes;
};

struct code_compilation_result_message {
   sysvmoc_optional_offset_or_import_t start;
   unsigned apply_offset;
   int starting_memory_pages;
   unsigned initdata_prologue_size;
   //Two sent fds: 1) wasm code, 2) initial memory snapshot
};


struct compilation_result_unknownfailure {};
struct compilation_result_toofull {};

using wasm_compilation_result = std::variant<code_descriptor,  //a successful compile
                                             compilation_result_unknownfailure,
                                             compilation_result_toofull>;

struct wasm_compilation_result_message {
   code_tuple code;
   wasm_compilation_result result;
   size_t cache_free_bytes;
};

using sysvmoc_message = std::variant<initialize_message,
                                     initalize_response_message,
                                     compile_wasm_message,
                                     evict_wasms_message,
                                     code_compilation_result_message,
                                     wasm_compilation_result_message>;
}}}

FC_REFLECT(sysio::chain::sysvmoc::initialize_message, )
FC_REFLECT(sysio::chain::sysvmoc::initalize_response_message, (error_message))
FC_REFLECT(sysio::chain::sysvmoc::code_tuple, (code_id)(vm_version))
FC_REFLECT(sysio::chain::sysvmoc::compile_wasm_message, (code)(sysvmoc_config))
FC_REFLECT(sysio::chain::sysvmoc::evict_wasms_message, (codes))
FC_REFLECT(sysio::chain::sysvmoc::code_compilation_result_message, (start)(apply_offset)(starting_memory_pages)(initdata_prologue_size))
FC_REFLECT(sysio::chain::sysvmoc::compilation_result_unknownfailure, )
FC_REFLECT(sysio::chain::sysvmoc::compilation_result_toofull, )
FC_REFLECT(sysio::chain::sysvmoc::wasm_compilation_result_message, (code)(result)(cache_free_bytes))
