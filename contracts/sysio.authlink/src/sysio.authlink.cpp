#include <cstdint>

#include <sysio.system/non_wasm_types.hpp>
#include <sysio.authlink/sysio.authlink.hpp>
#include <sysio/print.hpp>

extern "C" {
    __attribute__((sysio_wasm_import))
    int32_t blake2b_256(
      const char* data_ptr,
      std::uint32_t data_len,
      char*       out_ptr,    uint32_t out_len
    ) noexcept;
}

namespace sysiosystem {
  class system_contract;
}

namespace sysio {
  // ----- PUBLIC ACTIONS -----
  [[sysio::action]] void authlink::createlink(
      const name& chainName,
      const name& username,
      const signature& sig,
      const public_key& pubKey,
      const uint64_t nonce
  ) {
    // Require caller authorization
    require_auth(username);

    // ——— Chain → curve mapping & validation ———
    name curve;
    if (chainName == "ethereum"_n) {
      curve = "ec"_n;
    } else if (chainName == "solana"_n || chainName == "sui"_n) {
      curve = "ed"_n;
    } else {
      check(false, "Invalid chain. See 'wnsmanager' contract for supported chains.");
    }

    // ——— Table & indices ———
    links_t links(get_self(), get_self().value);
    auto by_namechain = links.get_index<"bynamechain"_n>();
    uint128_t name_chain = (uint128_t(username.value) << 64) | chainName.value;
    check(by_namechain.find(name_chain) == by_namechain.end(),
          "Account already has a link for this curve.");

    auto by_pubkey = links.get_index<"bypubkey"_n>();
    auto pub_hash = pubkey_to_checksum256(pubKey);
    check(by_pubkey.find(pub_hash) == by_pubkey.end(),
          "Public key already linked to a different account.");

    // ——— Nonce freshness ———
    constexpr uint64_t TEN_MIN_MS = 10 * 60 * 1000;
    uint64_t now_ms = current_time_point().time_since_epoch().count() / 1000;
    check(nonce <= now_ms && now_ms - nonce <= TEN_MIN_MS,
          "Invalid nonce: must be within the last 10 minutes");

    // ——— Build the message string ———
    static constexpr const char* DIGEST_TAIL = "createlink auth";
    std::string msg = pubkey_to_string(pubKey)
                    + "|" + username.to_string()
                    + "|" + chainName.to_string()
                    + "|" + std::to_string(nonce)
                    + "|" + DIGEST_TAIL;

    // ——— Curve-specific signing & address derivation ———
    if (curve == "ec"_n) {
      // 1) keccak(msg)
      keccak::SHA3_CTX ctx;
      keccak::keccak_init(&ctx);
      keccak::keccak_update(&ctx,
          reinterpret_cast<const unsigned char*>(msg.data()), msg.size());
      unsigned char eth_hash[32];
      keccak::keccak_final(&ctx, eth_hash);

      // 2) verify
      assert_recover_key(eth_hash, sig, pubKey);

      // 3) decompress + address = last 20 bytes of keccak(pubkey)
      auto compressed = std::get<3>(pubKey); // std::array<char,33>
      std::string hex;
      hex.reserve(66);
      for (auto c : compressed) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(c));
        hex += buf;
      }
      auto full = compression::decompressPublicKey(hex).substr(2);

      std::vector<unsigned char> raw;
      raw.reserve(full.size() / 2);
      for (std::size_t i = 0; i < full.size(); i += 2) {
        raw.push_back(static_cast<unsigned char>(
            std::strtol(full.substr(i, 2).c_str(), nullptr, 16)
        ));
      }

      keccak::SHA3_CTX pk_ctx;
      keccak::keccak_init(&pk_ctx);
      keccak::keccak_update(&pk_ctx, raw.data(), raw.size());
      std::vector<unsigned char> pk_hash(32);
      keccak::keccak_final(&pk_ctx, pk_hash.data());

      // // Optional: log the final pk_hash as hex as well
      // std::string pk_hash_hex;
      // pk_hash_hex.reserve(64);
      // for (auto b : pk_hash) {
      //     char buf[3];
      //     snprintf(buf, sizeof(buf), "%02x", b);
      //     pk_hash_hex += buf;
      // }
      // sysio::print("keccak(pubkey) hash: ", pk_hash_hex, "\n");

