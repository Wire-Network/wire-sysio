#pragma once

#include <cstdint>
#include <tuple>
#include <vector>

#include <sysio/chain/name.hpp>
#include <sysio/opp/debugging/debugging.pb.h>

namespace sysio::opp::debugging {

/**
 * @brief Tuple emitted on the batch-operator debug-envelope signal whenever
 *        an OPP envelope moves in either direction.
 *
 * Fields: (epoch_index, endpoint_type, batch_op_name, envelope_data).
 *
 * Owned by `batch_operator_plugin` because the signal that carries it is the
 * plugin's own `debug_envelope_signal`. `depot_ops` and `outpost_opp_job` —
 * both also in this plugin — reference the type; `external_debugging_plugin`
 * consumes it transitively via the signal accessor.
 */
using DebugEnvelopeEvent = std::tuple<uint64_t,
                                      DebugOutpostEndpointsType,
                                      ::sysio::chain::name,
                                      std::vector<char>>;

} // namespace sysio::opp::debugging
