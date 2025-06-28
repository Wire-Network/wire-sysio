#include <sysio/sub_chain_plugin/contract_action_match.hpp>
#include <sysio/chain/exceptions.hpp>

namespace sysio {
   using match_type = contract_action_match::match_type;
   using name = contract_action_match::name;

   contract_action_match::contract_action_match(const name& root_name, const name& contract_match_name, match_type type)
   : root_name(root_name) {
      SYS_ASSERT(root_name != name(), chain::producer_exception, "root_name cannot be empty");
      SYS_ASSERT(contract_match_name != name(), chain::producer_exception, "contract_match_name cannot be empty");
      switch(type) {
         case match_type::prefix:
            contract_matcher = [contract_match_name](const name& n) { return n.prefix() == contract_match_name; };
            break;
         case match_type::exact:
            contract_matcher = [contract_match_name](const name& n) { return n == contract_match_name; };
            break;
         case match_type::suffix:
            SYS_ASSERT(contract_match_name == contract_match_name.suffix(), chain::producer_exception,
                       "contract_match_name must be the desired suffix and should not contain any '.'.",
                       ("contract_match_name", contract_match_name.to_string()));
            contract_matcher = [contract_match_name](const name& n) { return n.suffix() == contract_match_name; };
            break;
         case match_type::any:
            [[fallthrough]];
         default:
            SYS_THROW(chain::producer_exception,
                      "contract_action_match does not support the given type: ${type}",
                      ("type",static_cast<int64_t>(type)));
       }
   }

   void contract_action_match::add_action(const name& action_name, match_type type) {
      SYS_ASSERT(action_name != name() || type == match_type::any, chain::producer_exception,
                 "action_name cannot be empty");
      switch(type) {
         case match_type::prefix:
            action_matchers.push_back([action_name](const name& n) { return n == action_name; });
            break;
         case match_type::exact:
            action_matchers.push_back([action_name](const name& n) { return n == action_name; });
            break;
         case match_type::suffix:
            SYS_ASSERT(action_name == action_name.suffix(), chain::producer_exception,
                       "action_name must be the desired suffix and should not contain any '.'.",
                       ("action_name", action_name.to_string()));
            action_matchers.push_back([action_name](const name& n) { return n.suffix() == action_name; });
            break;
         case match_type::any:
            action_matchers.push_back([action_name](const name& n) { return true; });
            break;
         default:
            SYS_THROW(chain::producer_exception,
                      "contract_action_match::add_action does not support the given type: ${type}",
                      ("type",static_cast<int64_t>(type)));
      }
   }

    bool contract_action_match::is_contract_match(const name& contract_name) const {
        return contract_matcher(contract_name);
    }

    bool contract_action_match::is_action_match(const name& action_name) const {
        for (const auto& matcher : action_matchers) {
            if (matcher(action_name)) {
                return true;
            }
        }
        return false;
    }

} // namespace sysio
