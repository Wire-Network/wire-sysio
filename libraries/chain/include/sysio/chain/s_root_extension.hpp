#pragma once
#include <sysio/chain/types.hpp>
#include <string>

namespace sysio { namespace chain {

struct s_header {
    name               contract_name;
    checksum256_type   previous_s_id;
    checksum256_type   current_s_id;
    checksum256_type   current_s_root;

    s_header() = default;
    s_header(const name& contract, const checksum256_type& prev, const checksum256_type& curr, const checksum256_type& root)
    : contract_name(contract), previous_s_id(prev), current_s_id(curr), current_s_root(root) {}

    // Copy constructor (default)
    s_header(const s_header& other) = default;

    // Copy assignment operator
    s_header& operator=(const s_header& other) {
        if (this != &other) {
            contract_name = other.contract_name;
            previous_s_id = other.previous_s_id;
            current_s_id = other.current_s_id;
            current_s_root = other.current_s_root;
        }
        return *this;
    }

    friend bool operator == (const s_header& a, const s_header& b) {
        return std::tie(a.contract_name, a.previous_s_id, a.current_s_id, a.current_s_root) ==
               std::tie(b.contract_name, b.previous_s_id, b.current_s_id, b.current_s_root);
    }

    std::string to_string() const {
        return "\n\tContract Name:  " + contract_name.to_string() +
               "\n\tPrevious S-ID:  " + previous_s_id.str() +
               "\n\tCurrent S-ID:   " + current_s_id.str() +
               "\n\tCurrent S-Root: " + current_s_root.str();
    }
};

struct s_root_extension {
    static constexpr uint16_t extension_id() { return 2; } // Unique ID for the extension
    static constexpr bool enforce_unique() { return false; } // Allow each block to have more than one such extension

    s_header s_header_data;

    s_root_extension() = default;
    explicit s_root_extension(const s_header& header)
    : s_header_data(header) {}

    friend bool operator == (const s_root_extension& a, const s_root_extension& b) {
        return a.s_header_data == b.s_header_data;
    }
};

}} // namespace sysio::chain

FC_REFLECT(sysio::chain::s_header,
           (contract_name)
           (previous_s_id)
           (current_s_id)
           (current_s_root))

FC_REFLECT(sysio::chain::s_root_extension,
           (s_header_data))