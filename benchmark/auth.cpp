#include <fc/crypto/private_key.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/authority_checker.hpp>

#include <benchmark.hpp>

using namespace sysio::chain;

namespace sysio::benchmark {

void auth_benchmarking() {
   // Pre-generate a pool of keys
   constexpr int NUM_KEYS = 50;
   std::vector<fc::crypto::public_key> pub_keys;
   pub_keys.reserve(NUM_KEYS);
   for (int i = 0; i < NUM_KEYS; ++i) {
      pub_keys.push_back(fc::crypto::private_key::generate().get_public_key());
   }

   // Null authority lookup (no recursive permission resolution)
   auto get_null_authority = [](const permission_level&) -> const authority* {
      return nullptr;
   };

   // --- Single-key authority satisfaction (most common case) ---
   {
      auto auth = authority(1, {key_weight{pub_keys[0], 1}});
      flat_set<fc::crypto::public_key> provided{pub_keys[0]};

      benchmarking("auth_single_key", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- Single-key authority NOT satisfied (exercises rollback) ---
   {
      auto auth = authority(1, {key_weight{pub_keys[0], 1}});
      flat_set<fc::crypto::public_key> provided{pub_keys[1]};

      benchmarking("auth_single_key_fail", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- Multi-sig 1-of-3 (threshold-1 fast path) ---
   {
      auto auth = authority(1, {
         key_weight{pub_keys[0], 1},
         key_weight{pub_keys[1], 1},
         key_weight{pub_keys[2], 1}
      });
      flat_set<fc::crypto::public_key> provided{pub_keys[1]};

      benchmarking("auth_multisig_1of3", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- Multi-sig 2-of-3 ---
   {
      auto auth = authority(2, {
         key_weight{pub_keys[0], 1},
         key_weight{pub_keys[1], 1},
         key_weight{pub_keys[2], 1}
      });
      flat_set<fc::crypto::public_key> provided{pub_keys[0], pub_keys[1]};

      benchmarking("auth_multisig_2of3", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- Multi-sig 3-of-5 ---
   {
      auto auth = authority(3, {
         key_weight{pub_keys[0], 1},
         key_weight{pub_keys[1], 1},
         key_weight{pub_keys[2], 1},
         key_weight{pub_keys[3], 1},
         key_weight{pub_keys[4], 1}
      });
      flat_set<fc::crypto::public_key> provided{pub_keys[0], pub_keys[1], pub_keys[2]};

      benchmarking("auth_multisig_3of5", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- Key search with 50 provided keys ---
   {
      auto auth = authority(1, {key_weight{pub_keys[25], 1}});
      flat_set<fc::crypto::public_key> provided(pub_keys.begin(), pub_keys.end());

      benchmarking("auth_search_in_50_keys", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- 10-key authority with 50 provided keys ---
   {
      std::vector<key_weight> kws;
      for (int i = 0; i < 10; ++i) {
         kws.push_back(key_weight{pub_keys[i * 5], 1});
      }
      auto auth = authority(5, kws);
      flat_set<fc::crypto::public_key> provided(pub_keys.begin(), pub_keys.end());

      benchmarking("auth_10keys_in_50_provided", [&]() {
         auto checker = make_auth_checker(get_null_authority, 2, provided);
         checker.satisfied(auth);
      });
   }

   // --- Recursive permission resolution (2 levels deep) ---
   {
      authority bob_auth(1, {key_weight{pub_keys[1], 1}});

      auto auth = authority(2, {key_weight{pub_keys[0], 1}},
                            {permission_level_weight{{name("bob"), name("active")}, 1}});

      auto get_auth = [&bob_auth](const permission_level& p) -> const authority* {
         if (p.actor == name("bob") && p.permission == name("active"))
            return &bob_auth;
         return nullptr;
      };

      flat_set<fc::crypto::public_key> provided{pub_keys[0], pub_keys[1]};

      benchmarking("auth_recursive_2_level", [&]() {
         auto checker = make_auth_checker(get_auth, 3, provided);
         checker.satisfied(auth);
      });
   }

   // --- Recursive permission resolution (3 levels deep) ---
   {
      authority charlie_auth(1, {key_weight{pub_keys[2], 1}});

      authority bob_auth(2, {key_weight{pub_keys[1], 1}},
                         {permission_level_weight{{name("charlie"), name("active")}, 1}});

      auto auth = authority(2, {key_weight{pub_keys[0], 1}},
                            {permission_level_weight{{name("bob"), name("active")}, 1}});

      auto get_auth = [&](const permission_level& p) -> const authority* {
         if (p.actor == name("bob") && p.permission == name("active"))
            return &bob_auth;
         if (p.actor == name("charlie") && p.permission == name("active"))
            return &charlie_auth;
         return nullptr;
      };

      flat_set<fc::crypto::public_key> provided{pub_keys[0], pub_keys[1], pub_keys[2]};

      benchmarking("auth_recursive_3_level", [&]() {
         auto checker = make_auth_checker(get_auth, 4, provided);
         checker.satisfied(auth);
      });
   }
}

} // namespace sysio::benchmark
