// SPDX-License-Identifier: MIT
#include <fc/network/solana/solana_system_programs.hpp>

#include <cstring>

#include <fc/crypto/sha256.hpp>
#include <sodium.h>

namespace fc::network::solana::system {

//=============================================================================
// System Program Instruction Indices
//=============================================================================

namespace {
// System program instruction indices
constexpr uint32_t CREATE_ACCOUNT = 0;
constexpr uint32_t ASSIGN = 1;
constexpr uint32_t TRANSFER = 2;
constexpr uint32_t CREATE_ACCOUNT_WITH_SEED = 3;
constexpr uint32_t ADVANCE_NONCE_ACCOUNT = 4;
constexpr uint32_t WITHDRAW_NONCE_ACCOUNT = 5;
constexpr uint32_t INITIALIZE_NONCE_ACCOUNT = 6;
constexpr uint32_t AUTHORIZE_NONCE_ACCOUNT = 7;
constexpr uint32_t ALLOCATE = 8;
constexpr uint32_t TRANSFER_WITH_SEED = 11;
constexpr uint32_t UPGRADE_NONCE_ACCOUNT = 12;

// Compute budget instruction indices
constexpr uint8_t COMPUTE_BUDGET_REQUEST_HEAP_FRAME = 1;
constexpr uint8_t COMPUTE_BUDGET_SET_COMPUTE_UNIT_LIMIT = 2;
constexpr uint8_t COMPUTE_BUDGET_SET_COMPUTE_UNIT_PRICE = 3;
constexpr uint8_t COMPUTE_BUDGET_SET_LOADED_ACCOUNTS_DATA_SIZE_LIMIT = 4;

// PDA marker string
const std::string PDA_MARKER = "ProgramDerivedAddress";

}  // namespace

//=============================================================================
// System Program Instructions Implementation
//=============================================================================

namespace instructions {

instruction create_account(const pubkey& from, const pubkey& new_account, uint64_t lamports,
                           uint64_t space, const pubkey& owner) {
   borsh::encoder enc;
   enc.write_u32(CREATE_ACCOUNT);
   enc.write_u64(lamports);
   enc.write_u64(space);
   enc.write_pubkey(owner);

   return instruction(
      program_ids::SYSTEM_PROGRAM,
      {account_meta::signer(from, true),    // Fee payer and signer
       account_meta::signer(new_account, true)},  // New account, must be signer
      enc.finish());
}

instruction transfer(const pubkey& from, const pubkey& to, uint64_t lamports) {
   borsh::encoder enc;
   enc.write_u32(TRANSFER);
   enc.write_u64(lamports);

   return instruction(program_ids::SYSTEM_PROGRAM,
                      {account_meta::signer(from, true), account_meta::writable(to, false)},
                      enc.finish());
}

instruction assign(const pubkey& account, const pubkey& owner) {
   borsh::encoder enc;
   enc.write_u32(ASSIGN);
   enc.write_pubkey(owner);

   return instruction(program_ids::SYSTEM_PROGRAM, {account_meta::signer(account, true)}, enc.finish());
}

instruction allocate(const pubkey& account, uint64_t space) {
   borsh::encoder enc;
   enc.write_u32(ALLOCATE);
   enc.write_u64(space);

   return instruction(program_ids::SYSTEM_PROGRAM, {account_meta::signer(account, true)}, enc.finish());
}

instruction create_account_with_seed(const pubkey& from, const pubkey& to, const pubkey& base,
                                     const std::string& seed, uint64_t lamports, uint64_t space,
                                     const pubkey& owner) {
   borsh::encoder enc;
   enc.write_u32(CREATE_ACCOUNT_WITH_SEED);
   enc.write_pubkey(base);
   enc.write_string(seed);
   enc.write_u64(lamports);
   enc.write_u64(space);
   enc.write_pubkey(owner);

   std::vector<account_meta> accounts = {account_meta::signer(from, true), account_meta::writable(to, false)};

   // If base is different from from, it needs to be a signer too
   if (base != from) {
      accounts.push_back(account_meta::signer(base, false));
   }

   return instruction(program_ids::SYSTEM_PROGRAM, std::move(accounts), enc.finish());
}

instruction transfer_with_seed(const pubkey& from, const pubkey& from_base, const std::string& seed,
                               const pubkey& from_owner, const pubkey& to, uint64_t lamports) {
   borsh::encoder enc;
   enc.write_u32(TRANSFER_WITH_SEED);
   enc.write_u64(lamports);
   enc.write_string(seed);
   enc.write_pubkey(from_owner);

   return instruction(
      program_ids::SYSTEM_PROGRAM,
      {account_meta::writable(from, false), account_meta::signer(from_base, false), account_meta::writable(to, false)},
      enc.finish());
}

instruction advance_nonce(const pubkey& nonce_account, const pubkey& authority) {
   borsh::encoder enc;
   enc.write_u32(ADVANCE_NONCE_ACCOUNT);

   return instruction(
      program_ids::SYSTEM_PROGRAM,
      {account_meta::writable(nonce_account, false), account_meta::readonly(sysvars::RECENT_BLOCKHASHES, false),
       account_meta::signer(authority, false)},
      enc.finish());
}

instruction withdraw_nonce(const pubkey& nonce_account, const pubkey& authority, const pubkey& to,
                           uint64_t lamports) {
   borsh::encoder enc;
   enc.write_u32(WITHDRAW_NONCE_ACCOUNT);
   enc.write_u64(lamports);

   return instruction(program_ids::SYSTEM_PROGRAM,
                      {account_meta::writable(nonce_account, false), account_meta::writable(to, false),
                       account_meta::readonly(sysvars::RECENT_BLOCKHASHES, false),
                       account_meta::readonly(sysvars::RENT, false), account_meta::signer(authority, false)},
                      enc.finish());
}

instruction initialize_nonce(const pubkey& nonce_account, const pubkey& authority) {
   borsh::encoder enc;
   enc.write_u32(INITIALIZE_NONCE_ACCOUNT);
   enc.write_pubkey(authority);

   return instruction(program_ids::SYSTEM_PROGRAM,
                      {account_meta::writable(nonce_account, false),
                       account_meta::readonly(sysvars::RECENT_BLOCKHASHES, false),
                       account_meta::readonly(sysvars::RENT, false)},
                      enc.finish());
}

instruction authorize_nonce(const pubkey& nonce_account, const pubkey& authority, const pubkey& new_authority) {
   borsh::encoder enc;
   enc.write_u32(AUTHORIZE_NONCE_ACCOUNT);
   enc.write_pubkey(new_authority);

   return instruction(program_ids::SYSTEM_PROGRAM,
                      {account_meta::writable(nonce_account, false), account_meta::signer(authority, false)},
                      enc.finish());
}

instruction upgrade_nonce_account(const pubkey& nonce_account) {
   borsh::encoder enc;
   enc.write_u32(UPGRADE_NONCE_ACCOUNT);

   return instruction(program_ids::SYSTEM_PROGRAM, {account_meta::writable(nonce_account, false)}, enc.finish());
}

}  // namespace instructions

//=============================================================================
// Compute Budget Instructions Implementation
//=============================================================================

namespace compute_budget {

instruction set_compute_unit_limit(uint32_t units) {
   borsh::encoder enc;
   enc.write_u8(COMPUTE_BUDGET_SET_COMPUTE_UNIT_LIMIT);
   enc.write_u32(units);

   return instruction(program_ids::COMPUTE_BUDGET_PROGRAM, {}, enc.finish());
}

instruction set_compute_unit_price(uint64_t micro_lamports) {
   borsh::encoder enc;
   enc.write_u8(COMPUTE_BUDGET_SET_COMPUTE_UNIT_PRICE);
   enc.write_u64(micro_lamports);

   return instruction(program_ids::COMPUTE_BUDGET_PROGRAM, {}, enc.finish());
}

instruction request_heap_frame(uint32_t bytes) {
   borsh::encoder enc;
   enc.write_u8(COMPUTE_BUDGET_REQUEST_HEAP_FRAME);
   enc.write_u32(bytes);

   return instruction(program_ids::COMPUTE_BUDGET_PROGRAM, {}, enc.finish());
}

instruction set_loaded_accounts_data_size_limit(uint32_t bytes) {
   borsh::encoder enc;
   enc.write_u8(COMPUTE_BUDGET_SET_LOADED_ACCOUNTS_DATA_SIZE_LIMIT);
   enc.write_u32(bytes);

   return instruction(program_ids::COMPUTE_BUDGET_PROGRAM, {}, enc.finish());
}

}  // namespace compute_budget

//=============================================================================
// PDA Utilities Implementation
//=============================================================================

bool is_on_curve(const pubkey& address) {
   // Use libsodium to check if the point is on the ed25519 curve
   // crypto_core_ed25519_is_valid_point returns 1 if valid, 0 otherwise
   return crypto_core_ed25519_is_valid_point(address.data.data()) == 1;
}

pubkey create_program_address(const std::vector<std::vector<uint8_t>>& seeds, const pubkey& program_id) {
   // Compute: sha256(seeds || program_id || "ProgramDerivedAddress")
   fc::sha256::encoder enc;

   for (const auto& seed : seeds) {
      FC_ASSERT(seed.size() <= 32, "Seed exceeds maximum length of 32 bytes");
      enc.write(reinterpret_cast<const char*>(seed.data()), seed.size());
   }

   enc.write(reinterpret_cast<const char*>(program_id.data.data()), pubkey::SIZE);
   enc.write(PDA_MARKER.data(), PDA_MARKER.size());

   fc::sha256 hash = enc.result();

   pubkey result;
   std::memcpy(result.data.data(), hash.data(), pubkey::SIZE);

   // The derived address must NOT be on the ed25519 curve
   FC_ASSERT(!is_on_curve(result), "Derived address is on the ed25519 curve - invalid PDA");

   return result;
}

std::pair<pubkey, uint8_t> find_program_address(const std::vector<std::vector<uint8_t>>& seeds,
                                                 const pubkey& program_id) {
   // Try bump seeds from 255 down to 0
   for (int bump = 255; bump >= 0; --bump) {
      std::vector<std::vector<uint8_t>> seeds_with_bump = seeds;
      seeds_with_bump.push_back({static_cast<uint8_t>(bump)});

      try {
         pubkey address = create_program_address(seeds_with_bump, program_id);
         return {address, static_cast<uint8_t>(bump)};
      } catch (...) {
         // This bump seed didn't work, try the next one
         continue;
      }
   }

   FC_THROW("Unable to find valid bump seed for PDA");
}

pubkey get_associated_token_address(const pubkey& owner, const pubkey& mint, const pubkey& token_program) {
   std::vector<std::vector<uint8_t>> seeds = {
      std::vector<uint8_t>(owner.data.begin(), owner.data.end()),
      std::vector<uint8_t>(token_program.data.begin(), token_program.data.end()),
      std::vector<uint8_t>(mint.data.begin(), mint.data.end())};

   auto [address, bump] = find_program_address(seeds, program_ids::ASSOCIATED_TOKEN_PROGRAM);
   return address;
}

pubkey create_with_seed(const pubkey& base, const std::string& seed, const pubkey& program_id) {
   // Compute: sha256(base || seed || program_id)
   FC_ASSERT(seed.size() <= 32, "Seed exceeds maximum length of 32 characters");

   fc::sha256::encoder enc;
   enc.write(reinterpret_cast<const char*>(base.data.data()), pubkey::SIZE);
   enc.write(seed.data(), seed.size());
   enc.write(reinterpret_cast<const char*>(program_id.data.data()), pubkey::SIZE);

   fc::sha256 hash = enc.result();

   pubkey result;
   std::memcpy(result.data.data(), hash.data(), pubkey::SIZE);
   return result;
}

}  // namespace fc::network::solana::system
