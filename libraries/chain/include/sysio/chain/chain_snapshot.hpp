#pragma once

#include <sysio/chain/exceptions.hpp>

namespace sysio::chain {

struct chain_snapshot_header {
   /**
    * Version history
    *   1: initial WIRE version
    */

   static constexpr uint32_t minimum_compatible_version = 1;
   static constexpr uint32_t current_version = 1;

   static constexpr uint32_t first_version_with_split_table_sections = 1;

   uint32_t version = current_version;

   void validate() const {
      auto min = minimum_compatible_version;
      auto max = current_version;
      SYS_ASSERT(version >= min && version <= max,
              snapshot_validation_exception,
              "Unsupported version of chain snapshot: ${version}. Supported version must be between ${min} and ${max} inclusive.",
              ("version",version)("min",min)("max",max));
   }
};

} // namespace sysio::chain

FC_REFLECT(sysio::chain::chain_snapshot_header,(version))
