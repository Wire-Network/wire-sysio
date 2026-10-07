#pragma once
/**
 * @file role_config.hpp
 * @brief The choice of the key a role of the batch-operator plugin signs with. Pure functions, so the plugin's
 *        tests drive them without a node.
 */

#include <cstddef>
#include <optional>
#include <vector>

namespace sysio::batch_operator_detail {

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
   for (const auto& provider : providers) {
      if (!qualifies(provider)) continue;
      ++out.matches;
      out.chosen = provider;
   }
   if (out.matches != 1) out.chosen.reset();
   return out;
}

} // namespace sysio::batch_operator_detail
