#pragma once
#include <sysio/chain/types.hpp>

namespace sysio {
   class contract_action_match {
   public:
      using name = sysio::chain::name;
      enum class match_type {
         prefix,
         exact,
         suffix,
         any      // to be used only for actions, where the contract's actions are all tracked
      };
      using matcher = std::function<bool(const name&)>;

      contract_action_match(const name& root_name, const name& contract_match_name, match_type type);

      void add_action(const name& action_name, match_type type);

      bool is_contract_match(const name& contract_name) const;

      bool is_action_match(const name& action_name) const;

      const name root_name;

      private:
      matcher contract_matcher;
      std::vector<matcher> action_matchers;
   };
} // namespace sysio