#include <fc/crypto/city.hpp>
#include <fc/int128.hpp>
#include <fc/variant.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

namespace fc {
fc::uint128 to_uint128(std::uint64_t hi, uint64_t lo) {
   fc::uint128 value(hi);
   value = (value & std::numeric_limits<uint64_t>::max()) << 64;
   value |= fc::uint128(lo & std::numeric_limits<uint64_t>::max());
   return value;
}

void to_variant(const fc::uint128& var, fc::variant& vo) {
   vo = var.str();
}
void from_variant(const variant& var, fc::uint128& vo) {
   vo = fc::uint128(var.as_string());
}

void to_variant(const fc::int128& var, fc::variant& vo) {
   vo = var.str();
}
void from_variant(const variant& var, fc::int128& vo) {
   vo = fc::int128(var.as_string());
}

} // namespace fc

/*
 * Portions of the above code were adapted from the work of Evan Teran.
 *
 * Copyright (c) 2008
 * Evan Teran
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appears in all copies and that both the
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the same name not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission. We make no representations about the
 * suitability this software for any purpose. It is provided "as is"
 * without express or implied warranty.
 */
