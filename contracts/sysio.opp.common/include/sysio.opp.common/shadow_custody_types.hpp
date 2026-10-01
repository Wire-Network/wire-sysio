#pragma once
/**
 * @file shadow_custody_types.hpp
 * @brief The two row types of the shadow-custody library, on their own so a custodian's contract
 *        header can declare its tables without pulling in the library's functions.
 *
 * A custodian embeds a `position` in each sub-holder row and stores one `yield_pool` per shadow
 * symbol in a table of its own. The functions that operate on them, and the model, obligations and
 * solvency argument that govern them, are in `shadow_custody.hpp`.
 */

#include <sysio/serialize.hpp>

#include <cstdint>

namespace sysio::opp::shadow::custody {

/// One sub-holder's yield state, embedded in the custodian's own row next to the balance it tracks.
struct position {
   uint128_t index_checkpoint = 0;   ///< `sysio.liq`'s index for the symbol at the last settle
   uint64_t  owed_wire        = 0;   ///< WIRE banked by earlier settles, not yet taken
   SYSLIB_SERIALIZE(position, (index_checkpoint)(owed_wire))
};

/// One symbol's solvency pool, stored by the custodian in its own table under its own key.
///
/// Named `yield_pool` rather than a bare `pool`, which elsewhere in this codebase names the swap's
/// liquidity pools.
struct yield_pool {
   uint64_t  received     = 0;   ///< WIRE pulled from `sysio.liq` for the symbol (`pull`)
   uint64_t  credited     = 0;   ///< WIRE taken out of `received` for sub-holders (`take`); never above it
   uint128_t pulled_index = 0;   ///< `sysio.liq`'s index for the symbol at the last recorded pull
   uint64_t  pulled_owed  = 0;   ///< what that pull recorded: the custodian's owed at `pulled_index`
   SYSLIB_SERIALIZE(yield_pool, (received)(credited)(pulled_index)(pulled_owed))
};

} // namespace sysio::opp::shadow::custody
