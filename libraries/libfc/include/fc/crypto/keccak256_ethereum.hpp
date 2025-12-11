// keccak256.hpp
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>

namespace fc::crypto {

// Keccak-f[1600] parameters for Keccak-256:
// - state: 5x5 lanes of 64 bits => 1600 bits
// - rate r = 1088 bits (136 bytes)
// - capacity c = 512 bits
// - domain separation suffix = 0x01 (Keccak, not SHA3)
// - output length = 256 bits (32 bytes)
class keccak256_ethereum_hasher {
public:
    static constexpr std::size_t hash_size = 32;
    static constexpr std::size_t rate_bytes = 136; // 1088 / 8
    static constexpr std::uint8_t domain_suffix = 0x01;

    keccak256_ethereum_hasher() noexcept {
        reset();
    }

    void reset() noexcept {
        state_.fill(0);
        pos_ = 0;
        finalized_ = false;
    }

    // Absorb arbitrary bytes into the sponge
    void update(std::span<const std::uint8_t> data) noexcept {
        if (finalized_) {
            // Re-use after finalize: reset automatically
            reset();
        }
        for (std::uint8_t byte : data) {
            absorb_byte(byte);
        }
    }

    // Convenience overload for raw byte strings
    void update(std::string_view bytes) noexcept {
        auto ptr = reinterpret_cast<const std::uint8_t*>(bytes.data());
        update(std::span<const std::uint8_t>(ptr, bytes.size()));
    }

    // Finalize and write 32-byte digest into out
    void finalize(std::uint8_t* out) noexcept {
        if (!finalized_) {
            apply_padding_and_permute();
            finalized_ = true;
        }
        squeeze(out, hash_size);
    }

    // Finalize and return std::array
    [[nodiscard]] std::array<std::uint8_t, hash_size> digest() noexcept {
        std::array<std::uint8_t, hash_size> out{};
        finalize(out.data());
        return out;
    }

    // Static one-shot helper
    [[nodiscard]] static std::array<std::uint8_t, hash_size>
    hash(std::span<const std::uint8_t> data) noexcept {
        keccak256_ethereum_hasher h;
        h.update(data);
        return h.digest();
    }

    [[nodiscard]] static std::array<std::uint8_t, hash_size>
    hash(std::string_view bytes) noexcept {
        keccak256_ethereum_hasher h;
        h.update(bytes);
        return h.digest();
    }

private:
    // State is 25 lanes of 64 bits, stored as little-endian words
    std::array<std::uint64_t, 25> state_{};
    std::size_t pos_ = 0;      // number of bytes already absorbed in current block [0, rate_bytes)
    bool finalized_ = false;

    static constexpr std::uint64_t rotl64(std::uint64_t x, unsigned shift) noexcept {
        return (x << shift) | (x >> (64u - shift));
    }

    // Keccak-f[1600] round constants
    static constexpr std::array<std::uint64_t, 24> round_constants{
        0x0000000000000001ULL, 0x0000000000008082ULL,
        0x800000000000808aULL, 0x8000000080008000ULL,
        0x000000000000808bULL, 0x0000000080000001ULL,
        0x8000000080008081ULL, 0x8000000000008009ULL,
        0x000000000000008aULL, 0x0000000000000088ULL,
        0x0000000080008009ULL, 0x000000008000000aULL,
        0x000000008000808bULL, 0x800000000000008bULL,
        0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL,
        0x000000000000800aULL, 0x800000008000000aULL,
        0x8000000080008081ULL, 0x8000000000008080ULL,
        0x0000000080000001ULL, 0x8000000080008008ULL
    };

    // Rotation offsets (Rho step), indexed as x + 5*y
    static constexpr std::array<unsigned, 25> rotation_offsets{
         0u, 36u,  3u, 41u, 18u,
         1u, 44u, 10u, 45u,  2u,
        62u,  6u, 43u, 15u, 61u,
        28u, 55u, 25u, 21u, 56u,
        27u, 20u, 39u,  8u, 14u
    };

    void absorb_byte(std::uint8_t byte) noexcept {
        // Map byte index in current block to lane index and byte position within lane
        std::size_t lane_index = pos_ / 8;
        unsigned   lane_offset = static_cast<unsigned>(pos_ % 8u);

        // Little-endian placement of byte into lane
        state_[lane_index] ^= (static_cast<std::uint64_t>(byte) << (8u * lane_offset));

        ++pos_;
        if (pos_ == rate_bytes) {
            keccak_f1600();
            pos_ = 0;
        }
    }

