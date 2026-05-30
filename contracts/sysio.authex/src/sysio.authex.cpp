#include <cstdint>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.system/native.hpp>
#include <sysio/print.hpp>
#include <sysio/system.hpp>   // get_ram_usage

namespace {
using namespace sysio;

// ABI of sysio.roa::giftram, for the inline call in createlink.
struct giftram_args {
   name    account;
   int64_t usage_before;
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


} // anonymous namespace


namespace sysio {

// ----- PUBLIC ACTIONS -----
[[sysio::action]] void authex::createlink(const opp::types::ChainKind chain_kind, const name& account,
                                          const signature& sig, const public_key& pub_key, const uint64_t nonce) {
   using ChainKind = opp::types::ChainKind;

   // Require caller authorization
   require_auth(account);

   // ——— Chain kind validation ———
   // TODO @jglanz: SUI removed in v6; restore when SUI outpost is added.
   check(chain_kind == ChainKind::CHAIN_KIND_EVM
         || chain_kind == ChainKind::CHAIN_KIND_SVM,
         "Invalid chain_kind. Supported: CHAIN_KIND_EVM(2), CHAIN_KIND_SVM(3).");

   // ——— Table & indices ———
   links_t links(get_self());
   auto by_namechain = links.get_index<"bynamechain"_n>();
   uint128_t name_chain = to_namechain_key(account, chain_kind);
   check(by_namechain.find(name_chain) == by_namechain.end(), "Account already has a link for this chain.");

   auto by_pubkey = links.get_index<"bypubkey"_n>();
   auto pub_hash = pubkey_to_checksum256(pub_key);
   check(by_pubkey.find(pub_hash) == by_pubkey.end(), "Public key already linked to a different account.");

   // ——— Nonce freshness ———
   constexpr uint64_t TEN_MIN_MS = 10 * 60 * 1000;
   uint64_t now_ms = current_time_point().time_since_epoch().count() / 1000;
   check(nonce <= now_ms && now_ms - nonce <= TEN_MIN_MS, "Invalid nonce: must be within the last 10 minutes");

   // ——— Build the message string ———
   //
   // Wire format: the chain identifier is serialised as the decimal of
   // its proto numeric value (EVM=2, SVM=3). `magic_enum::enum_integer`
   // extracts the underlying value type-safely; off-chain signers
   // reconstruct the same string from their generated `ChainKind` enum's
   // numeric value.
   static constexpr const char* DIGEST_TAIL = "createlink auth";
   std::string chain_kind_str = std::to_string(magic_enum::enum_integer(chain_kind));
   std::string msg = pubkey_to_string(pub_key) + "|" + account.to_string() + "|" + chain_kind_str + "|" +
                     std::to_string(nonce) + "|" + DIGEST_TAIL;

   // For EM keys, recover_key returns the real y-parity prefix.
   // Store it so downstream consumers (advance → OPERATORS attestation)
   // get the correct compressed key for ETH address derivation.
   public_key verified_pub_key = pub_key;

   // ——— Curve-specific signing & address derivation ———
   if (chain_kind == ChainKind::CHAIN_KIND_EVM) {
      // 1) keccak(msg) — use the pubkey string as the contract sees it
      //    (fc/CDT may normalize the compression prefix byte)
      auto eth_hash = sysio::keccak(msg.c_str(), msg.size());

      // 2) recover the public key from the signature + digest, then compare
      //    the x-coordinate (bytes 1..32). The prefix byte (y-parity) may differ
      //    due to EIP-191 double-hashing in the recovery path, but the
      //    x-coordinate is sufficient to verify key ownership.
      //    The recovered key (with correct prefix from libsecp256k1) is stored
      //    in the link table so downstream consumers get the real y-parity.
      auto recovered = recover_key(eth_hash, sig);
      auto expected_raw = std::get<3>(pub_key);
      auto recovered_raw = std::get<3>(recovered);

      check(std::equal(expected_raw.begin() + 1, expected_raw.end(),
                       recovered_raw.begin() + 1),
            "EM key recovery failed: x-coordinate mismatch");

      verified_pub_key = recovered;

   } else if (chain_kind == ChainKind::CHAIN_KIND_SVM) {
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
   }

   // CREATE LINK RECORD — use verified_pub_key which has the real y-parity
   // prefix from recovery (for EM) rather than the potentially ambiguous input.
   uint64_t next_key = 0;
   if (links.cbegin() != links.cend()) {
      auto last = --links.cend();
      next_key = last->key + 1;
   }
   links.emplace("sysio"_n, links_key{next_key}, links_s{
      .key = next_key,
      .username = account,
      .chain_kind = chain_kind,
      .pub_key = verified_pub_key,
   });

   // Snapshot RAM usage before the inline auth change below; giftram (which runs after
   // it) gifts exactly the delta it consumes. RAM is checked at transaction end, so the
   // temporary overage in between is fine.
   int64_t usage_before = get_ram_usage(account);

   // AMEND `active` PERMISSIONS
   action(permission_level{"sysio"_n, "active"_n}, "sysio"_n, "expandauth"_n,
          expandauth{account, "active"_n,

                     std::vector<key_weight>{key_weight{verified_pub_key, 1}},
                     std::vector<sysiosystem::permission_level_weight>{}})
      .send();

   // GIFT exactly the RAM the active key-add consumes, via sysio.roa::giftram (runs after
   // expandauth → sees the post-add usage), drawn from sysio's pool.
   action(permission_level{get_self(), "owner"_n}, "sysio.roa"_n, "giftram"_n,
          giftram_args{account, usage_before})
      .send();
}


// ! Clear links table, for testing only. Remove before deployment.
[[sysio::action]] void authex::clearlinks() {
   require_auth(get_self());

   links_t links(get_self());

   // Delete all entries in the links table.
   auto itr = links.begin();
   while (itr != links.end()) {
      itr = links.erase(itr);
   }
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

} // namespace sysio
