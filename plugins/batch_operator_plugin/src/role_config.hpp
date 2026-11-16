#pragma once
/**
 * @file role_config.hpp
 * @brief Parsing of the batch-operator plugin's role options and the choice and check of the key a role signs with.
 *        Free functions, so the plugin's tests drive them without a node.
 */

#include "exposure_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <exception>
#include <string_view>

#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>
#include <fc/slug_name.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
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

/// Parse exposure policies by symbol code: an asset caps exposure, unlimited_exposure explicitly removes the cap,
/// and an absent symbol disables new underwriting. Throws on malformed input, nonpositive caps, duplicate symbols,
/// or a symbol named by both options. Unlimited symbols contain one to seven uppercase letters, without precision.
inline std::map<chain::symbol_code, exposure_policy>
parse_exposure_policies(const std::vector<std::string>& specs, const std::vector<std::string>& unlimited_specs) {
   std::map<chain::symbol_code, exposure_policy> policies;
   for (const std::string& spec : specs) {
      const chain::asset cap = chain::asset::from_string(spec);
      FC_ASSERT(cap.get_amount() > 0, "exposure cap {} must be positive", spec);
      FC_ASSERT(policies.emplace(cap.get_symbol().to_symbol_code(), cap).second,
                "exposure cap names {} more than once", cap.get_symbol().name());
   }
   for (const std::string& spec : unlimited_specs) {
      FC_ASSERT(!spec.empty() && spec.size() <= chain::max_symbol_name_length && chain::symbol::valid_name(spec),
                "unlimited exposure symbol '{}' must contain one to seven uppercase letters", spec);
      const chain::symbol_code code = chain::symbol(0, spec).to_symbol_code();
      const auto found = policies.find(code);
      FC_ASSERT(found == policies.end() || !std::holds_alternative<chain::asset>(found->second),
                "batch-underwriter-max-exposure and batch-underwriter-unlimited-exposure both name {}", spec);
      const bool inserted = policies.emplace(code, unlimited_exposure{}).second;
      FC_ASSERT(inserted, "unlimited exposure names {} more than once", spec);
   }
   return policies;
}

/// Resolve configured symbol policies to registry token codes. Reject finite caps with a mismatched precision;
/// unlimited policies are independent of precision. Report mismatches and unknown shadows through the callbacks.
template <typename PrecisionMismatch, typename UnknownShadow>
std::map<fc::slug_name, exposure_policy> resolve_exposure_policies(
   const std::map<chain::symbol_code, exposure_policy>& configured,
   const std::map<fc::slug_name, chain::symbol>& shadows,
   PrecisionMismatch&& precision_mismatch, UnknownShadow&& unknown_shadow) {
   std::map<fc::slug_name, exposure_policy> out;
   std::set<chain::symbol_code> named;
   for (const auto& [token_code, symbol] : shadows) {
      const auto policy = configured.find(symbol.to_symbol_code());
      if (policy == configured.end()) continue;
      named.insert(policy->first);
      if (const auto* cap = std::get_if<chain::asset>(&policy->second); cap && cap->get_symbol() != symbol) {
         precision_mismatch(*cap, symbol);
         continue;
      }
      out.emplace(token_code, policy->second);
   }
   for (const auto& [code, policy] : configured) {
      if (named.contains(code)) continue;
      if (const auto* cap = std::get_if<chain::asset>(&policy)) {
         unknown_shadow("batch-underwriter-max-exposure", cap->to_string());
      } else if (std::holds_alternative<unlimited_exposure>(policy)) {
         unknown_shadow("batch-underwriter-unlimited-exposure", code.name());
      }
   }
   return out;
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

/// Whether `provider`'s signature over `probe` recovers to its own key, which is how the chain checks a transaction
/// signature. Logs why not. A transient signing failure, such as AWS KMS briefly unavailable, says nothing about the
/// key, so the signer passes unchecked with a warning.
inline bool signs_for_its_key(std::string_view role, const fc::crypto::signature_provider_t& provider,
                              const fc::sha256& probe) {
   try {
      if (fc::crypto::public_key::recover(provider.sign(probe), probe) == provider.public_key) return true;
      elog("batch_operator: {}'s signature provider {} makes signatures that do not recover to its key", role,
           provider.key_name);
   } catch (const chain::signing_transient_exception& e) {
      wlog("batch_operator: {}'s signature provider {} could not sign its startup probe, accepting it unchecked: {}",
           role, provider.key_name, e.top_message());
      return true;
   } catch (const fc::exception& e) {
      elog("batch_operator: {}'s signature provider {} cannot sign: {}", role, provider.key_name, e.top_message());
   } catch (const std::exception& e) {
      elog("batch_operator: {}'s signature provider {} cannot sign: {}", role, provider.key_name, e.what());
   }
   return false;
}

} // namespace sysio::batch_operator_detail