    void apply_padding_and_permute() noexcept {
        // Domain-separated Keccak padding: suffix 0x01 + final bit 0x80 at end of block
        {
            std::size_t lane_index = pos_ / 8;
            unsigned   lane_offset = static_cast<unsigned>(pos_ % 8u);
            state_[lane_index] ^= (static_cast<std::uint64_t>(domain_suffix) << (8u * lane_offset));
        }

        // XOR 0x80 into last byte of the rate portion
        {
            constexpr std::size_t last_pos   = rate_bytes - 1;       // 135
            constexpr std::size_t lane_index = last_pos / 8;         // 16
            constexpr unsigned    lane_off   = last_pos % 8u;        // 7
            state_[lane_index] ^= (static_cast<std::uint64_t>(0x80u) << (8u * lane_off));
        }

        // Single permutation after final padded block
        keccak_f1600();
        pos_ = 0;
    }

    void squeeze(std::uint8_t* out, std::size_t out_len) noexcept {
        // For Keccak-256, out_len == 32 < rate_bytes, so one permutation is enough.
        // Still implement the generic loop in case you extend it.
        std::size_t out_pos = 0;

        while (out_pos < out_len) {
            std::size_t block_remaining = rate_bytes;
            while (block_remaining > 0 && out_pos < out_len) {
                std::size_t byte_index = rate_bytes - block_remaining;
                std::size_t lane_index = byte_index / 8;
                unsigned    lane_off   = static_cast<unsigned>(byte_index % 8u);
                std::uint8_t byte =
                    static_cast<std::uint8_t>(
                        (state_[lane_index] >> (8u * lane_off)) & 0xFFu
                    );
                out[out_pos++] = byte;
                --block_remaining;
            }
            if (out_pos < out_len) {
                keccak_f1600();
            }
        }
    }

    void keccak_f1600() noexcept {
        std::uint64_t b[25];
        std::uint64_t c[5];
        std::uint64_t d[5];

        for (unsigned round = 0; round < 24; ++round) {
            // Theta step
            for (unsigned x = 0; x < 5; ++x) {
                c[x] = state_[x] ^ state_[x + 5] ^ state_[x + 10] ^ state_[x + 15] ^ state_[x + 20];
            }
            for (unsigned x = 0; x < 5; ++x) {
                d[x] = rotl64(c[(x + 4) % 5] ^ c[(x + 1) % 5], 1);
            }
            for (unsigned x = 0; x < 5; ++x) {
                for (unsigned y = 0; y < 5; ++y) {
                    state_[x + 5 * y] ^= d[x];
                }
            }

            // Rho and Pi steps
            for (unsigned x = 0; x < 5; ++x) {
                for (unsigned y = 0; y < 5; ++y) {
                    unsigned idx   = x + 5 * y;
                    unsigned new_x = (2 * x + 3 * y) % 5;
                    unsigned new_y = y;
                    unsigned dst   = new_x + 5 * new_y;
                    b[dst] = rotl64(state_[idx], rotation_offsets[idx]);
                }
            }

            // Chi step
            for (unsigned y = 0; y < 5; ++y) {
                std::uint64_t row[5];
                for (unsigned x = 0; x < 5; ++x) {
                    row[x] = b[x + 5 * y];
                }
                for (unsigned x = 0; x < 5; ++x) {
                    state_[x + 5 * y] =
                        row[x] ^ ((~row[(x + 1) % 5]) & row[(x + 2) % 5]);
                }
            }

            // Iota step
            state_[0] ^= round_constants[round];
        }
    }
};

// One-shot helpers

[[nodiscard]] inline std::array<std::uint8_t, keccak256_ethereum_hasher::hash_size>
keccak256_ethereum(std::span<const std::uint8_t> data) noexcept {
    return keccak256_ethereum_hasher::hash(data);
}

[[nodiscard]] inline std::array<std::uint8_t, keccak256_ethereum_hasher::hash_size>
keccak256_ethereum(std::string_view bytes) noexcept {
    return keccak256_ethereum_hasher::hash(bytes);
}

} // namespace fc::crypto