      std::string eth_addr = "0x";
      eth_addr.reserve(42);
      for (auto it = pk_hash.end() - 20; it != pk_hash.end(); ++it) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(*it));
        eth_addr += buf;
      }

      links.emplace(get_self(), [&](auto& a) {
          a.key      = links.available_primary_key();
          a.username = username;
          a.chain    = chainName;
          a.pub_key  = pubKey;
          a.address  = eth_addr;
      });

      action(
          permission_level{get_self(), "owner"_n},
          get_self(), "onlinkauth"_n,
          std::make_tuple(username, "ec.ext"_n, pubKey)
      ).send();

    } else { // curve == ed
      checksum256 hash256;

      if (chainName == "solana"_n) {
        // 1) sha256(msg) → returns a checksum256
        checksum256 raw_digest = sysio::sha256(msg.c_str(), msg.size());
        auto const* raw1 = reinterpret_cast<const unsigned char*>(raw_digest.data());

        // 2) map each byte into printable ASCII [33..126]
        unsigned char mapped[32];
        for (int i = 0; i < 32; ++i) {
          mapped[i] = static_cast<unsigned char>((raw1[i] % 94) + 33);
        }

        // 3) pack into checksum256
        std::memcpy(hash256.data(), mapped, 32);
      } else { // sui
        std::vector<uint8_t> bcs;
        bcs.reserve(4 + msg.size());
        bcs.insert(bcs.end(), {3, 0, 0, static_cast<uint8_t>(msg.size())});
        bcs.insert(bcs.end(), msg.begin(), msg.end());

        unsigned char raw_digest[32];
        check(blake2b_256(
            reinterpret_cast<const char*>(bcs.data()), bcs.size(),
            reinterpret_cast<char*>(raw_digest), sizeof(raw_digest)) == 0,
            "blake2b_256 failed");
        hash256 = checksum256(raw_digest);
      }

      assert_recover_key(hash256, sig, pubKey);

      // ed25519 raw 32-byte key
      const auto& arr = std::get<4>(pubKey);
      unsigned char raw_key[32];
      std::copy(arr.begin(), arr.end(), raw_key);

      // Solana address
      auto sol = base58::EncodeBase58(raw_key, raw_key + 32);
      links.emplace(get_self(), [&](auto& a) {
          a.key      = links.available_primary_key();
          a.username = username;
          a.chain    = "solana"_n;
          a.pub_key  = pubKey;
          a.address  = sol;
      });

      // SUI address
      unsigned char sui_raw[32];
      check(blake2b_256(
          reinterpret_cast<const char*>(raw_key), 32,
          reinterpret_cast<char*>(sui_raw), sizeof(sui_raw)) == 0,
          "blake2b_256(pubkey) failed");

      std::string sui = "0x";
      sui.reserve(66);
      for (auto b : sui_raw) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(b));
        sui += buf;
      }
      links.emplace(get_self(), [&](auto& a) {
          a.key      = links.available_primary_key();
          a.username = username;
          a.chain    = "sui"_n;
          a.pub_key  = pubKey;
          a.address  = sui;
      });

      action(
          permission_level{get_self(), "owner"_n},
          get_self(), "onlinkauth"_n,
          std::make_tuple(username, "ed.ext"_n, pubKey)
      ).send();
    }
  }


  [[sysio::action]] void authlink::onlinkauth(const name &accountName, const name &permission, const sysio::public_key &pubKey) {
    require_auth(get_self());

    // **Notify necessary parties, with notify parameters.**
    require_recipient(name("sysio"));
  };

  // ! Clear links table, for testing only. Remove before deployment.
  [[sysio::action]] void authlink::clearlinks() {
    require_auth(get_self());

    links_t links(get_self(), get_self().value);

    // Delete all entries in the links table.
    auto itr = links.begin();
    while (itr != links.end()) {
      itr = links.erase(itr);
    }
  };

  // TODO: Adjust this logic need to handle removal of auth.ed.ext or auth.ec.ext respectively.
  void authlink::onmanualrmv(const name& account, const name& permission) {
    uint128_t name_curve;
    switch(permission.value) {
      case "ed.ext"_n.value:
        // Handle removal of ED curve link
        name_curve = (uint128_t(account.value) << 64) | name("ed").value;
        break;
      case "ec.ext"_n.value:
        // Handle removal of EC curve link
        name_curve = (uint128_t(account.value) << 64) | name("ec").value;
        break;
      default:
        sysio::check(false, "Invalid permission for removal.");
    }

    // Find reference to 'account' in links table
    links_t links(get_self(), get_self().value);
    auto itr = links.find(name_curve);
    if(itr == links.end()) return;

    // Delete the row
    links.erase(itr);
  };

  // ----- PRIVATE HELPER METHODS -----

  std::array<uint8_t, 4> authlink::digestSuffixRipemd160(const std::array<char, 33> &data, const std::string &extra) {
    std::array<char, 35> d;  // 33 for data and 2 for 'K1' from extra

    // Copy data to d
    for(int i = 0; i < 33; ++i) {
      d[i] = data[i];
    }

    // Append 'K1' from extra to d
    d[33] = extra[0];
    d[34] = extra[1];

    // Get ripemd160 hash
    sysio::checksum160 hash = sysio::ripemd160(d.data(), d.size());

    // Extract the hash data
    auto hash_data = hash.extract_as_byte_array();

    // Prepare the result
    std::array<uint8_t, 4> result = {
      static_cast<uint8_t>(hash_data[0]),
      static_cast<uint8_t>(hash_data[1]),
      static_cast<uint8_t>(hash_data[2]),
      static_cast<uint8_t>(hash_data[3])
  };

    return result;
  };

  std::string authlink::pubkey_to_string(const sysio::public_key& pk) {
    switch (pk.index()) {
      case 3: { // PUB_EM_
        // raw is std::array<char,33>
        auto raw = std::get<3>(pk);

        // 1) turn it into array<char,33> for your helper
        std::array<char,33> data_char;
        std::copy(raw.begin(), raw.end(), data_char.begin());

        // 2) use your existing ripemdSuffix helper
        auto chk = digestSuffixRipemd160(data_char, "EM");

        // 3) build the 37-byte [ key || checksum ] buffer
        std::array<uint8_t,37> buf;
        for (size_t i = 0; i < 33; ++i) buf[i]     = static_cast<uint8_t>(data_char[i]);
        for (size_t i = 0; i < 4;  ++i) buf[33 + i] = chk[i];

        return "PUB_EM_" + base58::EncodeBase58(buf.data(), buf.data() + buf.size());
      }

      case 4: { // PUB_ED_
        // raw is std::array<char,32>
        auto raw = std::get<4>(pk);

        // 1) copy into a uint8_t array for the final buffer
        std::array<uint8_t,32> key_bytes;
        for (size_t i = 0; i < 32; ++i)
          key_bytes[i] = static_cast<uint8_t>(raw[i]);

        // 2) Inline the RIPEMD160+suffix for ED (32-byte key + "ED")
        std::array<char,34> d;              // 32 bytes key + 2 bytes tag
        for (size_t i = 0; i < 32; ++i)      d[i]     = raw[i];
        d[32] = 'E';                        // tag
        d[33] = 'D';                        // tag

        auto     hash   = sysio::ripemd160(d.data(), d.size());
        auto     hd     = hash.extract_as_byte_array();
        std::array<uint8_t,4> chk = {
          static_cast<uint8_t>(hd[0]),
          static_cast<uint8_t>(hd[1]),
          static_cast<uint8_t>(hd[2]),
          static_cast<uint8_t>(hd[3])
          };

        // 3) build the 36-byte [ key || checksum ] buffer
        std::array<uint8_t,36> buf;
        std::copy_n(key_bytes.begin(), 32, buf.begin());
        std::copy_n(chk.begin(),       4, buf.begin() + 32);

        return "PUB_ED_" + base58::EncodeBase58(buf.data(), buf.data() + buf.size());
      }

      default:
        sysio::check(false, "pubkey_to_string only supports EM (3) and ED (4)");
        return {};
    }
  }
}