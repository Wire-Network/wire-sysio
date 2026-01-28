// SPDX-License-Identifier: MIT
#include <fc/network/solana/solana_system_programs.hpp>

#include <cstring>

#include <fc/crypto/sha256.hpp>
#include <openssl/bn.h>
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
   // Solana checks if the point is on the Ed25519 curve (can be decompressed).
   // This is different from libsodium's crypto_core_ed25519_is_valid_point which
   // ALSO checks if the point is in the prime-order subgroup (torsion-free).
   //
   // For PDA derivation, we only need to check if the point is on the curve,
   // not if it's in the main subgroup. A point that is on the curve but in a
   // small subgroup (torsion point) should still be rejected as a valid PDA
   // because it IS on the curve.
   //
   // We implement the same check as Solana's curve25519_dalek:
   // Try to decompress the compressed Edwards Y point.
   //
   // Ed25519 curve: -x² + y² = 1 + d*x²*y² where d = -121665/121666
   // Compressed format: 32 bytes where first 255 bits = Y, last bit = sign of X
   //
   // To check if on curve:
   // 1. Extract Y (clear the sign bit)
   // 2. Compute u = y² - 1
   // 3. Compute v = d*y² + 1
   // 4. Compute x² = u/v = u * v^(p-2) mod p
   // 5. Check if x² is a quadratic residue (has a square root)
   //
   // We use the Legendre symbol: x² is a QR iff (x²)^((p-1)/2) ≡ 1 (mod p)

   // Field prime p = 2^255 - 19
   // d = -121665/121666 mod p
   // We need to work in the finite field F_p

   // For simplicity and correctness, we'll use a different approach:
   // Use libsodium's ge25519_frombytes which does the decompression check.
   // This is an internal function but we can access it via the public API.

   // Actually, we can use crypto_scalarmult_ed25519_noclamp to test.
   // If we multiply the point by scalar 1, it should work iff the point is on the curve.
   // But this might not work for points not in the main subgroup.

   // The safest approach: implement the curve check directly using big integer math.
   // But for now, let's use a workaround: try to use the point in an operation.

   // Alternative: crypto_core_ed25519_sub with identity should work
   // Actually, let's just implement using OpenSSL BIGNUM for the curve check.

   // For now, let's use a simple heuristic that matches Solana's behavior:
   // Try decompression by computing x² and checking if it's a quadratic residue.

   // We'll use libsodium's internal API via a workaround.
   // crypto_core_ed25519_add with the identity point (all zeros) should work
   // if the point is on the curve.

   // Actually, crypto_core_ed25519_from_hash takes arbitrary 64 bytes and maps
   // them to a valid curve point. We can't use that.

   // Let me implement the proper check using OpenSSL BN.

   // Copy the compressed point bytes
   std::array<uint8_t, 32> compressed = address.data;

   // Extract Y (clear the sign bit which is the MSB of the last byte)
   // The sign bit is used during full decompression but not needed for curve check
   compressed[31] &= 0x7F;

   // Field prime p = 2^255 - 19
   // d = -121665/121666 mod p
   // d_bytes (little-endian) = a3785913ca4deb75abd841414d0a700098e879777940c78c73fe6f2bee6c0352

   // Convert Y to BIGNUM (little-endian to big-endian for OpenSSL)
   BIGNUM* y = BN_new();
   BIGNUM* y_sq = BN_new();
   BIGNUM* u = BN_new();
   BIGNUM* v = BN_new();
   BIGNUM* p = BN_new();
   BIGNUM* d = BN_new();
   BIGNUM* x_sq = BN_new();
   BIGNUM* exp = BN_new();
   BIGNUM* legendre = BN_new();
   BIGNUM* one = BN_new();
   BIGNUM* neg_one = BN_new();
   BN_CTX* ctx = BN_CTX_new();

   // Set p = 2^255 - 19
   BN_one(p);
   BN_lshift(p, p, 255);
   BN_sub_word(p, 19);

   // Convert Y from little-endian to BIGNUM
   // OpenSSL expects big-endian, so we need to reverse
   std::array<uint8_t, 32> y_be;
   for (size_t i = 0; i < 32; i++) {
      y_be[i] = compressed[31 - i];
   }
   BN_bin2bn(y_be.data(), 32, y);

   // Check if Y >= p (invalid)
   if (BN_cmp(y, p) >= 0) {
      BN_free(y);
      BN_free(y_sq);
      BN_free(u);
      BN_free(v);
      BN_free(p);
      BN_free(d);
      BN_free(x_sq);
      BN_free(exp);
      BN_free(legendre);
      BN_free(one);
      BN_free(neg_one);
      BN_CTX_free(ctx);
      return false;  // Y out of range means not on curve
   }

   // Compute y² mod p
   BN_mod_sqr(y_sq, y, p, ctx);

   // u = y² - 1 mod p
   BN_one(one);
   BN_mod_sub(u, y_sq, one, p, ctx);

   // d = -121665/121666 mod p
   // = 37095705934669439343138083508754565189542113879843219016388785533085940283555
   // In big-endian hex: 52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3
   const uint8_t d_be[] = {
      0x52, 0x03, 0x6c, 0xee, 0x2b, 0x6f, 0xfe, 0x73, 0x8c, 0xc7, 0x40, 0x79, 0x77, 0x79, 0xe8, 0x98,
      0x00, 0x70, 0x0a, 0x4d, 0x41, 0x41, 0xd8, 0xab, 0x75, 0xeb, 0x4d, 0xca, 0x13, 0x59, 0x78, 0xa3};
   BN_bin2bn(d_be, 32, d);

   // v = d*y² + 1 mod p
   BN_mod_mul(v, d, y_sq, p, ctx);
   BN_mod_add(v, v, one, p, ctx);

   // x² = u * v^(-1) mod p = u * v^(p-2) mod p (Fermat's little theorem)
   BN_copy(exp, p);
   BN_sub_word(exp, 2);
   BN_mod_exp(x_sq, v, exp, p, ctx);
   BN_mod_mul(x_sq, x_sq, u, p, ctx);

   // Check if x² is a quadratic residue using Legendre symbol
   // Legendre symbol = x²^((p-1)/2) mod p
   // If result is 0 or 1, it's a QR (on curve). If result is p-1, it's not.
   BN_copy(exp, p);
   BN_sub_word(exp, 1);
   BN_rshift1(exp, exp);  // (p-1)/2
   BN_mod_exp(legendre, x_sq, exp, p, ctx);

   // neg_one = p - 1
   BN_copy(neg_one, p);
   BN_sub_word(neg_one, 1);

   // If Legendre symbol is -1 (p-1), x² is not a QR, point is not on curve
   bool on_curve = (BN_cmp(legendre, neg_one) != 0);

   BN_free(y);
   BN_free(y_sq);
   BN_free(u);
   BN_free(v);
   BN_free(p);
   BN_free(d);
   BN_free(x_sq);
   BN_free(exp);
   BN_free(legendre);
   BN_free(one);
   BN_free(neg_one);
   BN_CTX_free(ctx);

   return on_curve;
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
