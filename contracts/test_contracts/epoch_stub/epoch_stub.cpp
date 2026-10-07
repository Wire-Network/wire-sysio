/**
 * @file epoch_stub.cpp
 * @brief Test-only stand-in deployed on `sysio.epoch`. It writes the real `epochstate` singleton, so a
 *        suite can move the depot's epoch index without the operators, emissions and consensus that
 *        `sysio.epoch::advance` requires. Readers of the index -- `sysio::epoch::current_epoch_index`
 *        -- see exactly what the real contract would have written.
 */

#include <sysio.epoch/sysio.epoch.hpp>

/// Writes `sysio.epoch`'s epoch state under the account it is deployed on.
class [[sysio::contract("epoch_stub")]] epoch_stub : public sysio::contract {
public:
   using contract::contract;

   /// Set the current epoch index to `index`, keeping every other field of the epoch state.
   /// Auth=the account the stub is deployed on.
   [[sysio::action]] void setindex(uint32_t index) {
      require_auth(get_self());
      sysio::epoch::epochstate_t  state(get_self());
      sysio::epoch::epoch_state   current = state.get_or_default(sysio::epoch::epoch_state{});
      current.current_epoch_index = index;
      state.set(current, get_self());
   }
};
