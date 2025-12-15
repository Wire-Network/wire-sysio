#pragma once
#include <fc/utility.hpp>
#include <string>
#include <vector>
#include <concepts>

#include <fc/variant.hpp>

namespace fc {
    uint8_t     from_hex(char c);
    std::string to_hex(const char* d, uint32_t s, bool add_prefix = false);

    template<typename T>
    T hex_to_number(const std::string& hex_str) {
       auto clean_hex_str = to_lower(hex_str).starts_with("0x") ? hex_str.substr(2) : hex_str;
       if constexpr (std::is_unsigned_v<T>) {
          return std::stoull(clean_hex_str, nullptr, 16);
       } else {
          return std::stoll(clean_hex_str, nullptr, 16);
       }
    }

    template <typename Container>
       requires std::contiguous_iterator<typename Container::iterator>
    std::string to_hex(const Container& data, bool add_prefix = false) {
       auto hex = data.size() ? to_hex(reinterpret_cast<const char*>(data.data()), data.size()) : "0";
       return add_prefix ? "0x" + hex : hex;
    }

    /**
     *  @return the number of bytes decoded
     */
    size_t from_hex(const std::string& hex_str, char* out_data, size_t out_data_len);

    std::vector<uint8_t> from_hex(const std::string& hex, bool trim_prefix = true);

    std::string trim_hex_prefix(const std::string& hex);


} // namespace fc
