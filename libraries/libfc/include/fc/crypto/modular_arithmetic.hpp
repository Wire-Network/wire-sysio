#pragma once

#include <fc/utility.hpp>

#include <cstdint>
#include <span>
#include <vector>
#include <variant>

namespace fc {
    using bytes = std::vector<char>;

    enum class modular_arithmetic_error : int32_t {
        modulus_len_zero,
    };

    /**
     * Big integer modular exponentiation: (base ^ exponent) mod modulus.
     *
     * The operands are unsigned big-endian integers of any length; an empty operand is zero, and 0^0 is 1. Beyond a
     * small fixed amount of work, the power is computed in steps with a call to `yield` after each.
     *
     * @param base     The base.
     * @param exponent The exponent.
     * @param modulus  The modulus.
     * @param yield    Called after each step; an exception it throws propagates out of modexp.
     * @return The result as a big-endian integer of exactly `modulus.size()` bytes, all zero when the modulus is
     *         zero, or `modulus_len_zero` when `modulus` is empty.
     */
    std::variant<modular_arithmetic_error, bytes> modexp(std::span<const char> base, std::span<const char> exponent,
                                                         std::span<const char> modulus, const yield_function_t& yield);
}
