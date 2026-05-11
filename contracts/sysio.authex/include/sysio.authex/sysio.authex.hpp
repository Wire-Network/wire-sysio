#pragma once

#include <stdlib.h>
#include <string>
#include <vector>

#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/name.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/sysio.hpp>
#include <sysio/system.hpp>
#include <sysio/crypto.hpp>
#include <sysio/crypto_ext.hpp>
#include <sysio/hex.hpp>
#include <sysio/chain_conversions.hpp>
#include <sysio/datastream.hpp>
#include <sysio/serialize.hpp>

namespace sysio {

  constexpr uint128_t to_namechain_key(const name& name, const fc::crypto::chain_kind_t kind) {
    return (static_cast<uint128_t>(name.value) << 64) | static_cast<uint64_t>(kind);
  }

  /**
   * @brief Bitcoin Base58 alphabet — visually unambiguous (no '0', 'O', 'I', 'l').
   *
   * Header-scoped `inline constexpr` so multiple TUs can share a single definition
   * without ODR conflicts. Used by `base58_encode` below.
   */
  inline constexpr char base58_alphabet[] =
      "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

  /**
   * @brief Encode a byte array as a Base58 string using the Bitcoin alphabet.
   *
   * Base58 provides a compact, human-readable textual representation of binary
   * data. It avoids visually ambiguous characters, making it suitable for
   * financial identifiers, cryptocurrency addresses, and similar contexts.
   *
   * Defined `inline` in the header so cross-contract callers (e.g. sysio.msgch
   * resolving an authex link by pubkey) can call it without linking against
   * sysio.authex.cpp's WASM.
   *
   * @param bytes    Pointer to the byte array to encode.
   * @param data_len Length of the byte array.
   * @return Base58 string.
   */
  inline std::string base58_encode(const unsigned char* bytes, uint32_t data_len) {
     uint32_t leading_zeros = 0;
     while (leading_zeros < data_len && bytes[leading_zeros] == 0)
        ++leading_zeros;

     uint32_t max_len = data_len * 138 / 100 + 2;
     std::vector<uint8_t> b58(max_len, 0);

     for (uint32_t i = leading_zeros; i < data_len; ++i) {
        uint32_t carry = bytes[i];
        for (int32_t j = static_cast<int32_t>(max_len) - 1; j >= 0; --j) {
           carry += 256u * b58[j];
           b58[j] = static_cast<uint8_t>(carry % 58);
           carry /= 58;
        }
     }

     uint32_t start = 0;
     while (start < max_len && b58[start] == 0)
        ++start;

     std::string result;
     result.reserve(leading_zeros + (max_len - start));
     result.append(leading_zeros, '1');
     for (uint32_t i = start; i < max_len; ++i)
        result += base58_alphabet[b58[i]];

     return result;
  }

  /**
   * @brief Render a public_key into its canonical "PUB_<curve>_<encoding>" string.
   *
   * Mirrors eosjs-ecc's spelling so that off-chain signers, the authex contract,
   * and any cross-contract caller (e.g. sysio.msgch's bypubkey lookup) all hash
   * the same byte sequence into the secondary-index key.
   *
   * Supported variants:
   *   - EM (variant index 3, secp256k1 compressed, 33 bytes) → "PUB_EM_<hex>"
   *   - ED (variant index 4, Ed25519, 32 bytes)              → "PUB_ED_<base58>"
   *
   * Other variants (K1, R1, WebAuthn, BLS) are rejected with a check failure —
   * authex's links table only ever stores EM and ED keys; if a caller hands in
   * anything else, that's a bug the caller must fix.
   *
   * Defined `inline` in the header so sister contracts can compute the same
   * string without linking against sysio.authex.cpp's WASM.
   *
   * @param pk The public_key variant.
   * @return Canonical string spelling.
   */
  inline std::string pubkey_to_string(const sysio::public_key& pk) {
     switch (pk.index()) {
     case fc::crypto::key_type_em: { // PUB_EM_<hex(compressed_33)>
        auto raw = std::get<3>(pk);
        return "PUB_EM_" + sysio::to_hex(reinterpret_cast<const char*>(raw.data()),
                                         raw.size());
     }
     case 4: { // PUB_ED_<base58(raw_32)> — no checksum, matches fc
        auto raw = std::get<4>(pk);
        std::array<uint8_t, 32> key_bytes;
        for (size_t i = 0; i < 32; ++i)
           key_bytes[i] = static_cast<uint8_t>(raw[i]);
        return "PUB_ED_" + base58_encode(key_bytes.data(), key_bytes.size());
     }
     default:
        sysio::check(false, "pubkey_to_string only supports EM (3) and ED (4)");
        return {};
     }
  }

