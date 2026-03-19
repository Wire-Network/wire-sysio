#pragma once

#include <string_view>
#include <cstdint>

#include <tuple>
#include <utility>

namespace fc::crypto {

// enum instead of enum class so they can be used in contracts (CDT supports enum but not enum class).
enum chain_kind_t : uint8_t {
   chain_kind_unknown = 0,
   chain_kind_wire = 1,
   chain_kind_ethereum = 2,
   chain_kind_solana = 3,
   chain_kind_sui = 4,
};

enum chain_key_type_t : uint8_t {
   chain_key_type_unknown = 0,
   chain_key_type_wire = 1,
   chain_key_type_wire_bls = 2,
   chain_key_type_ethereum = 3,
   chain_key_type_solana = 4,
   chain_key_type_sui = 5,
};

enum key_type_t : uint8_t {
   key_type_unknown = 0,
   key_type_k1      = 1,
   key_type_r1      = 2,
   key_type_em      = 3,
   key_type_ed      = 4,
   key_type_bls     = 5,
};


// SIZES INCLUDE SCHEME FLAG
constexpr std::size_t key_size_ed25519  = 33;
constexpr std::size_t key_size_sec256k1 = 33;
constexpr std::size_t key_size_sec256r1 = key_size_sec256k1;

constexpr std::size_t chain_address_size_sui      = 32; // base 64 (Blake2b256(pubkey))
constexpr std::size_t chain_address_size_solana   = 32; // Base 58 (pubkey direct)
constexpr std::size_t chain_address_size_ethereum = 20; // base 64 (Keccak256(pubkey[12:]))
constexpr std::size_t chain_address_size_wire     = key_size_sec256k1;

constexpr auto chain_public_key_types = std::tuple{
   std::pair{chain_kind_unknown,  std::pair{chain_key_type_unknown, std::size_t{0}}    },
   std::pair{chain_kind_wire,     std::pair{chain_key_type_wire, key_size_sec256k1}    },
   std::pair{chain_kind_ethereum, std::pair{chain_key_type_ethereum, key_size_sec256k1}},
   std::pair{chain_kind_solana,   std::pair{chain_key_type_solana, key_size_ed25519}   },
   std::pair{chain_kind_sui,      std::pair{chain_key_type_sui, key_size_ed25519}      },
};

// fc::lut<chain_kind_t, std::pair<chain_key_type_t, std::size_t>, chain_kind_count>
constexpr auto chain_address_sizes = std::tuple{
   std::pair{chain_kind_unknown,  std::pair{chain_key_type_unknown, chain_key_type_unknown}  },
   std::pair{chain_kind_wire,     std::pair{chain_key_type_wire, chain_key_type_wire}        },
   std::pair{chain_kind_ethereum, std::pair{chain_key_type_ethereum, chain_key_type_ethereum}},
   std::pair{chain_kind_solana,   std::pair{chain_key_type_solana, chain_key_type_solana}    },
   std::pair{chain_kind_sui,      std::pair{chain_key_type_sui, chain_key_type_sui}          },
};

constexpr auto chain_key_types = std::tuple{
   std::pair{chain_kind_unknown,  std::tuple{chain_key_type_unknown}},
   std::pair{chain_kind_wire,     std::tuple{chain_key_type_wire, chain_key_type_wire_bls}},
   std::pair{chain_kind_ethereum, std::tuple{chain_key_type_ethereum}},
   std::pair{chain_kind_solana,   std::tuple{chain_key_type_solana}},
   std::pair{chain_kind_sui,      std::tuple{chain_key_type_sui}},
};

// Mapping: chain_key_type_t -> tuple of key_type_t
constexpr auto chain_key_type_to_key_types = std::tuple{
   std::pair{chain_key_type_unknown,  std::tuple{key_type_unknown}},
   std::pair{chain_key_type_wire,     std::tuple{key_type_k1, key_type_r1,key_type_em,key_type_ed}},
   std::pair{chain_key_type_wire_bls, std::tuple{key_type_bls}},
   std::pair{chain_key_type_ethereum, std::tuple{key_type_em}},
   std::pair{chain_key_type_solana,   std::tuple{key_type_ed}},
   std::pair{chain_key_type_sui,      std::tuple{key_type_ed}},
};

// Mapping: key_type_t -> tuple of chain_key_type_t
constexpr auto key_type_to_chain_key_types = std::tuple{
   std::pair{key_type_unknown, std::tuple{chain_key_type_unknown}},
   std::pair{key_type_k1,      std::tuple{chain_key_type_wire}},
   std::pair{key_type_r1,      std::tuple{chain_key_type_wire}},
   std::pair{key_type_em,      std::tuple{chain_key_type_ethereum}},
   std::pair{key_type_ed,      std::tuple{chain_key_type_solana, chain_key_type_sui}},
   std::pair{key_type_bls,     std::tuple{chain_key_type_wire_bls}},
};
// Mapping: key_type_t -> string prefix
constexpr std::pair<key_type_t, std::string_view> key_type_prefixes[] = {
   {key_type_unknown, "UN"},
   {key_type_k1,      "K1"},
   {key_type_r1,      "R1"},
   {key_type_em,      "EM"},
   {key_type_ed,      "ED"},
   {key_type_bls,     "BLS"},
};



} // namespace fc::crypto
