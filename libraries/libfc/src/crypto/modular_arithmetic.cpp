#include <fc/crypto/modular_arithmetic.hpp>

#include <gmp.h>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstddef>

namespace fc {

namespace {

/// Powers needing at most this much work, counted as exponent bits times (modulus limbs + 1) squared, are computed by
/// one GMP call, which takes about a tenth of a millisecond.
constexpr uint64_t single_call_work_limit = uint64_t{1} << 16;

/// Exponent bits per window: a table of 16 powers of the base leaves one multiplication per four exponent bits. Every
/// squaring and multiplication is its own GMP call followed by a yield, as one takes a few hundred microseconds at
/// 8 KiB operands.
constexpr unsigned window_bits = 4;
static_assert(CHAR_BIT % window_bits == 0, "a window must not straddle two exponent bytes");

constexpr unsigned windows_per_byte = CHAR_BIT / window_bits;
constexpr unsigned window_mask      = (1u << window_bits) - 1;
constexpr size_t   window_values    = size_t{1} << window_bits;

/// mpz_import / mpz_export arguments for a string of single bytes.
constexpr int    most_significant_first  = 1;
constexpr int    least_significant_first = -1;
constexpr size_t byte_word_size          = 1;
constexpr int    native_endian           = 0;
constexpr size_t no_nail_bits            = 0;

/// Owns one GMP integer, so that an exception thrown by a yield releases it.
class gmp_integer {
public:
   gmp_integer() { mpz_init(value); }
   ~gmp_integer() { mpz_clear(value); }
   gmp_integer(const gmp_integer&)            = delete;
   gmp_integer& operator=(const gmp_integer&) = delete;

   mpz_ptr    get() { return value; }
   mpz_srcptr get() const { return value; }

private:
   mpz_t value;
};

/// Set `out` to the unsigned big-endian integer in `data`; an empty span is zero.
void import_big_endian(gmp_integer& out, std::span<const char> data) {
   if (!data.empty())
      mpz_import(out.get(), data.size(), most_significant_first, byte_word_size, native_endian, no_nail_bits,
                 data.data());
}

/// The bytes of a big-endian integer from its first nonzero byte on.
std::span<const char> without_leading_zeros(std::span<const char> value) {
   const auto first = std::find_if(value.begin(), value.end(), [](char c) { return c != 0; });
   return value.subspan(static_cast<size_t>(first - value.begin()));
}

/// Bit length of a big-endian integer that has no leading zero byte.
uint64_t bit_length(std::span<const char> significant) {
   if (significant.empty())
      return 0;
   const uint64_t leading_zero_bits = std::countl_zero(static_cast<unsigned char>(significant.front()));
   return uint64_t{significant.size()} * CHAR_BIT - leading_zero_bits;
}

/// Whether one GMP call computes a power of this size within `single_call_work_limit`.
bool fits_single_call(uint64_t exponent_bits, size_t modulus_limbs) {
   const uint64_t limb_factor = uint64_t{modulus_limbs} + 1;
   return exponent_bits <= single_call_work_limit / limb_factor / limb_factor;
}

/// Set `r` to `a * b mod m`.
void mul_mod(mpz_ptr r, mpz_srcptr a, mpz_srcptr b, mpz_srcptr m) {
   mpz_mul(r, a, b);
   mpz_mod(r, r, m);
}

/**
 * Set `result` to `base ^ exponent mod modulus` by Horner's rule in base 2^window_bits over the exponent, most
 * significant window first: r = r^(2^window_bits) * base^window mod modulus. `yield` runs after every modular
 * multiplication.
 *
 * @param modulus Nonzero.
 */
void stepped_powm(mpz_ptr result, std::span<const char> base, std::span<const char> exponent, mpz_srcptr modulus,
                  const yield_function_t& yield) {
   // powers[d] = base^d mod modulus, filled up to the largest window value met so far.
   std::array<gmp_integer, window_values> powers;
   mpz_set_ui(powers[0].get(), 1);
   mpz_mod(powers[0].get(), powers[0].get(), modulus);
   import_big_endian(powers[1], base);
   mpz_mod(powers[1].get(), powers[1].get(), modulus);
   yield();
   size_t filled = 2;

   mpz_set(result, powers[0].get());
   for (const char byte : exponent) {
      for (unsigned window = 0; window < windows_per_byte; ++window) {
         for (unsigned bit = 0; bit < window_bits; ++bit) {
            mul_mod(result, result, result, modulus);
            yield();
         }
         const unsigned shift = CHAR_BIT - window_bits * (window + 1);
         const unsigned value = (static_cast<unsigned char>(byte) >> shift) & window_mask;
         if (value == 0)
            continue;
         for (; filled <= value; ++filled) {
            mul_mod(powers[filled].get(), powers[filled - 1].get(), powers[1].get(), modulus);
            yield();
         }
         mul_mod(result, result, powers[value].get(), modulus);
         yield();
      }
   }
}

} // namespace

std::variant<modular_arithmetic_error, bytes> modexp(std::span<const char> base, std::span<const char> exponent,
                                                     std::span<const char> modulus, const yield_function_t& yield) {
   if (modulus.empty())
      return modular_arithmetic_error::modulus_len_zero;

   bytes output(modulus.size(), '\0');

   gmp_integer mod;
   import_big_endian(mod, modulus);
   if (mpz_sgn(mod.get()) == 0)
      return output;

   const std::span<const char> significant_exponent = without_leading_zeros(exponent);
   gmp_integer                 result;
   if (fits_single_call(bit_length(significant_exponent), mpz_size(mod.get()))) {
      gmp_integer b;
      gmp_integer e;
      import_big_endian(b, base);
      import_big_endian(e, significant_exponent);
      mpz_powm(result.get(), b.get(), e.get(), mod.get());
   } else {
      stepped_powm(result.get(), base, significant_exponent, mod.get(), yield);
   }

   // export as little-endian, then convert to big-endian
   mpz_export(output.data(), nullptr, least_significant_first, byte_word_size, native_endian, no_nail_bits,
              result.get());
   std::reverse(output.begin(), output.end());

   return output;
}

} // namespace fc
