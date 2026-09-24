#pragma once
/**
 * @file shadow_yield_reference.hpp
 * @brief The shadow yield spec (sysio.opp.common/shadow_yield.hpp), written by
 *        hand and shared by the sysio.swap and sysio.liq suites: a distribution
 *        advances the index by WIRE * SCALE / supply with the remainder carried
 *        into the next one, and a holder is owed its banked WIRE plus
 *        balance * (index - checkpoint) / SCALE, floored. Also the Boost.Test
 *        printer for the 128-bit index fields, which have no stream operator of
 *        their own.
 */

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/test/unit_test.hpp>
#include <fc/int128.hpp>

#include <cstdint>
#include <ostream>

// Boost.Test prints both operands of a failed assertion.
namespace boost::test_tools::tt_detail {
   template<> struct print_log_value<fc::uint128_t> {
      void operator()( std::ostream& os, const fc::uint128_t& v ) { os << fc::to_string( v ); }
   };
}

namespace yield_reference {
   using wide = boost::multiprecision::uint128_t;
   constexpr uint64_t Scale = 1'000'000'000'000;
   // The index and the checkpoints are 128-bit, like the rows they are read from.
   struct distribution { wide index_delta; uint64_t carry; };
   inline distribution distribute( int64_t wire, int64_t supply, uint64_t carry_in ) {
      const wide total = wide(wire) * Scale + carry_in;
      return { wide( total / supply ), uint64_t( total % supply ) };
   }
   inline int64_t owed( int64_t balance, fc::uint128_t index, fc::uint128_t checkpoint, uint64_t banked = 0 ) {
      return int64_t( banked + uint64_t( wide(balance) * (wide(index) - wide(checkpoint)) / Scale ) );
   }
}
