#pragma once
/**
 * @file depot_native_token.hpp
 * @brief Resolve a depot-native token code to the custody contract that holds it and its symbol, and
 *        move such a token by inline transfer.
 *
 * Shared by every depot contract that moves depot-native tokens on behalf of a registry code
 * (`sysio.opreg` for operator collateral, `sysio.bond` for underwriting bonds), so the lookup and
 * the transfer exist once.
 */

#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/name.hpp>
#include <sysio/slug_name.hpp>
#include <sysio/symbol.hpp>
#include <sysio.liq/sysio.liq.hpp>
#include <sysio.opp.common/wire_asset.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace sysio::opp::custody {

/// Where a depot-native token lives: the contract whose `transfer` moves it and the symbol its
/// amounts are denominated in.
struct depot_native_token {
   name   contract;   ///< Custody contract whose `transfer` moves the token: `sysio.token` or `sysio.liq`.
   symbol sym;        ///< Symbol amounts of the token are denominated in on that contract.
};

/// Resolve a depot-native `token_code` to its custody contract and symbol -- the ONE lookup every
/// transfer into or out of a depot contract's custody goes through.
///
/// Two custody contracts qualify:
///   * `token_account` (`sysio.token`) for WIRE (`opp::wire::token_code`), denominated in
///     `opp::wire::asset_symbol`;
///   * `liq_account` (`sysio.liq`) for a shadow LIQ symbol, found through the `stat` table's `bytoken`
///     index on the liq token's REGISTRY code (LIQETH, LIQSOL, ...), denominated in that row's supply
///     symbol. The registry code, not the symbol code, is what names the token everywhere else.
///
/// Every depot-native row is keyed `(opp::wire::chain_code, token_code)`: the chain names where the
/// token is custodied, and for a shadow symbol that is the depot, not the outpost whose liq it
/// mirrors.
///
/// @param liq_account    account of the `sysio.liq` contract
/// @param token_account  account of the `sysio.token` contract
/// @param token_code     registry code of the token
/// @return the custody contract and symbol, or `std::nullopt` for any other code
inline std::optional<depot_native_token>
resolve_depot_native_token(name liq_account, name token_account, sysio::slug_name token_code) {
   if (token_code == opp::wire::token_code) {
      return depot_native_token{.contract = token_account, .sym = opp::wire::asset_symbol};
   }
   const auto stat = liq::find_stat_by_token(liq_account, token_code);
   if (!stat) return std::nullopt;
   return depot_native_token{.contract = liq_account, .sym = stat->supply.symbol};
}

/// Action every custody contract (`sysio.token`, `sysio.liq`) exposes to move a balance.
inline constexpr name TRANSFER_ACTION = "transfer"_n;

/// Permission an inline depot-native transfer is sent under: the sender's own `active`.
inline constexpr name TRANSFER_PERMISSION = "active"_n;

/// Move `amount` base units of `token` from `from` to `to` by an inline `transfer` on its custody
/// contract, under `from@TRANSFER_PERMISSION`. The sending contract must be privileged (as
/// `sysio.opreg` and `sysio.bond` are), or `from` must be the sending contract itself with its own
/// `sysio.code` permission. The caller has checked `amount` fits an `asset`.
///
/// Queue it after the caller's rows are committed: the custody contract notifies both parties, so a
/// notification handler that re-enters the caller observes the committed state.
inline void transfer_depot_native(name from, name to, const depot_native_token& token, uint64_t amount,
                                  std::string_view memo) {
   action(permission_level{from, TRANSFER_PERMISSION}, token.contract, TRANSFER_ACTION,
          std::make_tuple(from, to, asset(static_cast<int64_t>(amount), token.sym), std::string(memo)))
      .send();
}

/// Pull `amount` base units of `token` from `from` into `self`'s custody: the one inbound transfer a
/// depot contract sends under the signer's authority (`sysio.opreg::deposit`, `sysio.bond`'s
/// request, bounty, accept and hold).
inline void pull_depot_native(name self, name from, const depot_native_token& token, uint64_t amount,
                              std::string_view memo) {
   transfer_depot_native(from, self, token, amount, memo);
}

} // namespace sysio::opp::custody
