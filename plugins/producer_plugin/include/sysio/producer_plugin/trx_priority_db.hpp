#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <sysio/chain/block.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/transaction.hpp>

namespace sysio {

namespace producer_plugin_detail {

/**
 * Shared pointer snapshot slot using the standard atomic specialization.
 */
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
template <typename T>
using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
#else
/**
 * Shared pointer snapshot slot with mutex-backed atomic semantics.
 *
 * Some AppleClang/libc++ toolchains do not provide the C++20 `std::atomic<std::shared_ptr<T>>` specialization, and
 * the older `std::atomic_load` / `std::atomic_store` free functions are deprecated. This fallback exposes only the
 * operations this project currently needs: load the current immutable snapshot and replace it with a new snapshot.
 */
template <typename T>
class atomic_shared_ptr {
public:
   using ptr_type = std::shared_ptr<T>;

   atomic_shared_ptr() = default;
   atomic_shared_ptr(const atomic_shared_ptr&) = delete;
   atomic_shared_ptr& operator=(const atomic_shared_ptr&) = delete;

   /**
    * Return a shared ownership copy of the current pointer.
    */
   ptr_type load(std::memory_order order = std::memory_order_seq_cst) const {
      // Mutex provides sequentially-consistent semantics regardless of the requested order, so this is a no-op here.
      (void)order;
      std::scoped_lock lock{_mutex};
      return _ptr;
   }

   /**
    * Replace the current pointer with `ptr`.
    */
   void store(ptr_type ptr, std::memory_order order = std::memory_order_seq_cst) {
      // Mutex provides sequentially-consistent semantics regardless of the requested order, so this is a no-op here.
      (void)order;
      std::scoped_lock lock{_mutex};
      _ptr = std::move(ptr);
   }

private:
   mutable std::mutex _mutex{};
   ptr_type _ptr{};
};
#endif

} // namespace producer_plugin_detail

/**
 * Manages transaction priorities in a thread-safe manner.
 *
 * Periodically retrieves transaction priorities from system contract `trxpriority` table on LIB signal.
 */
class trx_priority_db {
public:
   constexpr static chain::block_num_type trx_priority_refresh_interval = 1000;
   constexpr static fc::microseconds serializer_max_time = fc::milliseconds(30);

   trx_priority_db() = default;

   /**
    * Thread safe
    * @return priority of trx, returns priority::low if trx has no configured priority
    */
   int get_trx_priority(const chain::transaction& trx) const;

   /**
    * Called from main thread
    */
   void on_irreversible_block(const chain::signed_block_ptr& lib, const chain::block_id_type& block_id,
                              const chain::controller& chain);

private:
   // matches trx_match_type of system contract
   enum trx_match_type : uint8_t {
      only = 0,  // trx has only one action and it matches
      first = 1, // trx first action matches
      any = 2    // trx has any action that matches
   };
   // matches trx_prio of system contract
   struct trx_prio {
      short priority = 0;
      chain::name receiver{};
      chain::name action_name{};
      uint8_t match_type = only;
   };
   friend struct fc::reflector<trx_match_type>;
   friend struct fc::reflector<trx_prio>;

   using trx_priority_map_t = boost::container::flat_multimap<chain::name, trx_prio>;
   using trx_priority_map_ptr = std::shared_ptr<const trx_priority_map_t>;

private:
   /// Immutable map snapshot, atomically replaced on refresh.
   producer_plugin_detail::atomic_shared_ptr<const trx_priority_map_t> _trx_priority_map{};
   chain::block_timestamp_type _last_trx_priority_update{}; // only accessed on main thread

private:
   void load_trx_priority_map(const chain::controller& control, trx_priority_map_t& m);
};

} // namespace sysio

FC_REFLECT_ENUM(sysio::trx_priority_db::trx_match_type, (only)(first)(any))
FC_REFLECT(sysio::trx_priority_db::trx_prio, (priority)(receiver)(action_name)(match_type))
