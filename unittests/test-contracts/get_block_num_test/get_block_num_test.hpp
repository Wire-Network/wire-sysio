#pragma once

#include <sysio/sysio.hpp>

using bytes = std::vector<char>;

namespace sysio {
   namespace internal_use_do_not_use {
      extern "C" {
         __attribute__((sysio_wasm_import))
         uint32_t get_block_num(); 
      }
   }
}

class [[sysio::contract]] get_block_num_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   void testblock(uint32_t expected_result);
};
