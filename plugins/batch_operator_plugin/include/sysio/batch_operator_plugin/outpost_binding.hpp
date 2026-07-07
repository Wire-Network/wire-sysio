#pragma once
/**
 * @file outpost_binding.hpp
 * @brief Per-chain-code remote OPP contract bindings supplied via the
 *        `--batch-outpost` config option.
 */

#include <string>
#include <utility>

#include <fc/exception/exception.hpp>
#include <fc/slug_name.hpp>
#include <fc/string.hpp>

namespace sysio::batch_operator_detail {

/// Repeatable config option binding one active `sysio.chains` row (by its
/// chain code) to the exact remote OPP contract identity this operator
/// relays it through. See `parse_outpost_binding` for the spec format.
inline constexpr auto BATCH_OUTPOST_OPTION = "batch-outpost";

/// One `--batch-outpost` binding: the exact remote OPP contract identity
/// for a single active `sysio.chains` row, keyed by the row's chain code.
/// Which fields a row requires is enforced in `build_opp_jobs`, where the
/// row's on-chain `ChainKind` is known:
///  * EVM — `opp_addr` = OPP contract, `opp_inbound_addr` = OPPInbound
///    contract, both required (0x-hex).
///  * SVM — `opp_addr` = the outpost program id (base58); the single
///    program serves both directions, so no inbound address.
struct outpost_binding {
   std::string opp_addr;
   std::string opp_inbound_addr;
};

/// Parse one `--batch-outpost` spec — `<CHAIN_CODE>,<opp_addr>[,<opp_inbound_addr>]`
/// — into (packed chain code, binding). Comma-separated like the
/// `--outpost-ethereum-client` spec. Throws on a malformed spec so a
/// misconfigured node refuses to start rather than relaying an outpost
/// with a wrong or partial remote identity.
inline std::pair<uint64_t, outpost_binding> parse_outpost_binding(const std::string& spec) {
   auto parts = fc::split(spec, ',');
   FC_ASSERT(parts.size() == 2 || parts.size() == 3,
             "Invalid {} spec '{}': expected <CHAIN_CODE>,<opp_addr>[,<opp_inbound_addr>]",
             BATCH_OUTPOST_OPTION, spec);
   FC_ASSERT(!parts[0].empty() && !parts[1].empty() && (parts.size() == 2 || !parts[2].empty()),
             "Invalid {} spec '{}': empty field", BATCH_OUTPOST_OPTION, spec);
   const fc::slug_name code{parts[0]};   // throws unless [A-Z0-9_], <= 8 chars
   outpost_binding binding;
   binding.opp_addr = parts[1];
   if (parts.size() == 3)
      binding.opp_inbound_addr = parts[2];
   return {code.value, std::move(binding)};
}

} // namespace sysio::batch_operator_detail
