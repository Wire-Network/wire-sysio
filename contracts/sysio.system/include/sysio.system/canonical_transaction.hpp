#pragma once

#include <sysio/check.hpp>
#include <sysio/datastream.hpp>
#include <sysio/transaction.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

namespace sysiosystem {

/// Error text when packed transaction bytes are not the canonical encoding of what they decode to.
inline constexpr const char* non_canonical_transaction_error = "transaction is not canonically serialized";

/**
 * Read one `transaction` from `ds`, accepting it only when re-encoding it reproduces the bytes it was
 * read from. `ds` is left after the transaction; checking what follows is the caller's job.
 *
 * A contract that has the node check a packed transaction and then acts on its own decode of the
 * same bytes needs both readings to agree. Canonical bytes have exactly one reading, so the caller
 * sees the transaction the node sees.
 */
inline sysio::transaction unpack_canonical_transaction(sysio::datastream<const char*>& ds) {
   const char* const start = ds.pos();
   sysio::transaction trx;
   ds >> trx;
   const size_t consumed = static_cast<size_t>(ds.pos() - start);
   const std::vector<char> repacked = sysio::pack(trx);
   sysio::check(repacked.size() == consumed && std::memcmp(repacked.data(), start, consumed) == 0,
                non_canonical_transaction_error);
   return trx;
}

/**
 * Decode all `size` bytes at `data` as one `transaction` in canonical encoding, as
 * `unpack_canonical_transaction(datastream&)` does, with no bytes after it.
 */
inline sysio::transaction unpack_canonical_transaction(const char* data, size_t size) {
   sysio::datastream<const char*> ds(data, size);
   sysio::transaction trx = unpack_canonical_transaction(ds);
   sysio::check(ds.remaining() == 0, non_canonical_transaction_error);
   return trx;
}

} // namespace sysiosystem
