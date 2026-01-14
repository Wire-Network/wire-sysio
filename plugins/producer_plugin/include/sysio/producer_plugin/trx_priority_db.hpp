#pragma once

#include <sysio/chain/block.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/name.hpp>

namespace sysio {

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
   void on_irreversible_block(const chain::signed_block_ptr& lib, const chain::block_id_type& block_id, const chain::controller& chain);

private:
   // matches trx_match_type of system contract
   enum trx_match_type : uint8_t {
      only = 0,    // trx has only one action and it matches
      first = 1,   // trx first action matches
      any = 2      // trx has any action that matches
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
private:

   std::atomic<std::shared_ptr<trx_priority_map_t>>  _trx_priority_map{nullptr};
   std::atomic<chain::block_timestamp_type>          _last_trx_priority_update{};

private:
   void load_trx_priority_map(const chain::controller& control, trx_priority_map_t& m);
};

} // namespace sysio

FC_REFLECT_ENUM(sysio::trx_priority_db::trx_match_type, (only)(first)(any))
FC_REFLECT(sysio::trx_priority_db::trx_prio, (priority)(receiver)(action_name)(match_type))
