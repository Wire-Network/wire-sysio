#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/check.hpp>
#include <sysio/contract.hpp>
#include <sysio/name.hpp>

#include <string>

/// Transfer-notify blocking regression helper.
///
/// Deployed onto an account that a system path pays. `sysio.token::transfer` calls
/// `require_recipient(to)`, and `apply_context::exec` dispatches notified receivers in a bare loop
/// with no exception isolation -- so the assert below aborts the ENTIRE transaction containing the
/// transfer, every parent inline action included.
///
/// While the system payout paths still pushed transfers, that made this contract a chain-halting
/// weapon. Parked on a producer, a batch operator, an operator facing termination, or a
/// swap-from-WIRE refund recipient, it aborted
/// `sysio.epoch::advance` and stopped epoch advancement, emissions accrual and outbound envelope
/// construction for everyone. The swap-from-WIRE case was permanent: the drain's `q.erase()` rolled
/// back with the aborted transaction, so the offending queue row survived and re-blocked every
/// later epoch.
///
/// Those payout paths now credit a claimable balance and transfer nothing, so no notify handler
/// runs on them at all. This contract can therefore only block its own `claimpay` / `claimwire` /
/// `claimremit` -- which is the property these tests assert.
///
/// A handler that burns CPU instead of asserting is the same vector with the same fix (the handler
/// never executes on a credit path), so the assert form covers both shapes. Asserting is the
/// sharper test: it fails loudly and deterministically instead of depending on a CPU deadline.
class [[sysio::contract]] block_transfer : public sysio::contract {
public:
   using contract::contract;

   /// Reject every INCOMING transfer.
   ///
   /// Outgoing transfers (`from == get_self()`) are deliberately ignored so the account can still
   /// fund escrows and be drained during test teardown -- otherwise this contract would block the
   /// very setup steps (e.g. `swapfromwire`'s escrow leg) needed to reach the path under test.
   [[sysio::on_notify("sysio.token::transfer")]]
   void on_transfer(sysio::name from, sysio::name to, sysio::asset quantity, std::string memo) {
      if (to != get_self()) return;
      sysio::check(false, "block_transfer: rejecting incoming transfer");
   }
};
