#pragma once

#include <variant>

#include <sysio/chain/asset.hpp>

namespace sysio::batch_operator_detail {

/// No new bonds are permitted, including for a default-created policy.
struct disabled_exposure {};

/// Explicit permission to bond without a cap; balances and verification still apply.
struct unlimited_exposure {};

/// Exposure is disabled by default, capped by an asset, or explicitly unlimited.
using exposure_policy = std::variant<disabled_exposure, chain::asset, unlimited_exposure>;

} // namespace sysio::batch_operator_detail
