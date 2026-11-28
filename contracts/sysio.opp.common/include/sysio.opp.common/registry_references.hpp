#pragma once

#include <sysio.chains/sysio.chains.hpp>
#include <sysio.tokens/sysio.tokens.hpp>

namespace sysio::opp::registry {

/// Accounts owning the chain and token registries.
inline constexpr name chains_account = "sysio.chains"_n;
inline constexpr name tokens_account = "sysio.tokens"_n;

/// Diagnostics shared by privileged writers of registry references.
inline constexpr auto missing_chain = ": chain code not registered";
inline constexpr auto missing_token = ": token code not registered";
inline constexpr auto missing_binding = ": (chain_code, token_code) binding not registered";

/// Require parent records to exist without imposing an activation-order constraint.
/// The token writer supplies its own receiver; consumers use the canonical registry.
inline void check_registered_parents(slug_name chain_code, slug_name token_code,
                                     name token_registry, const char* context) {
   chains::chains_t chains_tbl(chains_account);
   tokens::tokens_t tokens_tbl(token_registry);
   check(chains_tbl.contains(chains::chain_key{chain_code}), std::string(context) + missing_chain);
   check(tokens_tbl.contains(tokens::token_key{token_code}), std::string(context) + missing_token);
}

} // namespace sysio::opp::registry
