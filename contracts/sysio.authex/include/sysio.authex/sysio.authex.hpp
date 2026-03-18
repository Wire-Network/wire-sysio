#pragma once

#include <stdlib.h>
#include <string>
#include <vector>

#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/name.hpp>
#include <sysio/singleton.hpp>
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
    void onmanualrmv(const name& account, const fc::crypto::chain_kind_t kind);

    // ! For testing only, remove before MAINNET deployment.
    [[sysio::action]] void clearlinks();

    private:
    // ----- Tables -----

    /**
     * @brief The links table stores the mapping between the WIRE account name and their external chain identity. SCOPE: Default
     */
    TABLE links_s {
      uint64_t key;
      name username;                       // Wire account name of the user
      fc::crypto::chain_kind_t chain_kind; // The external chain identifier
      public_key pub_key;                  // External chain's Public key in PUB_XX_ format.


      uint64_t primary_key() const { return key; }
      uint128_t by_namechain() const { return to_namechain_key(username, chain_kind); }
      uint64_t by_name() const { return username.value; }
      checksum256 by_pub_key() const {
        return pubkey_to_checksum256(pub_key);
      }
      uint64_t by_chain() const { return static_cast<uint64_t>(chain_kind); }
    };

    using links_t = multi_index<"links"_n, links_s,
        indexed_by<"bynamechain"_n, const_mem_fun<links_s, uint128_t, &links_s::by_namechain>>,
        indexed_by<"byname"_n, const_mem_fun<links_s, uint64_t, &links_s::by_name>>,
        indexed_by<"bypubkey"_n, const_mem_fun<links_s, checksum256, &links_s::by_pub_key>>,
        indexed_by<"bychain"_n, const_mem_fun<links_s, uint64_t, &links_s::by_chain>>
    >;
    // ----- Helper methods -----

    /**
     * @brief Given the compressed key and type of key, returns the checksum. Mimic's eosjs-ecc's ripemd160.
     *
     * @param data The compressed key.
     * @param extra The curve identifier, e.g. K1, R1, EM.
     * @return std::array<uint8_t, 4> The checksum of the given compressed key.
     */
    static std::array<uint8_t, 4> digestSuffixRipemd160(const std::array<char, 33> &data, const std::string &extra);

    /**
     * @brief Get the checksum of a given pub_key, compressed_key, or address (contract address).
     *
     * @return string representation of the pubkey, mimicking the format used by eosjs-ecc for different key types (e.g. "PUB_EM_" + hex(compressed_33_bytes) for EM keys).
     */
    static std::string pubkey_to_string(const sysio::public_key& pk);

    /**
     * Pack an EOSIO public_key into raw bytes and return its SHA-256 digest.
     */
    static checksum256 pubkey_to_checksum256( const public_key& pk ) {
      std::string pk_str = pubkey_to_string(pk);

      return sha256( pk_str.c_str(), pk_str.size() );
    }
  };
}
