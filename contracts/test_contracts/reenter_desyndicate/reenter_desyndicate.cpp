#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/contract.hpp>
#include <sysio/singleton.hpp>

#include <string>
#include <tuple>

namespace {
/// The real custody transfer whose holder notification exercises WIRE-441.
constexpr sysio::name synd_account{"sysio.synd"};
constexpr sysio::name desyndicate_action{"desyndicate"};
constexpr sysio::name active_permission{"active"};
constexpr sysio::name armed_table{"armed"};
} // namespace

/// Re-enter desyndication once from the holder's outgoing LIQ transfer notification.
class [[sysio::contract]] reenter_desyndicate : public sysio::contract {
public:
   using contract::contract;

   /// Amount and fee cap of the one nested request; removing the row prevents further recursion.
   struct [[sysio::table("armed")]] armed_request {
      sysio::asset quantity;
      uint64_t max_fee;
   };
   using armed_t = sysio::singleton<armed_table, armed_request>;

   /// Arm a single callback before starting the outer desyndication directly on sysio.synd.
   [[sysio::action]] void arm(sysio::asset quantity, uint64_t max_fee) {
      require_auth(get_self());
      armed_t(get_self(), get_self().value).set(armed_request{quantity, max_fee}, get_self());
   }

   /// A genuine outgoing custody transfer triggers the self-authorized nested request exactly once.
   [[sysio::on_notify("sysio.liq::transfer")]]
   void on_transfer(sysio::name from, sysio::name to, sysio::asset quantity, std::string memo) {
      if (from != get_self() || to != synd_account) return;
      armed_t armed(get_self(), get_self().value);
      if (!armed.exists()) return;
      const auto request = armed.get();
      armed.remove();
      sysio::action(sysio::permission_level{get_self(), active_permission}, synd_account, desyndicate_action,
                    std::make_tuple(get_self(), request.quantity, request.max_fee)).send();
   }
};
