#pragma once
/**
 * @file treasury.hpp
 * @brief The unreserved WIRE of the `sysio` treasury.
 *
 * The one computation a contract sizes a discretionary treasury draw on: `sysio.kicker`'s gifts and
 * the WIRE side of a yield pool `sysio.liq::regliqpool` seeds after the bootstrap window. Kept apart
 * from emissions.hpp, which stays free of token reads.
 */
#include <sysio.system/emissions.hpp>
#include <sysio.token/sysio.token.hpp>
#include <sysio.opp.common/wire_asset.hpp>

#include <sysio/check.hpp>
#include <sysio/name.hpp>

#include <algorithm>
#include <cstdint>

namespace sysiosystem::treasury {

/// Refusals of malformed treasury state.
namespace errors {
inline constexpr auto negative_pending = "negative pending emissions";
inline constexpr auto invalid_balance  = "invalid treasury WIRE balance";
} // namespace errors

/// WIRE in `system_account`'s balance on `token_account` that nothing has reserved: the balance less
/// the pending emissions, the outstanding pay claims and the remaining emission ceiling. Missing
/// emission state or balance fails closed as 0.
inline uint64_t available(sysio::name system_account, sysio::name token_account) {
   using namespace sysiosystem::emissions;
   const t5state_t treasury(system_account);
   const emitcfg_t emission_config(system_account);
   if (!treasury.exists() || !emission_config.exists()) return 0;
   const auto state = treasury.get();
   sysio::check(state.pending_emission_amount >= 0, errors::negative_pending);
   const auto claims = payclaimtot_t(system_account).get_or_default(pay_claim_total{});
   const sysio::token::accounts balances(token_account, system_account.value);
   const sysio::token::acct_key key{sysio::opp::wire::asset_symbol.code().raw()};
   if (!balances.contains(key)) return 0;
   const auto balance = balances.get(key).balance;
   sysio::check(balance.symbol == sysio::opp::wire::asset_symbol && balance.amount >= 0, errors::invalid_balance);
   const auto emission = emission_config.get();
   // Widen before subtraction so malformed or exhausted state cannot wrap a reserve.
   const auto remaining = std::max<__int128>(0, static_cast<__int128>(emission.t5_distributable) -
                                               emission.t5_floor - state.total_distributed);
   const auto reserved = static_cast<unsigned __int128>(state.pending_emission_amount) + claims.outstanding +
                         static_cast<unsigned __int128>(remaining);
   if (reserved >= static_cast<unsigned __int128>(balance.amount)) return 0;
   return static_cast<uint64_t>(static_cast<unsigned __int128>(balance.amount) - reserved);
}

} // namespace sysiosystem::treasury
