// Copyright (c) 2014-2024 The Bitcoin Core developers
// Copyright (c) 2009-2010 Satoshi Nakamoto
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Pure-bytes base58 encode/decode adapted from Bitcoin Core's
// src/base58.cpp. No OpenSSL BIGNUM dependency. The algorithm runs in
// O(n*m) on small fixed-size keys/signatures, which is negligible in
// practice and removes the per-call heap allocations OpenSSL's BIGNUM
// path introduced.

#include <fc/crypto/base58.hpp>
#include <fc/exception/exception.hpp>

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Alphabet used for base58 encoding (no 0OIl to avoid visually similar glyphs).
constexpr const char* k_b58_alphabet =
   "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/// Inline capacity for the working buffer used inside encode/decode.
/// Covers any input up to ~92 bytes without touching the heap; signatures
/// are 65 bytes, public keys 33, private keys 32, so all production callers
/// stay on the stack. Larger inputs fall back to a heap allocation.
constexpr size_t k_b58_inline_capacity = 128;

/// Reverse lookup table: maps an ASCII byte to its base58 digit value, or -1
/// if the byte is not a valid base58 character. Sized at 256 so any unsigned
/// byte indexes safely.
constexpr int8_t k_b58_reverse[256] = {
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1, 0, 1, 2, 3, 4, 5, 6,  7, 8,-1,-1,-1,-1,-1,-1,
   -1, 9,10,11,12,13,14,15, 16,-1,17,18,19,20,21,-1,
   22,23,24,25,26,27,28,29, 30,31,32,-1,-1,-1,-1,-1,
   -1,33,34,35,36,37,38,39, 40,41,42,43,-1,44,45,46,
   47,48,49,50,51,52,53,54, 55,56,57,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
   -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
};

/// Encode the byte range [pbegin, pend) as a base58 string.
///
/// Algorithm: treat the input as a big-endian unsigned integer and
/// repeatedly divide by 58, collecting remainders as base58 digits.
/// The division is implemented in-place on a working buffer that holds
/// the partial quotient in base58. Leading zero bytes in the input are
/// preserved as leading '1' characters in the output.
std::string encode_base58(const unsigned char* pbegin, const unsigned char* pend) {
   int zeroes = 0;
   while (pbegin != pend && *pbegin == 0) {
      ++pbegin;
      ++zeroes;
   }
   // log(256) / log(58) ~= 1.366, round up via *138/100 + 1.
   const int size = static_cast<int>((pend - pbegin) * 138 / 100 + 1);
   boost::container::small_vector<unsigned char, k_b58_inline_capacity> b58(size);
   int length = 0;
   while (pbegin != pend) {
      int carry = *pbegin;
      int i = 0;
      // b58 = b58 * 256 + carry, walking from least to most significant digit.
      for (auto it = b58.rbegin(); (carry != 0 || i < length) && it != b58.rend(); ++it, ++i) {
         carry += 256 * (*it);
         *it = static_cast<unsigned char>(carry % 58);
         carry /= 58;
      }
      assert(carry == 0);
      length = i;
      ++pbegin;
   }
   auto it = b58.begin() + (size - length);
   while (it != b58.end() && *it == 0)
      ++it;
   std::string str;
   str.reserve(static_cast<size_t>(zeroes) + static_cast<size_t>(b58.end() - it));
   str.assign(zeroes, '1');
   while (it != b58.end())
      str += k_b58_alphabet[*(it++)];
   return str;
}

/// Decode a base58-encoded NUL-terminated string into a byte vector.
/// Returns false on invalid input (unknown character, trailing garbage).
///
/// Mirrors encode_base58 in reverse: walks the input characters and
/// builds the big-endian base256 representation by repeatedly
/// multiplying the working buffer by 58 and adding the current digit.
bool decode_base58(const char* psz, std::vector<unsigned char>& vch) {
   vch.clear();
   while (*psz && std::isspace(static_cast<unsigned char>(*psz)))
      ++psz;
   int zeroes = 0;
   while (*psz == '1') {
      ++zeroes;
      ++psz;
   }
   const size_t psz_len = std::strlen(psz);
   // log(58) / log(256) ~= 0.733, round up via *733/1000 + 1.
   const int size = static_cast<int>(psz_len * 733 / 1000 + 1);
   boost::container::small_vector<unsigned char, k_b58_inline_capacity> b256(size);
   int length = 0;
   while (*psz && !std::isspace(static_cast<unsigned char>(*psz))) {
      const int digit = k_b58_reverse[static_cast<unsigned char>(*psz)];
      if (digit < 0)
         return false;
      int carry = digit;
      int i = 0;
      // b256 = b256 * 58 + carry.
      for (auto it = b256.rbegin(); (carry != 0 || i < length) && it != b256.rend(); ++it, ++i) {
         carry += 58 * (*it);
         *it = static_cast<unsigned char>(carry % 256);
         carry /= 256;
      }
      assert(carry == 0);
      length = i;
      ++psz;
   }
   while (std::isspace(static_cast<unsigned char>(*psz)))
      ++psz;
   if (*psz != 0)
      return false;
   auto it = b256.begin() + (size - length);
   vch.reserve(static_cast<size_t>(zeroes) + static_cast<size_t>(b256.end() - it));
   vch.assign(zeroes, 0x00);
   while (it != b256.end())
      vch.push_back(*(it++));
   return true;
}

} // namespace

namespace fc {

std::string to_base58(const char* d, size_t s, const fc::yield_function_t& yield) {
   yield();
   auto str = encode_base58(reinterpret_cast<const unsigned char*>(d),
                            reinterpret_cast<const unsigned char*>(d) + s);
   yield();
   return str;
}

std::string to_base58(const std::vector<char>& d, const fc::yield_function_t& yield) {
   if (d.empty())
      return std::string();
   return to_base58(d.data(), d.size(), yield);
}

std::vector<char> from_base58(const std::string& base58_str) {
   std::vector<unsigned char> out;
   if (!decode_base58(base58_str.c_str(), out)) {
      FC_THROW_EXCEPTION(parse_error_exception, "Unable to decode base58 string {}", base58_str);
   }
   return std::vector<char>(reinterpret_cast<const char*>(out.data()),
                            reinterpret_cast<const char*>(out.data()) + out.size());
}

size_t from_base58(const std::string& base58_str, char* out_data, size_t out_data_len) {
   std::vector<unsigned char> out;
   if (!decode_base58(base58_str.c_str(), out)) {
      FC_THROW_EXCEPTION(parse_error_exception, "Unable to decode base58 string {}", base58_str);
   }
   FC_ASSERT(out.size() <= out_data_len);
   std::memcpy(out_data, out.data(), out.size());
   return out.size();
}

} // namespace fc
