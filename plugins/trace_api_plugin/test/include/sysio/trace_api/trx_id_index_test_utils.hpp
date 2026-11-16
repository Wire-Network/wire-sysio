#pragma once

#include <sysio/chain/types.hpp>

#include <cstring>

namespace sysio::trace_api::test_common {

/// Build a transaction ID with a controlled native hash prefix and an optional distinct final byte.
inline chain::transaction_id_type make_trx_id(uint64_t prefix64, uint8_t disambiguator = 0) {
   chain::transaction_id_type id;
   std::memcpy(id.data(), &prefix64, sizeof(prefix64));
   id.data()[id.data_size() - 1] = disambiguator;
   return id;
}

} // namespace sysio::trace_api::test_common
