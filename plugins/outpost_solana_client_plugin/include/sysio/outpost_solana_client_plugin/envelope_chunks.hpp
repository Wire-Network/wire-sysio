#pragma once

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_system_programs.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sysio::outpost_solana_client_detail {

/// Byte-exact seed shared with wire-solana's EnvelopeChunks account declaration
/// in `programs/liqsol-core/src/states/opp_states.rs`.
inline constexpr std::string_view ENVELOPE_CHUNKS_SEED = "envelope_chunks";
/// Byte-exact seed shared with wire-solana's EpochDeliveries declaration
/// in `programs/liqsol-core/src/states/opp_states.rs`.
inline constexpr std::string_view EPOCH_DELIVERIES_SEED = "epoch_deliveries";
/// Instruction name shared by boot validation and the cleanup binding.
inline constexpr auto CLEANUP_ENVELOPE_CHUNKS_INSTRUCTION = "cleanup_envelope_chunks";
/// Cleanup waits for confirmed status; the default retry envelope remains unchanged.
inline constexpr fc::network::solana::solana_confirm_options ENVELOPE_CHUNK_CLEANUP_CONFIRM_OPTIONS{
   .commitment = fc::network::solana::commitment_t::confirmed};

/// The 4-byte little-endian seed encoding of a WIRE epoch index -- the exact
/// bytes the program's `epoch_index.to_le_bytes()` seed component uses.
inline std::vector<uint8_t> epoch_index_le_seed(uint32_t epoch_index) {
   return {static_cast<uint8_t>(epoch_index & 0xFF),
           static_cast<uint8_t>((epoch_index >> 8) & 0xFF),
           static_cast<uint8_t>((epoch_index >> 16) & 0xFF),
           static_cast<uint8_t>((epoch_index >> 24) & 0xFF)};
}

/// Derive the per-epoch `EpochDeliveries` PDA:
/// seeds `[EPOCH_DELIVERIES_SEED, epoch_index.to_le_bytes()]`.
inline fc::network::solana::solana_public_key
derive_epoch_deliveries_pda(const fc::network::solana::solana_public_key& program_id,
                            uint32_t                                      epoch_index) {
   return fc::network::solana::system::find_program_address(
             {std::vector<uint8_t>(EPOCH_DELIVERIES_SEED.begin(), EPOCH_DELIVERIES_SEED.end()),
              epoch_index_le_seed(epoch_index)},
             program_id)
      .first;
}

/// Derive the per-(epoch, uploader) envelope chunk-buffer PDA:
/// seeds `[ENVELOPE_CHUNKS_SEED, epoch_index.to_le_bytes(), uploader]`. The
/// uploader's pubkey is the third seed so multiple operators in the same
/// group stage their own buffers without contention.
inline fc::network::solana::solana_public_key
derive_envelope_chunks_pda(const fc::network::solana::solana_public_key& program_id,
                           uint32_t                                      epoch_index,
                           const fc::network::solana::solana_public_key& uploader) {
   const auto uploader_bytes = uploader.serialize();
   return fc::network::solana::system::find_program_address(
             {std::vector<uint8_t>(ENVELOPE_CHUNKS_SEED.begin(), ENVELOPE_CHUNKS_SEED.end()),
              epoch_index_le_seed(epoch_index),
              std::vector<uint8_t>(uploader_bytes.begin(), uploader_bytes.end())},
             program_id)
      .first;
}

} // namespace sysio::outpost_solana_client_detail
