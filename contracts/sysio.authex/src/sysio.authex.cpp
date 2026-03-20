#include <cstdint>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.system/native.hpp>
#include <sysio/print.hpp>

namespace {
using namespace sysio;

constexpr name ex_eth = "ex.eth"_n;
constexpr name ex_sol = "ex.sol"_n;
constexpr name ex_sui = "ex.sui"_n;

/**
 * Duplicated struct representing ABI of the `updateauth` action.
 */
struct updateauth {
   name account;
   name permission;
   name parent;
   sysiosystem::authority auth;
};

struct expandauth {
   name account;
   name permission;
   std::vector<key_weight> new_keys;
   std::vector<sysiosystem::permission_level_weight> new_accounts;
};

using ed_raw_key_t = std::array<uint8_t, 32>;

[[maybe_unused]] ed_raw_key_t get_ed_raw_key(const sysio::public_key& pub_key) {
   const auto& arr = std::get<4>(pub_key);
   ed_raw_key_t raw_key;
   std::copy(arr.begin(), arr.end(), raw_key.data());
   return raw_key;
}


/**
 * Bitcoin base58 alphabet
 */
constexpr char base58_alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/**
 * Encodes a given byte array into a Base58 encoded string using the Bitcoin Base58
 * alphabet ("123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz").
 *
 * Base58 encoding provides a compact, human-readable textual representation of binary
 * data. It avoids visually ambiguous characters, such as '0' (zero) and 'O' (uppercase
 * o), making it suitable for use in financial identifiers, cryptocurrency addresses,
 * and similar contexts.
 *
 * @param bytes Pointer to the byte array to be encoded.
 * @param data_len Length of the byte array to be encoded.
 * @return A Base58 encoded string representation of the input byte array.
 */
std::string base58_encode(const unsigned char* bytes, uint32_t data_len) {
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
} // anonymous namespace


namespace sysio {

// ----- PUBLIC ACTIONS -----
[[sysio::action]] void authex::createlink(const fc::crypto::chain_kind_t chain_kind, const name& account,
                                          const signature& sig, const public_key& pub_key, const uint64_t nonce) {
   using namespace fc::crypto;
   // Require caller authorization
   require_auth(account);

   // ——— Chain kind validation ———
   check(chain_kind == chain_kind_ethereum || chain_kind == chain_kind_solana || chain_kind == chain_kind_sui,
         "Invalid chain_kind. Supported: chain_kind_ethereum(2), chain_kind_solana(3), chain_kind_sui(4).");

   // ——— Table & indices ———
   links_t links(get_self(), get_self().value);
   auto by_namechain = links.get_index<"bynamechain"_n>();
   uint128_t name_chain = (static_cast<uint128_t>(account.value) << 64) | static_cast<uint64_t>(chain_kind);
   check(by_namechain.find(name_chain) == by_namechain.end(), "Account already has a link for this chain.");

   auto by_pubkey = links.get_index<"bypubkey"_n>();
   auto pub_hash = pubkey_to_checksum256(pub_key);
   check(by_pubkey.find(pub_hash) == by_pubkey.end(), "Public key already linked to a different account.");

   // ——— Nonce freshness ———
   constexpr uint64_t TEN_MIN_MS = 10 * 60 * 1000;
   uint64_t now_ms = current_time_point().time_since_epoch().count() / 1000;
   check(nonce <= now_ms && now_ms - nonce <= TEN_MIN_MS, "Invalid nonce: must be within the last 10 minutes");

   // ——— Build the message string ———
   static constexpr const char* DIGEST_TAIL = "createlink auth";
   std::string chain_kind_str = std::to_string(static_cast<uint8_t>(chain_kind));
   std::string msg = pubkey_to_string(pub_key) + "|" + account.to_string() + "|" + chain_kind_str + "|" +
                     std::to_string(nonce) + "|" + DIGEST_TAIL;

   std::optional<name> ex_permission = std::nullopt;
   // ——— Curve-specific signing & address derivation ———
   if (chain_kind == chain_kind_ethereum) {
      // 1) keccak(msg)
      auto eth_hash = sysio::keccak(msg.c_str(), msg.size());

      // 2) verify
      assert_recover_key(eth_hash, sig, pub_key);


      ex_permission = ex_eth;

   } else if (chain_kind == chain_kind_solana) {
      checksum256 hash256;
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
      assert_recover_key(hash256, sig, pub_key);

      ex_permission = ex_sol;
   } else if (chain_kind == chain_kind_sui) { // sui
      std::vector<uint8_t> bcs;
      bcs.reserve(4 + msg.size());
      bcs.insert(bcs.end(), {3, 0, 0, static_cast<uint8_t>(msg.size())});
      bcs.insert(bcs.end(), msg.begin(), msg.end());

      unsigned char raw_digest[32];
      check(sysio::blake2b_256(reinterpret_cast<const char*>(bcs.data()), bcs.size(),
                               reinterpret_cast<char*>(raw_digest), sizeof(raw_digest)) == 0,
            "blake2b_256 failed");

      ex_permission = ex_sui;
   }

   // MAKE SURE WE MAPPED TO A SUPPORTED PERMISSION
   sysio::check(ex_permission.has_value(), "Internal error: ex_permission not set");

   // CREATE LINK RECORD
   links.emplace("sysio"_n, [&](auto& a) {
      a.key = links.available_primary_key();
      a.username = account;
      a.chain_kind = chain_kind;
      a.pub_key = pub_key;
   });

   // PUSH `ex.<chain_prefix>` TO PERMISSIONS
   action(
      permission_level{
         get_self(), "owner"_n
   },
      "sysio"_n, "updateauth"_n,
      updateauth{account,
                 ex_permission.value(),
                 "active"_n,
                 {
                    1,
                    {{pub_key, 1}},
                 }})
      .send();

   // AMEND `active` PERMISSIONS
   action(permission_level{"sysio"_n, "active"_n}, "sysio"_n, "expandauth"_n,
          expandauth{account, "active"_n,

                     std::vector<key_weight>{key_weight{pub_key, 1}},
                     std::vector<sysiosystem::permission_level_weight>{}})
      .send();
}


// ! Clear links table, for testing only. Remove before deployment.
[[sysio::action]] void authex::clearlinks() {
   require_auth(get_self());

   links_t links(get_self(), get_self().value);

   // Delete all entries in the links table.
   auto itr = links.begin();
   while (itr != links.end()) {
      itr = links.erase(itr);
   }
};


// TODO: Adjust this logic need to handle removal of ex.eth or ex.sol respectively.
void authex::onmanualrmv(const name& account, const name& permission) {
   using namespace fc::crypto;

   chain_kind_t kind;
   switch (permission.value) {
   case ex_sol.value:
      kind = chain_kind_solana;
      break;
   case ex_eth.value:
      kind = chain_kind_ethereum;
      break;
   case ex_sui.value:
      kind = chain_kind_sui;
      break;
   default:
      sysio::check(false, "Invalid permission for removal.");
      return; // unreachable, silences uninitialized warning
   }

   // Find reference to 'account' in links table via namechain index
   links_t links(get_self(), get_self().value);
   auto by_namechain = links.get_index<"bynamechain"_n>();
   uint128_t name_chain = to_namechain_key(account, kind);
   auto itr = by_namechain.find(name_chain);
   if (itr == by_namechain.end())
      return;

   by_namechain.erase(itr);
};

// ----- PRIVATE HELPER METHODS -----

std::array<uint8_t, 4> authex::digestSuffixRipemd160(const std::array<char, 33>& data, const std::string& extra) {
   std::array<char, 35> d; // 33 for data and 2 for 'K1' from extra

   // Copy data to d
   for (int i = 0; i < 33; ++i) {
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
   std::array<uint8_t, 4> result = {static_cast<uint8_t>(hash_data[0]), static_cast<uint8_t>(hash_data[1]),
                                    static_cast<uint8_t>(hash_data[2]), static_cast<uint8_t>(hash_data[3])};

   return result;
};

std::string authex::pubkey_to_string(const sysio::public_key& pk) {
   switch (pk.index()) {
   case fc::crypto::key_type_em: { // PUB_EM_

      auto raw = std::get<3>(pk);

      return "PUB_EM_" + sysio::to_hex(reinterpret_cast<const char*>(raw.data()), raw.size());
   }

   case 4: { // PUB_ED_
      // raw is std::array<char,32>
      auto raw = std::get<4>(pk);

      // 1) copy into a uint8_t array for the final buffer
      std::array<uint8_t, 32> key_bytes;
      for (size_t i = 0; i < 32; ++i)
         key_bytes[i] = static_cast<uint8_t>(raw[i]);

      // 2) Inline the RIPEMD160+suffix for ED (32-byte key + "ED")
      std::array<char, 34> d; // 32 bytes key + 2 bytes tag
      for (size_t i = 0; i < 32; ++i)
         d[i] = raw[i];
      d[32] = 'E'; // tag
      d[33] = 'D'; // tag

      auto hash = sysio::ripemd160(d.data(), d.size());
      auto hd = hash.extract_as_byte_array();
      std::array<uint8_t, 4> chk = {static_cast<uint8_t>(hd[0]), static_cast<uint8_t>(hd[1]),
                                    static_cast<uint8_t>(hd[2]), static_cast<uint8_t>(hd[3])};

      // 3) build the 36-byte [ key || checksum ] buffer
      std::array<uint8_t, 36> buf;
      std::copy_n(key_bytes.begin(), 32, buf.begin());
      std::copy_n(chk.begin(), 4, buf.begin() + 32);

      return "PUB_ED_" + base58_encode(buf.data(), buf.size());
   }

   default:
      sysio::check(false, "pubkey_to_string only supports EM (3) and ED (4)");
      return {};
   }
}
} // namespace sysio
