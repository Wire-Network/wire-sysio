// Compression.hpp
#ifndef COMPRESSION_HPP
#define COMPRESSION_HPP

#include <sysio/sysio.hpp>
#include <sysio/print.hpp>
#include <intx/base.hpp>
#include <string>
#include <vector>
// #include <sstream>

namespace compression
{
    inline uint512_t modExp(uint512_t base, uint256_t exponent, uint256_t modulus) {
        uint512_t result = 1;
        base = base % modulus;

        while (exponent > 0) {
            if ((exponent & 1) != 0) {  // Check if exponent is odd
                result = (result * base) % modulus;
            }
            exponent = exponent >> 1;
            base = (base * base) % modulus;
        }

        return result;
    }

    inline std::string decompressPublicKey(const std::string& compressed) {
        std::string prefix = compressed.substr(0, 2);
        uint256_t x = intx::from_string<uint256_t>(compressed.substr(2));

        // secp256k1 curve parameters
        uint256_t p = intx::from_string<uint256_t>(
            "0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f");
        
        uint512_t xSquared = uint512_t(x) * x;
        uint512_t xSquaredMod = xSquared % p;
        uint512_t xCubed = xSquaredMod * x % p;  // You could use xSquared here, but it might be unnecessarily large
        
        uint512_t ySquared = (xCubed + 7) % p;
        uint512_t y = modExp(ySquared, (p + 1) / 4, p);

        std::string x_hex = intx::hex(x);
        std::string y_hex;

        // * 02 prefix = y is even
        // * 03 prefix = y is odd
        if ((prefix == "02" && y % 2 == 0) || (prefix == "03" && y % 2 != 0)) {
            y_hex = intx::hex(y);
        } else {
            y = p - y;
            y_hex = intx::hex(y);
        }

        if(x_hex.length() < 64) {
            x_hex = std::string(64 - x_hex.length(), '0') + x_hex;
        }

        if(y_hex.length() < 64) {
            y_hex = std::string(64 - y_hex.length(), '0') + y_hex;
        }

        return "04" + x_hex + y_hex;
    }
} // namespace compression

#endif // COMPRESSION_HPP
