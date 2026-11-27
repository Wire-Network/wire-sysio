/**
 * @file epoch_stub.cpp
 * @brief Test-only stand-in deployed on `sysio.epoch`. It writes the real per-chain `epochstate` rows, so a
 *        suite can move an outpost chain's epoch index without the operators, emissions and consensus that
 *        `sysio.epoch::advance` requires. Readers of the index -- `sysio::epoch::current_epoch_index` -- see
 *        exactly what the real contract would have written.
 */

#include <sysio.epoch/sysio.epoch.hpp>

/// Writes `sysio.epoch`'s epoch state under the account it is deployed on.
class [[sysio::contract("epoch_stub")]] epoch_stub : public sysio::contract {
public:
   using contract::contract;

   /// Set each outpost in `chain_codes` to current epoch index `index`, keeping every other field of its
   /// epoch state. Auth=the account the stub is deployed on.
   [[sysio::action]] void setindex(std::vector<uint64_t> chain_codes, uint32_t index) {
      require_auth(get_self());
      sysio::epoch::epochstate_t state(get_self());
      for (const uint64_t chain_code : chain_codes) {
         const sysio::epoch::chain_key key{chain_code};
         sysio::epoch::epoch_state     current =
            state.try_get(key).value_or(sysio::epoch::epoch_state{.chain_code = chain_code});
         current.current_epoch_index = index;
         state.set(get_self(), key, current);
      }
   }

   /// Accept the traffic sysio.msgch records for a chain (`sysio.epoch::notetraffic`), as the real contract does, so
   /// `queueout` and `buildenv` run against the stub. The stub's chains never advance, so it records nothing.
   /// Auth=sysio.msgch.
   [[sysio::action]] void notetraffic([[maybe_unused]] sysio::slug_name chain_code,
                                      [[maybe_unused]] sysio::time_point since) {
      require_auth(sysio::epoch::MSGCH_ACCOUNT);
   }
};
