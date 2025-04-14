#pragma once

#include <string> // std::string

namespace sysio { namespace version {

   ///< Helper function for `version_client()`
   std::string _version_client();

   ///< Helper function for `version_full()`
   std::string _version_full();

   ///< Helper function for `version_hash()`
   std::string _version_hash();

} }
