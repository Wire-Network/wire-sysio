#include <sysio/sysio.hpp>
#include <string>
#include <sysio/print.hpp>

namespace wns {

    struct uint256_t {
        uint128_t low;
        uint128_t high;

        // Default constructor
        uint256_t() : high(0), low(0) {}

        // Construct from uint128_t
        uint256_t(uint128_t val) : high(0), low(val) {}
        uint256_t(uint128_t low_val, uint128_t high_val): high(high_val), low(low_val) {}


        // Addition with overflow check
        uint256_t operator+(const uint256_t& other) const {
            uint256_t result;
            result.low = low + other.low;
            result.high = high + other.high;
            if (result.low < low) { // Check for overflow in low part
                result.high++; // Carry to high part
            }
            return result;
        }

        // Subtraction with underflow check
        uint256_t operator-(const uint256_t& other) const {
            uint256_t result;
            result.low = low - other.low;
            result.high = high - other.high;
            if (result.low > low) { // Check for underflow in low part
                result.high--; // Borrow from high part
            }
            return result;
        }

        uint256_t& operator+=(const uint256_t& other) {
            uint128_t new_low = low + other.low;
            high += other.high;
            if( new_low < low) { //Check for overflow
                high++; //Carry the one
            }
            low = new_low;
            return *this;
        }

        uint256_t& operator -=(const uint256_t& other) {
            uint128_t new_low = low - other.low;
            high -= other.high;
            if( new_low > low ) { // Check for underflow
                high--; //Borrow one
            }
            low = new_low;
            return *this;
        }

        // Less than
        bool operator<(const uint256_t& other) const {
            if (high == other.high) {
                return low < other.low;
            }
            return high < other.high;
        }

        // Greater than
        bool operator>(const uint256_t& other) const {
            if (high == other.high) {
                return low > other.low;
            }
            return high > other.high;
        }

        // Equal to
        bool operator==(const uint256_t& other) const {
            return (high == other.high) && (low == other.low);
        }

        // Less than or equal to
        bool operator<=(const uint256_t& other) const {
            return *this < other || *this == other;
        }

        bool operator>=(const uint256_t& other) const {
            return *this > other || *this == other;
        }

        // Convert to hexadecimal string for easier printing and debugging
        std::string to_string() const {
            char buffer[65]; // 64 hex chars + null terminator
            uint64_t high_high = (uint64_t)(high >> 64);
            uint64_t high_low = (uint64_t)(high);
            uint64_t low_high = (uint64_t)(low >> 64);
            uint64_t low_low = (uint64_t)(low);

            snprintf(buffer, sizeof(buffer),
                    "%016llx%016llx%016llx%016llx",
                    high_high, high_low, low_high, low_low);
            return std::string(buffer);
        }
        // Print method for sysio::print compatibility
        void print() const {
            sysio::print(to_string());
        }

        static uint256_t from_string(const std::string& str) {
            uint128_t high = (uint128_t)std::stoull(str.substr(0, 16), nullptr, 16) << 64 |
                            (uint128_t)std::stoull(str.substr(16, 16), nullptr, 16);
            uint128_t low = (uint128_t)std::stoull(str.substr(32, 16), nullptr, 16) << 64 |
                            (uint128_t)std::stoull(str.substr(48, 16), nullptr, 16);

            return uint256_t(low, high);
        }

        // Serialization support
        SYSLIB_SERIALIZE(uint256_t, (low)(high))
    };
}
