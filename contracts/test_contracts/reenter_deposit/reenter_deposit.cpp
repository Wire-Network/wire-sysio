#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/contract.hpp>
#include <sysio/name.hpp>
#include <sysio/permission.hpp>

#include <string>
#include <tuple>

/// SEC-103 deposit-reentrancy regression helper.
///
/// Deployed onto an `sysio.opreg` operator account. `opreg::deposit` moves the
/// operator's SYS collateral to `sysio.opreg` via `sysio.token::transfer`, which
/// notifies the `from` account — this contract. The handler re-enters
/// `opreg::deposit` once, modelling a malicious operator that tries to land a
/// second credit against the same pre-credit balance and push its collateral row
/// past `asset::max_amount`.
///
/// With `opreg::deposit` performing its cap check atomically inside the same
/// `modify` as the credit (and crediting before the transfer), the re-entrant
/// deposit observes the already-committed balance: its own cap check trips and
/// aborts the whole transaction, so the operator can never exceed the cap.
class [[sysio::contract]] reenter_deposit : public sysio::contract {
public:
   using contract::contract;

   /// On the deposit's outgoing SYS transfer, re-enter `opreg::deposit` with a
   /// `+1` credit. Guards: only react to OUR OWN outgoing transfer
   /// (`from == get_self()`), and only when the moved quantity exceeds the `+1`
   /// re-deposit, so the re-entrant deposit's own transfer cannot recurse.
   [[sysio::on_notify("sysio.token::transfer")]]
   void on_transfer(sysio::name from, sysio::name to, sysio::asset quantity, std::string memo) {
      if (from != get_self()) return;
      if (quantity.amount <= 1) return;
      sysio::action(
         sysio::permission_level{get_self(), sysio::name("active")},
         sysio::name("sysio.opreg"), sysio::name("deposit"),
         std::make_tuple(get_self(), static_cast<uint64_t>(1))
      ).send();
   }
};
