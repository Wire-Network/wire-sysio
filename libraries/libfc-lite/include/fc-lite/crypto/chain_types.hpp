#pragma once

#include <cstdint>
#include <tuple>
#include <utility>

namespace fc::crypto {

enum class chain_kind_t : uint8_t {
   unknown = 0,
   wire = 1,
   ethereum = 2,
   solana = 3,
   sui = 4,
};

enum class chain_key_type_t : uint8_t {
   unknown = 0,
   wire = 1,
   wire_bls = 2,
   ethereum = 3,
   solana = 4,
   sui = 5,
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
   std::pair{chain_kind_t::unknown,  std::pair{chain_key_type_t::unknown, std::size_t{0}}    },
   std::pair{chain_kind_t::wire,     std::pair{chain_key_type_t::wire, key_size_sec256k1}    },
   std::pair{chain_kind_t::ethereum, std::pair{chain_key_type_t::ethereum, key_size_sec256k1}},
   std::pair{chain_kind_t::solana,   std::pair{chain_key_type_t::solana, key_size_ed25519}   },
   std::pair{chain_kind_t::sui,      std::pair{chain_key_type_t::sui, key_size_ed25519}      },
};

// fc::lut<chain_kind_t, std::pair<chain_key_type_t, std::size_t>, chain_kind_count>
constexpr auto chain_address_sizes = std::tuple{
   std::pair{chain_kind_t::unknown,  std::pair{chain_key_type_t::unknown, chain_key_type_t::unknown}  },
   std::pair{chain_kind_t::wire,     std::pair{chain_key_type_t::wire, chain_key_type_t::wire}        },
   std::pair{chain_kind_t::ethereum, std::pair{chain_key_type_t::ethereum, chain_key_type_t::ethereum}},
   std::pair{chain_kind_t::solana,   std::pair{chain_key_type_t::solana, chain_key_type_t::solana}    },
   std::pair{chain_kind_t::sui,      std::pair{chain_key_type_t::sui, chain_key_type_t::sui}          },
};

constexpr auto chain_key_types = std::tuple{
   std::pair{chain_kind_t::unknown,  std::tuple{chain_key_type_t::unknown}},
   std::pair{chain_kind_t::wire,     std::tuple{chain_key_type_t::wire, chain_key_type_t::wire_bls}},
   std::pair{chain_kind_t::ethereum, std::tuple{chain_key_type_t::ethereum}},
   std::pair{chain_kind_t::solana,   std::tuple{chain_key_type_t::solana}},
   std::pair{chain_kind_t::sui,      std::tuple{chain_key_type_t::sui}},
};

} // namespace fc::crypto
