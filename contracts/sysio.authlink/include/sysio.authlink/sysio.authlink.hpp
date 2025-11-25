#pragma once

#include <stdlib.h>
#include <string>
#include <vector>

#include <compression/compression.hpp>
#include <base58/base58.hpp>

#include <keccak256/keccak256.hpp>
#include <sysio/name.hpp>
#include <sysio/singleton.hpp>
#include <sysio/sysio.hpp>
#include <sysio/system.hpp>
#include <sysio/crypto.hpp>
#include <sysio/datastream.hpp>
#include <sysio/serialize.hpp>
#include <sysio/crypto.hpp>

// ! Remove before deployment
#include <sysio/print.hpp>


namespace sysiosystem {
  class system_contract;
}

namespace sysio {
  class [[sysio::contract("sysio.authlink")]] authlink : public contract {
    public:
    using contract::contract;
    using bytes = std::vector<char>;

    /**
     * @brief Using the signarture and provided parameters, this action will create a link between the WIRE account name and the external chain address. Pub keys / Addresses are 1:1 mapped.
     *
     * @param chainName The name of the external chain, based on 'wnsmanager' contract's 'chains' table. REQUIREMENT: 'chain_name' to be active in 'wnsmanger'
     * @param username  The WIRE account name of the user which the address is being linked to.
     * @param sig       A valid 'chainName' signature converted to Wire's standard.
     * @param pubKey    The external chain's public key in Wire format.
     * @param nonce     A nonce, timestamp in ms. Will reject if the nonce is more than 10 minutes old.
     * @return [[sysio::action]] void
     */
    [[sysio::action]] void createlink(const sysio::name &chainName, const sysio::name &username, const sysio::signature &sig, const sysio::public_key &pubKey, const uint64_t nonce);

    /**
     * @brief Called on successful link process, used to adjust the parameters "sysio" contract will receive when notified.
     *
     * @param user          The account name of the user which will be granted 'permission'.
     * @param permission    "auth.ext"
     * @param pub_key       Public key associated with external chain. PUB_XX_ format.
     * @return [[sysio::action]] void
     */
    [[sysio::action]] void onlinkauth(const name &accountName, const name &permission, const sysio::public_key &pubKey);

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

    private:
    // ----- Tables -----

    /**
     * @brief The links table stores the mapping between the WIRE account name and their external chain identity. SCOPE: Default
     */
    TABLE links_s {
      uint64_t key;
      name username;          // Wire account name of the user
      name chain;             // The external chain
      public_key pub_key;     // External chain's Public key in Pub_XX_ format.
      std::string address;    // External chain's address in string format.

      uint64_t primary_key() const { return key; }
      uint128_t by_namechain() const { return (uint128_t(username.value) << 64) | chain.value; }
      uint64_t by_name() const { return username.value; }
      checksum256 by_pub_key() const {
        return pubkey_to_checksum256(pub_key);
      }
      checksum256 by_address() const {
        return sha256(address.c_str(), address.size());
      }
      uint64_t by_chain() const { return chain.value; }
    };

    using links_t = multi_index<"links"_n, links_s,
        indexed_by<"bynamechain"_n, const_mem_fun<links_s, uint128_t, &links_s::by_namechain>>,
        indexed_by<"byname"_n, const_mem_fun<links_s, uint64_t, &links_s::by_name>>,
        indexed_by<"bypubkey"_n, const_mem_fun<links_s, checksum256, &links_s::by_pub_key>>,
        indexed_by<"byaddress"_n, const_mem_fun<links_s, checksum256, &links_s::by_address>>,
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
     * @return checksum256 (sha256 ) of the given parameter.
     */
    // checksum256 get_checksum(const std::string &pub_key);
    // checksum256 get_checksum(const std::array<char, 33> &compressed_key);
    // checksum256 get_checksum(const std::vector<unsigned char> &address);
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