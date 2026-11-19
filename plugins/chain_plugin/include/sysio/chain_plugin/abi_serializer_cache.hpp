#pragma once

#include <sysio/chain/abi_serializer.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/time.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

namespace sysio::chain_apis {

/**
 * Least-recently-used cache of abi_serializers, keyed by the SHA-256 of the packed ABI each is built from.
 *
 * A key names exact ABI bytes, so an entry is valid wherever those bytes appear: at any account and on any fork.
 * Failed builds are cached as well and rethrown as copies. A build that ran out of time is retried by a caller with a
 * larger budget, or by any caller once `timed_out_retry_interval` has passed since it failed.
 *
 * Each entry is charged the size of its packed ABI plus `entry_overhead_bytes`; the least recently used entries are
 * dropped to keep the total within `max_bytes`. Safe to call from any thread; builds run outside the lock.
 */
class abi_serializer_cache {
public:
   /// Charge per entry on top of its packed ABI, for the state every serializer holds whatever its ABI.
   static constexpr size_t entry_overhead_bytes = 8 * 1024;
   /// How long a build that ran out of time is reported as failed before a caller with the same budget retries it.
   static constexpr fc::microseconds timed_out_retry_interval = fc::seconds(10);
   /// Bound of the cache that make_resolver uses.
   static constexpr size_t default_max_bytes = 16 * 1024 * 1024;

   explicit abi_serializer_cache(size_t max_bytes);

   /**
    * The serializer for the packed ABI `abi`, built on a miss under a `max_build_time` budget that covers unpacking the
    * ABI as well as setting it.
    *
    * @return nullptr when `abi` holds no ABI
    * @throws the exception building the serializer raised, whether now or when its failure was cached
    */
   std::shared_ptr<const chain::abi_serializer> get(std::span<const char> abi, const fc::microseconds& max_build_time);

   /// Entries held, including failed builds.
   size_t size() const;
   /// Bytes charged for the entries held.
   size_t charged_bytes() const;
   /// Builds attempted since construction.
   uint64_t builds() const;

private:
   /// The outcome of one build: a serializer, or the exception that ended it.
   struct entry {
      std::shared_ptr<const chain::abi_serializer> serializer;
      fc::exception_ptr                            error;
      /// The build ran past its budget; such a failure is not final.
      bool                                         timed_out = false;
      /// The budget the build had.
      fc::microseconds                             build_budget;
      /// When the build failed.
      fc::time_point                               failed_at;
      size_t                                       charge = 0;
   };
   using lru_list = std::list<std::pair<fc::sha256, entry>>;

   /// Builds the serializer for `abi` under `max_build_time`, capturing any failure in the entry.
   static entry build(std::span<const char> abi, const fc::microseconds& max_build_time);
   /// True when a caller with `max_build_time` must build `cached` again rather than reuse it.
   static bool must_rebuild(const entry& cached, const fc::microseconds& max_build_time);
   /// The serializer `outcome` holds, or a copy of its exception thrown.
   static std::shared_ptr<const chain::abi_serializer> result_of(const entry& outcome);

   /// Stores `outcome` under `digest` as the most recently used entry, then drops entries past the byte bound.
   void store(const fc::sha256& digest, const entry& outcome);

   const size_t                                           _max_bytes;
   mutable std::mutex                                     _mtx;
   lru_list                                               _lru; ///< most recently used first
   std::unordered_map<fc::sha256, lru_list::iterator>     _index;
   size_t                                                 _charged_bytes = 0;
   uint64_t                                               _builds = 0;
};

/// The cache every resolver from make_resolver shares.
abi_serializer_cache& shared_abi_serializer_cache();

} // namespace sysio::chain_apis
