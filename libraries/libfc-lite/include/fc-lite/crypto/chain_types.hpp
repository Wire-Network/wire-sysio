#pragma once
#include <cstdint>
#include <tuple>
#include <utility>
namespace fc::crypto {

enum chain_kind : uint8_t {
   chain_kind_unknown = 0,
   chain_kind_wire = 1,
   chain_kind_ethereum = 2,
   chain_kind_solana = 3,
   chain_kind_sui = 4,
   // chain_kind_count = 5,
};

enum chain_key_type : uint8_t {
   chain_key_type_unknown = 0,
   chain_key_type_wire = 1,
   chain_key_type_ethereum = 2,
   chain_key_type_solana = 3,
   chain_key_type_sui = 4,
   // chain_key_type_count = 5,
};

// SIZES INCLUDE SCHEME FLAG
constexpr std::size_t key_size_ed25519  = 33;
constexpr std::size_t key_size_sec256k1 = 34;
constexpr std::size_t key_size_sec256r1 = key_size_sec256k1;

constexpr std::size_t chain_address_size_sui      = 32; // base 64 (Blake2b256(pubkey))
constexpr std::size_t chain_address_size_solana   = 32; // Base 58 (pubkey direct)
constexpr std::size_t chain_address_size_ethereum = 20; // base 64 (Keccak256(pubkey[12:]))
constexpr std::size_t chain_address_size_wire     = key_size_sec256k1;

constexpr auto chain_public_key_types = std::tuple{
   std::pair{chain_kind_unknown,       std::pair{chain_key_type_unknown, std::size_t{0}}    },
   std::pair{chain_kind_wire,     std::pair{chain_key_type_wire, key_size_sec256k1}    },
   std::pair{chain_kind_ethereum, std::pair{chain_key_type_ethereum, key_size_sec256k1}},
   std::pair{chain_kind_solana,   std::pair{chain_key_type_solana, key_size_ed25519}   },
   std::pair{chain_kind_sui,      std::pair{chain_key_type_sui, key_size_ed25519}      },
};

// fc::lut<chain_kind, std::pair<chain_key_type, std::size_t>, chain_kind_count>
constexpr auto chain_address_sizes = std::tuple{
   std::pair{chain_kind_unknown,       std::pair{chain_key_type_unknown, chain_key_type_unknown}  },
   std::pair{chain_kind_wire,     std::pair{chain_key_type_wire, chain_key_type_wire}        },
   std::pair{chain_kind_ethereum, std::pair{chain_key_type_ethereum, chain_key_type_ethereum}},
   std::pair{chain_kind_solana,   std::pair{chain_key_type_solana, chain_key_type_solana}    },
   std::pair{chain_kind_sui,      std::pair{chain_key_type_sui, chain_key_type_sui}          },
};

}