  /**
   * @brief SHA-256 of the canonical pubkey string — secondary-index key for
   * `sysio.authex::links`'s `bypubkey` index.
   *
   * Use this to map a raw chain pubkey (carried inbound from an outpost via
   * `OperatorAction.op_address`) back to a WIRE account name without paying
   * the cost of iterating the links table.
   *
   * @param pk The public_key variant.
   * @return SHA-256 of pubkey_to_string(pk).
   */
  inline checksum256 pubkey_to_checksum256(const sysio::public_key& pk) {
     std::string pk_str = pubkey_to_string(pk);
     return sha256(pk_str.c_str(), pk_str.size());
  }

  class [[sysio::contract("sysio.authex")]] authex : public contract {
    public:
    using contract::contract;
    using bytes = std::vector<char>;

    /**
     * @brief Using the signature and provided parameters, this action will create a link between the WIRE account name and the external chain address. Pub keys / Addresses are 1:1 mapped.
     *
     * @param chain_kind The chain identifier from fc::crypto::chain_kind_t (e.g. chain_kind_ethereum, chain_kind_solana, chain_kind_sui).
     * @param account   The WIRE account name of the user which the address is being linked to.
     * @param sig        A valid signature for the target chain converted to Wire's standard.
     * @param pub_key     The external chain's public key in Wire format.
     * @param nonce      A nonce, timestamp in ms. Will reject if the nonce is more than 10 minutes old.
     * @return [[sysio::action]] void
     */
    [[sysio::action]] void createlink(
        const fc::crypto::chain_kind_t chain_kind,
        const sysio::name &account,
        const sysio::signature &sig,
        const sysio::public_key &pub_key,
        const uint64_t nonce);

    /**
     * @brief Listens for notifications from sysio contract when 'auth.ext' permission is removed from a users account. Upon receiving a notify (from sysio's 'deleteauth'), the link table is updated to reflect the change.
     *
     * @param account       the account name of the user which had the permission removed.
     * @param permission    the permission which was removed.
     */
    [[sysio::on_notify("sysio::deleteauth")]]
    void onmanualrmv(const name& account, const name& permission);

    // ! For testing only, remove before MAINNET deployment.
    [[sysio::action]] void clearlinks();

    // ----- Tables (public so sister contracts can read via cross-contract kv::table reads) -----

    struct links_key {
      uint64_t key;
      SYSLIB_SERIALIZE(links_key, (key))
    };

    /**
     * @brief The links table stores the mapping between the WIRE account name and their external chain identity. SCOPE: Default
     */
    struct [[sysio::table("links")]] links_s {
      uint64_t key;
      name username;                       // Wire account name of the user
      fc::crypto::chain_kind_t chain_kind; // The external chain identifier
      public_key pub_key;                  // External chain's Public key in PUB_XX_ format.

      uint128_t by_namechain() const { return to_namechain_key(username, chain_kind); }
      uint64_t by_name() const { return username.value; }
      checksum256 by_pub_key() const {
        return pubkey_to_checksum256(pub_key);
      }
      uint64_t by_chain() const { return static_cast<uint64_t>(chain_kind); }

      SYSLIB_SERIALIZE(links_s, (key)(username)(chain_kind)(pub_key))
    };

    using links_t = kv::table<"links"_n, links_key, links_s,
        kv::index<"bynamechain"_n, const_mem_fun<links_s, uint128_t, &links_s::by_namechain>>,
        kv::index<"byname"_n, const_mem_fun<links_s, uint64_t, &links_s::by_name>>,
        kv::index<"bypubkey"_n, const_mem_fun<links_s, checksum256, &links_s::by_pub_key>>,
        kv::index<"bychain"_n, const_mem_fun<links_s, uint64_t, &links_s::by_chain>>
    >;

    private:
    // ----- Helper methods -----

    /**
     * @brief Given the compressed key and type of key, returns the checksum. Mimic's eosjs-ecc's ripemd160.
     *
     * @param data The compressed key.
     * @param extra The curve identifier, e.g. K1, R1, EM.
     * @return std::array<uint8_t, 4> The checksum of the given compressed key.
     */
    static std::array<uint8_t, 4> digestSuffixRipemd160(const std::array<char, 33> &data, const std::string &extra);
  };
}
