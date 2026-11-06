#pragma once
/**
 * @file role_config.hpp
 * @brief Parsing of the batch-operator plugin's role options and the choice of the key a role signs with. Pure
 *        functions, so the plugin's tests drive them without a node.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fc/exception/exception.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/name.hpp>

namespace sysio::batch_operator_detail {

/// Parse `account[@permission]`, the permission defaulting to `active`. Throws when either part is empty or not a
/// valid name.
inline chain::permission_level parse_permission_level(const std::string& spec) {
   const size_t            at = spec.find('@');
   chain::permission_level level{chain::name(spec.substr(0, at)), chain::config::active_name};
   if (at != std::string::npos) level.permission = chain::name(spec.substr(at + 1));
   FC_ASSERT(level.actor.good(), "'{}' names no account", spec);
   FC_ASSERT(level.permission.good(), "'{}' names no permission", spec);
   return level;
}

/// Parse the exposure caps, one asset each, keyed by symbol code. Throws on a malformed asset, an amount that is
/// not positive, or a symbol named twice.
inline std::map<chain::symbol_code, chain::asset> parse_exposure_caps(const std::vector<std::string>& specs) {
   std::map<chain::symbol_code, chain::asset> caps;
   for (const std::string& spec : specs) {
      const chain::asset cap = chain::asset::from_string(spec);
      FC_ASSERT(cap.get_amount() > 0, "exposure cap {} must be positive", spec);
      FC_ASSERT(caps.emplace(cap.get_symbol().to_symbol_code(), cap).second,
                "exposure cap names {} more than once", cap.get_symbol().name());
   }
   return caps;
}

/// A depot action a role pushes.
struct pushed_action {
   const char* contract;
   const char* action;
};

/// An action a role pushes only while one of its options is on.
struct optional_action {
   pushed_action action;
   bool          enabled = false;
};

/// The actions a role's permission must be linked for: every action it always pushes, and each optional one only
/// while enabled. An action the role never pushes cannot fail its startup.
inline std::vector<pushed_action> required_actions(std::span<const pushed_action>   always,
                                                   std::span<const optional_action> optional) {
   std::vector<pushed_action> out(always.begin(), always.end());
   for (const optional_action& o : optional) {
      if (o.enabled) out.push_back(o.action);
   }
   return out;
}

/// The `actions` that `linked` refuses: those the role's permission may not declare, which fail the role's startup.
template <typename Linked>
std::vector<pushed_action> unlinked_actions(std::span<const pushed_action> actions, Linked&& linked) {
   std::vector<pushed_action> out;
   for (const pushed_action& a : actions) {
      if (!linked(a)) out.push_back(a);
   }
   return out;
}

/// The provider a role signs with, and how many qualified.
template <typename Provider>
struct signer_choice {
   std::optional<Provider> chosen;    ///< set when exactly one qualified
   std::size_t             matches = 0;
};

/// Choose the one provider `qualifies` accepts. None, or more than one, chooses nothing: with two the choice would
/// fall to provider order.
template <typename Provider, typename Qualifies>
signer_choice<Provider> choose_signer(const std::vector<Provider>& providers, Qualifies&& qualifies) {
   signer_choice<Provider> out;
   for (const Provider& provider : providers) {
      if (!qualifies(provider)) continue;
      ++out.matches;
      out.chosen = provider;
   }
   if (out.matches != 1) out.chosen.reset();
   return out;
}

} // namespace sysio::batch_operator_detail
