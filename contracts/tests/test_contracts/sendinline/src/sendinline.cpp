#include <sysio/action.hpp>
#include <sysio/contract.hpp>
#include <sysio/name.hpp>
#include <sysio/permission.hpp>
#include <vector>

/// `sysio.code` is a virtual permission (there is no private or public
/// key associated with it). Therefore, this test tests how `sysio.msig`
/// contract reacts to a smart contract submitting a proposal and
/// approving/unnapproving itself.
class [[sysio::contract]]
sendinline : public sysio::contract {
public:
   using contract::contract;

   [[sysio::action]]
   void send( sysio::name contract, sysio::name action_name, std::vector<sysio::permission_level> auths, std::vector<char> payload) {
      sysio::action act;
      act.account = contract;
      act.name = action_name;
      act.authorization = auths;
      act.data = std::move(payload);
      act.send();
   }
};
