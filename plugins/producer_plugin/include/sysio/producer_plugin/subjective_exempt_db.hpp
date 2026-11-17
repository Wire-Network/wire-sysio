#pragma once

#include <sysio/chain/block.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/chain/types.hpp>

#include <unordered_set>

namespace sysio {

namespace chain {
class controller;
class kv_store;
} // namespace chain

/**
 * Keeps the accounts that chain state exempts from subjective billing current: the sysio.system `subjexempt` list,
 * and the active batch and underwriter operators of sysio.opreg.
 *
 * Reloads them on the first irreversible block after startup and every refresh_interval blocks after it.
 */
class subjective_exempt_db {
public:
   /// Irreversible blocks between reloads, about a minute.
   static constexpr chain::block_num_type refresh_interval = 120;

   /// The accounts the chain state in `store` exempts.
   static std::unordered_set<chain::account_name> load(const chain::kv_store& store);

   /// Reloads the exempt accounts into `chain`'s subjective billing when a reload is due. Call from the main thread.
   void on_irreversible_block(const chain::signed_block_ptr& lib, chain::controller& chain);

private:
   bool _loaded = false; ///< set by the first reload, which does not wait for the interval
};

} // namespace sysio
