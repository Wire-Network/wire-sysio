#include <sysio/chain/authority.hpp>
#include <sysio/chain/authority_checker.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/io/raw.hpp>
#include <fc/scoped_exit.hpp>

#include <boost/container/flat_map.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace sysio::chain;

namespace {

namespace legacy {

using meta_permission_key   = std::tuple<uint32_t, int>;
using meta_permission_value = std::function<uint32_t()>;
using meta_permission_map =
   boost::container::flat_multimap<meta_permission_key, meta_permission_value, std::greater<>>;

/**
 * The authority walk as it was when every entry went through a flat_multimap keyed on (weight, priority). Kept here,
 * and only here, as the reference the current @ref sysio::chain::authority_checker must match for every input.
 */
template<typename PermissionToAuthorityFunc>
class authority_checker {
private:
   PermissionToAuthorityFunc  permission_to_authority;
   vector<public_key_type>    provided_keys;
   flat_set<permission_level> provided_permissions;
   vector<bool>               _used_keys;
   uint16_t                   recursion_depth_limit;

public:
   authority_checker(PermissionToAuthorityFunc permission_to_authority, uint16_t recursion_depth_limit,
                     const flat_set<public_key_type>& provided_keys,
                     const flat_set<permission_level>& provided_permissions)
      : permission_to_authority(permission_to_authority)
      , provided_keys(provided_keys.begin(), provided_keys.end())
      , provided_permissions(provided_permissions)
      , _used_keys(provided_keys.size(), false)
      , recursion_depth_limit(recursion_depth_limit) {}

   enum permission_cache_status { being_evaluated, permission_unsatisfied, permission_satisfied };

   typedef flat_map<permission_level, permission_cache_status> permission_cache_type;

   bool satisfied(const permission_level& permission, permission_cache_type* cached_perms = nullptr) {
      permission_cache_type cached_permissions;

      if (cached_perms == nullptr)
         cached_perms = initialize_permission_cache(cached_permissions);

      weight_tally_visitor visitor(*this, *cached_perms, 0);
      return (visitor(permission_level_weight{permission, 1}) > 0);
   }

   template<typename AuthorityType>
   bool satisfied(const AuthorityType& authority, permission_cache_type* cached_perms = nullptr) {
      permission_cache_type cached_permissions;

      if (cached_perms == nullptr)
         cached_perms = initialize_permission_cache(cached_permissions);

      return satisfied(authority, *cached_perms, 0);
   }

   bool all_keys_used() const { return boost::algorithm::all_of_equal(_used_keys, true); }

   flat_set<public_key_type> used_keys() const {
      auto range = filter_data_by_marker(provided_keys, _used_keys, true);
      return {range.begin(), range.end()};
   }

   static std::optional<permission_cache_status> permission_status_in_cache(const permission_cache_type& permissions,
                                                                           const permission_level& level) {
      auto itr = permissions.find(level);
      if (itr != permissions.end())
         return itr->second;

      itr = permissions.find({level.actor, permission_name()});
      if (itr != permissions.end())
         return itr->second;

      return std::optional<permission_cache_status>();
   }

private:
   static const public_key_type& resolve_key(const public_key_type& k) { return k; }
   static public_key_type resolve_key(const shared_public_key& k) { return k.to_public_key(); }

   permission_cache_type* initialize_permission_cache(permission_cache_type& cached_permissions) {
      for (const auto& p : provided_permissions) {
         cached_permissions.emplace_hint(cached_permissions.end(), p, permission_satisfied);
      }
      return &cached_permissions;
   }

   template<typename AuthorityType>
   bool satisfied(const AuthorityType& authority, permission_cache_type& cached_permissions, uint16_t depth) {
      if (authority.accounts.empty()) {
         if (authority.keys.size() == 1 && authority.threshold <= authority.keys[0].weight) {
            const auto& search_key = resolve_key(authority.keys[0].key);
            auto itr = std::lower_bound(provided_keys.begin(), provided_keys.end(), search_key);
            if (itr != provided_keys.end() && *itr == search_key) {
               _used_keys[itr - provided_keys.begin()] = true;
               return true;
            }
            return false;
         }
         if (authority.threshold == 1) {
            auto best = provided_keys.end();
            uint32_t best_weight = 0;
            for (const auto& k : authority.keys) {
               if (k.weight < authority.threshold || k.weight <= best_weight)
                  continue;
               const auto& search_key = resolve_key(k.key);
               auto itr = std::lower_bound(provided_keys.begin(), provided_keys.end(), search_key);
               if (itr != provided_keys.end() && *itr == search_key) {
                  best = itr;
                  best_weight = k.weight;
               }
            }
            if (best != provided_keys.end()) {
               _used_keys[best - provided_keys.begin()] = true;
               return true;
            }
            return false;
         }
      }

      auto KeyReverter = fc::make_scoped_exit([this, keys = _used_keys]() mutable { _used_keys = keys; });

      weight_tally_visitor visitor(*this, cached_permissions, depth);

      meta_permission_map permissions;

      auto emplace_permission = [&permissions, &visitor](int priority, const auto& mp) {
         permissions.emplace(std::make_tuple(mp.weight, priority), [&mp, &visitor]() { return visitor(mp); });
      };

      permissions.reserve(authority.keys.size() + authority.accounts.size());
      std::for_each(authority.accounts.begin(), authority.accounts.end(),
                    std::bind(emplace_permission, 1, std::placeholders::_1));
      std::for_each(authority.keys.begin(), authority.keys.end(),
                    std::bind(emplace_permission, 2, std::placeholders::_1));

      for (const auto& p : permissions)
         if (p.second() >= authority.threshold) {
            KeyReverter.cancel();
            return true;
         }
      return false;
   }

   struct weight_tally_visitor {
      authority_checker&     checker;
      permission_cache_type& cached_permissions;
      uint16_t               recursion_depth;
      uint32_t               total_weight = 0;

      weight_tally_visitor(authority_checker& checker, permission_cache_type& cached_permissions,
                           uint16_t recursion_depth)
         : checker(checker)
         , cached_permissions(cached_permissions)
         , recursion_depth(recursion_depth) {}

      template<typename KeyWeight,
               typename = std::enable_if_t<detail::is_any_of_v<KeyWeight, shared_key_weight, key_weight>>>
      uint32_t operator()(const KeyWeight& permission) {
         const auto& search_key = authority_checker::resolve_key(permission.key);
         auto itr = std::lower_bound(checker.provided_keys.begin(), checker.provided_keys.end(), search_key);
         if (itr != checker.provided_keys.end() && *itr == search_key) {
            checker._used_keys[itr - checker.provided_keys.begin()] = true;
            total_weight += permission.weight;
         }
         return total_weight;
      }

      uint32_t operator()(const permission_level_weight& permission) {
         auto status = authority_checker::permission_status_in_cache(cached_permissions, permission.permission);
         if (!status) {
            if (recursion_depth < checker.recursion_depth_limit) {
               bool r = false;

               std::invoke_result_t<decltype(checker.permission_to_authority), const permission_level> auth = nullptr;
               try {
                  auth = checker.permission_to_authority(permission.permission);
               } catch (const permission_query_exception&) {}
               if (!auth)
                  return total_weight;

               cached_permissions.emplace(permission.permission, being_evaluated);
               r = checker.satisfied(*auth, cached_permissions, recursion_depth + 1);

               auto cache_itr = cached_permissions.find(permission.permission);
               assert(cache_itr != cached_permissions.end());
               if (r) {
                  total_weight += permission.weight;
                  cache_itr->second = permission_satisfied;
               } else {
                  cache_itr->second = permission_unsatisfied;
               }
            }
         } else if (*status == permission_satisfied) {
            total_weight += permission.weight;
         }
         return total_weight;
      }
   };
};

} // namespace legacy

/// Deterministic K1 public key from raw bytes. The walk only orders and compares keys, so no curve point is needed.
public_key_type make_test_key(uint64_t seed) {
   const fc::sha256 digest = fc::sha256::hash(seed);
   fc::ecc::public_key_data data{};
   data[0] = 0x02; // compressed-point prefix
   std::memcpy(data.data() + 1, digest.data(), sizeof(digest));
   return public_key_type(public_key_type::storage_type(fc::ecc::public_key_shim(data)));
}

/// `count` distinct test keys in ascending order.
std::vector<public_key_type> make_sorted_test_keys(size_t count) {
   std::vector<public_key_type> keys;
   keys.reserve(count);
   for (size_t i = 0; i < count; ++i)
      keys.push_back(make_test_key(i));
   std::sort(keys.begin(), keys.end());
   return keys;
}

/// A cache's contents in key order, comparable across the legacy flat_map and the current cache type.
template<typename Cache>
std::vector<std::pair<permission_level, int>> cache_contents(const Cache& cache) {
   std::vector<std::pair<permission_level, int>> contents;
   for (const auto& [level, status] : cache)
      contents.emplace_back(level, static_cast<int>(status));
   return contents;
}

/// How the weights of a random authority are drawn.
enum class weight_pattern { equal, ascending, descending, small_random, full_random };
constexpr uint32_t weight_pattern_count = 5;

/// Inputs of one randomized differential case.
struct random_case {
   std::map<permission_level, authority>        authorities;
   std::map<permission_level, shared_authority> shared_authorities;
   std::set<permission_level>                   throwing_lookups;
   flat_set<public_key_type>                    provided_keys;
   flat_set<permission_level>                   provided_permissions;
   uint16_t                                     depth_limit = 0;
};

/// One query run against both checkers of a case.
struct random_query {
   std::optional<permission_level> level;          ///< query a permission when set, else `auth`
   authority                       auth;           ///< queried directly when `level` is empty
   bool                            external_cache = false; ///< pass a cache kept across the queries of the case
};

/// Draws random cases from a fixed seed so a failure is reproducible.
class case_generator {
public:
   static constexpr size_t   key_pool_size        = 12;
   static constexpr uint32_t max_keys_per_auth    = 9;
   static constexpr uint32_t max_accounts_per_auth = 6;
   static constexpr uint64_t seed                 = 0x5157'a07e'c4ec'0001ull;

   case_generator() : key_pool(make_sorted_test_keys(key_pool_size)) {
      for (const name actor : {name("alice"), name("bob"), name("carol"), name("dave"), name("erin")})
         for (const name perm : {config::owner_name, config::active_name, name("custom")})
            levels.push_back({actor, perm});
   }

   random_case next_case() {
      random_case c;
      for (const permission_level& level : levels) {
         const uint64_t roll = uniform(0, 9);
         if (roll < 7) {
            authority auth = random_authority();
            c.shared_authorities.emplace(std::piecewise_construct, std::forward_as_tuple(level),
                                         std::forward_as_tuple(auth));
            c.authorities.emplace(level, std::move(auth));
         } else if (roll == 7) {
            c.throwing_lookups.insert(level);
         }
      }
      for (const public_key_type& key : key_pool)
         if (chance(3, 10))
            c.provided_keys.insert(key);
      for (const permission_level& level : levels)
         if (chance(1, 8))
            c.provided_permissions.insert(level);
      // a provided permission with an empty name stands for every permission of its actor
      if (chance(1, 10))
         c.provided_permissions.insert({levels[uniform(0, levels.size() - 1)].actor, permission_name()});
      c.depth_limit = static_cast<uint16_t>(uniform(0, 5));
      return c;
   }

   random_query next_query() {
      random_query q;
      q.external_cache = chance(1, 2);
      if (chance(3, 4))
         q.level = levels[uniform(0, levels.size() - 1)];
      else
         q.auth = random_authority();
      return q;
   }

   uint64_t uniform(uint64_t lo, uint64_t hi) { return boost::random::uniform_int_distribution<uint64_t>(lo, hi)(rng); }
   bool     chance(uint64_t numerator, uint64_t denominator) { return uniform(1, denominator) <= numerator; }

private:
   authority random_authority() {
      authority auth;
      const uint64_t shape = uniform(0, 9); // 0-3 keys only, 4-5 accounts only, 6-9 mixed
      const bool     with_keys     = shape < 4 || shape >= 6;
      const bool     with_accounts = shape >= 4;
      const uint64_t key_count     = with_keys ? uniform(shape < 4 ? 1 : 0, max_keys_per_auth) : 0;
      const uint64_t account_count = with_accounts ? uniform(shape < 6 ? 1 : 0, max_accounts_per_auth) : 0;

      const bool allow_duplicates = chance(1, 10);
      std::vector<public_key_type> keys = key_pool;
      std::shuffle(keys.begin(), keys.end(), rng);
      for (uint64_t i = 0; i < key_count; ++i) {
         const public_key_type& key = allow_duplicates ? key_pool[uniform(0, key_pool.size() - 1)] : keys[i];
         auth.keys.push_back({key, 0});
      }
      std::vector<permission_level> accounts = levels;
      accounts.push_back({name("nobody"), config::active_name}); // never exists
      std::shuffle(accounts.begin(), accounts.end(), rng);
      for (uint64_t i = 0; i < account_count; ++i) {
         const permission_level& level = allow_duplicates ? levels[uniform(0, levels.size() - 1)] : accounts[i];
         auth.accounts.push_back({level, 0});
      }
      // validate() requires sorted entries; the walk must match on any declaration order
      if (chance(1, 2))
         auth.sort_fields();

      const weight_pattern pattern = static_cast<weight_pattern>(uniform(0, weight_pattern_count - 1));
      const uint16_t       base    = static_cast<uint16_t>(uniform(1, 3));
      const size_t         total   = auth.keys.size() + auth.accounts.size();
      // keys and accounts draw from one sequence so the patterns also cross the two lists
      std::vector<weight_type> weights(total);
      for (size_t i = 0; i < total; ++i) {
         switch (pattern) {
         case weight_pattern::equal:        weights[i] = base; break;
         case weight_pattern::ascending:    weights[i] = static_cast<weight_type>(i + 1); break;
         case weight_pattern::descending:   weights[i] = static_cast<weight_type>(total - i); break;
         case weight_pattern::small_random: weights[i] = static_cast<weight_type>(uniform(0, 3)); break;
         case weight_pattern::full_random:
            weights[i] = static_cast<weight_type>(uniform(1, std::numeric_limits<weight_type>::max()));
            break;
         }
      }
      if (chance(1, 2)) // start the shared sequence at the accounts instead of the keys
         std::rotate(weights.begin(), weights.begin() + auth.keys.size(), weights.end());
      uint32_t sum = 0;
      size_t   w   = 0;
      for (key_weight& k : auth.keys) {
         k.weight = weights[w++];
         sum += k.weight;
      }
      for (permission_level_weight& a : auth.accounts) {
         a.weight = weights[w++];
         sum += a.weight;
      }

      switch (uniform(0, 5)) {
      case 0:  auth.threshold = 1; break;
      case 1:  auth.threshold = sum; break;
      case 2:  auth.threshold = sum + 1; break;
      case 3:  auth.threshold = chance(1, 4) ? 0 : base; break;
      default: auth.threshold = static_cast<uint32_t>(uniform(1, std::max<uint32_t>(sum, 1))); break;
      }
      return auth;
   }

   boost::random::mt19937_64    rng{seed};
   std::vector<public_key_type> key_pool;
   std::vector<permission_level> levels;
};

/// Runs the queries of one case through the legacy and the current checker, requiring identical observable results.
template<typename AuthorityType>
void compare_checkers(const random_case& c, const std::map<permission_level, AuthorityType>& authorities,
                      const std::vector<random_query>& queries, size_t case_index) {
   auto lookup = [&](const permission_level& p) -> const AuthorityType* {
      if (c.throwing_lookups.count(p))
         SYS_THROW(permission_query_exception, "lookup of {} throws", p);
      auto itr = authorities.find(p);
      return itr == authorities.end() ? nullptr : &itr->second;
   };

   legacy::authority_checker<decltype(lookup)> reference(lookup, c.depth_limit, c.provided_keys,
                                                         c.provided_permissions);
   auto current = make_auth_checker(lookup, c.depth_limit, c.provided_keys, c.provided_permissions);

   typename legacy::authority_checker<decltype(lookup)>::permission_cache_type reference_cache;
   typename decltype(current)::permission_cache_type                           current_cache;

   for (size_t q = 0; q < queries.size(); ++q) {
      BOOST_TEST_CONTEXT("case " << case_index << " query " << q) {
         const random_query& query = queries[q];
         bool reference_result = false;
         bool current_result   = false;
         if (query.level) {
            reference_result = reference.satisfied(*query.level, query.external_cache ? &reference_cache : nullptr);
            current_result   = current.satisfied(*query.level, query.external_cache ? &current_cache : nullptr);
         } else if constexpr (std::is_same_v<AuthorityType, shared_authority>) {
            const shared_authority auth(query.auth);
            reference_result = reference.satisfied(auth, query.external_cache ? &reference_cache : nullptr);
            current_result   = current.satisfied(auth, query.external_cache ? &current_cache : nullptr);
         } else {
            reference_result = reference.satisfied(query.auth, query.external_cache ? &reference_cache : nullptr);
            current_result   = current.satisfied(query.auth, query.external_cache ? &current_cache : nullptr);
         }
         BOOST_REQUIRE_EQUAL(reference_result, current_result);
         BOOST_REQUIRE(reference.used_keys() == current.used_keys());
         BOOST_REQUIRE_EQUAL(reference.all_keys_used(), current.all_keys_used());
         BOOST_REQUIRE(cache_contents(reference_cache) == cache_contents(current_cache));
      }
   }
}

/// Number of randomized cases the differential test runs.
constexpr size_t differential_case_count = 20000;
/// Most queries run against one pair of checkers, sharing their used-key state and external cache.
constexpr uint64_t max_queries_per_case = 4;

/// Wall-clock budget for one walk of an authority near the transaction-size limit, with a wide margin for unoptimized
/// builds.
constexpr std::chrono::milliseconds large_walk_budget{500};

/// Authority entries a transaction of `max_trx_net_usage` bytes can carry, at `entry_size` bytes each, after a
/// margin for the transaction and action headers.
size_t entries_within_trx_size(uint32_t max_trx_net_usage, size_t entry_size) {
   constexpr size_t header_margin = 4096;
   return (max_trx_net_usage - header_margin) / entry_size;
}

/// Keys only, sorted, with weights ascending in declaration order.
authority make_ascending_key_authority(size_t key_count, uint16_t first_weight) {
   authority auth;
   for (const public_key_type& key : make_sorted_test_keys(key_count))
      auth.keys.push_back({key, static_cast<weight_type>(first_weight + auth.keys.size())});
   return auth;
}

/// Times one call of `walk`.
template<typename Walk>
std::chrono::milliseconds time_walk(Walk&& walk) {
   const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
   walk();
   return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace

BOOST_AUTO_TEST_SUITE(authority_checker_tests)

/// The sorted visit order reproduces the legacy flat_multimap walk on random authorities: keys-only and mixed,
/// equal, ascending and descending weights, every threshold kind, nested permissions with cycles, missing and
/// throwing lookups, the depth limit, and both fast paths.
BOOST_AUTO_TEST_CASE(matches_legacy_walk_on_random_authorities) {
   case_generator generator;
   for (size_t i = 0; i < differential_case_count; ++i) {
      const random_case         c = generator.next_case();
      std::vector<random_query> queries;
      const uint64_t            query_count = generator.uniform(1, max_queries_per_case);
      for (uint64_t q = 0; q < query_count; ++q)
         queries.push_back(generator.next_query());

      compare_checkers(c, c.authorities, queries, i);
      compare_checkers(c, c.shared_authorities, queries, i);
   }
}

/// Entries of equal weight are visited keys first, then in declaration order: the first declared provided key of
/// equal weight is the one marked used, and an account of equal weight is never evaluated once a key satisfies.
BOOST_AUTO_TEST_CASE(equal_weight_visit_order) {
   const std::vector<public_key_type> keys = make_sorted_test_keys(3);
   const permission_level             nested{name("alice"), config::active_name};
   const authority                    nested_auth(keys[2]);
   uint32_t                           lookups = 0;
   auto lookup = [&](const permission_level& p) -> const authority* {
      ++lookups;
      return p == nested ? &nested_auth : nullptr;
   };

   // threshold 2 keeps the walk off the keys-only fast paths
   const authority auth(2, {{keys[1], 2}, {keys[0], 2}}, {{nested, 2}});
   auto checker = make_auth_checker(lookup, 2, {keys[0], keys[1], keys[2]});
   BOOST_TEST(checker.satisfied(auth));
   BOOST_TEST(checker.used_keys() == flat_set<public_key_type>({keys[1]}));
   BOOST_TEST(lookups == 0u);
}

/// checktime runs once per authority evaluated and once every entries_per_checktime entries within one authority,
/// through the callable handed to make_auth_checker as a temporary.
BOOST_AUTO_TEST_CASE(checktime_per_authority_and_entries) {
   const std::vector<public_key_type> keys = make_sorted_test_keys(2);
   const permission_level             top{name("alice"), config::active_name};
   const permission_level             first{name("bob"), config::active_name};
   const permission_level             second{name("carol"), config::active_name};
   const authority                    top_auth(2, {}, {{first, 1}, {second, 1}});
   const authority                    first_auth(keys[0]);
   const authority                    second_auth(keys[1]);
   auto lookup = [&](const permission_level& p) -> const authority* {
      if (p == top)
         return &top_auth;
      if (p == first)
         return &first_auth;
      return p == second ? &second_auth : nullptr;
   };

   {
      uint32_t calls   = 0;
      auto     checker = make_auth_checker(lookup, 3, {keys[0], keys[1]}, {}, [&calls]() { ++calls; });
      BOOST_TEST(checker.satisfied(top));
      BOOST_TEST(calls == 3u); // top, first, second
   }

   constexpr size_t long_entry_count = 3 * detail::entries_per_checktime + 1;
   const uint32_t   expected_calls   = 1 + long_entry_count / detail::entries_per_checktime;
   authority        long_auth        = make_ascending_key_authority(long_entry_count, 1);
   {
      // general path: no key provided, so every entry is visited
      long_auth.threshold = 2;
      uint32_t calls      = 0;
      auto     checker    = make_auth_checker(lookup, 3, {}, {}, [&calls]() { ++calls; });
      BOOST_TEST(!checker.satisfied(long_auth));
      BOOST_TEST(calls == expected_calls);
   }
   {
      // keys-only threshold-1 fast path
      long_auth.threshold = 1;
      uint32_t calls      = 0;
      auto     checker    = make_auth_checker(lookup, 3, {}, {}, [&calls]() { ++calls; });
      BOOST_TEST(!checker.satisfied(long_auth));
      BOOST_TEST(calls == expected_calls);
   }
}

/// A checktime that throws aborts the walk and leaves no key of the unfinished authority marked used.
BOOST_AUTO_TEST_CASE(checktime_aborts_walk) {
   constexpr size_t entry_count = 2 * detail::entries_per_checktime;
   authority        auth        = make_ascending_key_authority(entry_count, 1);
   auth.threshold               = std::numeric_limits<uint32_t>::max();
   const flat_set<public_key_type> provided{auth.keys.front().key, auth.keys.back().key};
   auto lookup = [](const permission_level&) -> const authority* { return nullptr; };

   uint32_t calls = 0;
   auto checker = make_auth_checker(lookup, 2, provided, {}, [&calls]() {
      if (++calls == 2)
         SYS_THROW(deadline_exception, "deadline exceeded");
   });
   BOOST_CHECK_THROW(checker.satisfied(auth), deadline_exception);
   BOOST_TEST(calls == 2u);
   BOOST_TEST(checker.used_keys().empty());
}

/// An authority filling a transaction, with weights ascending in declaration order, is walked within budget: keys
/// only, and keys followed by the one account that satisfies it.
BOOST_AUTO_TEST_CASE(large_ascending_authority_walk) {
   const size_t key_entry_size = fc::raw::pack_size(key_weight{make_test_key(0), 1});
   const size_t key_count      = entries_within_trx_size(config::default_max_transaction_net_usage, key_entry_size);
   BOOST_TEST_MESSAGE("authority keys: " << key_count);

   const permission_level approver{name("bob"), config::active_name};
   auto lookup = [](const permission_level&) -> const authority* { return nullptr; };

   {
      // the provided key carries the lowest weight, so every key is visited and the threshold is never reached
      authority auth = make_ascending_key_authority(key_count, 1);
      auth.threshold = 2;
      const flat_set<public_key_type> provided{auth.keys.front().key};
      auto checker = make_auth_checker(lookup, 2, provided);
      bool satisfied = true;
      const std::chrono::milliseconds elapsed = time_walk([&]() { satisfied = checker.satisfied(auth); });
      BOOST_TEST_MESSAGE("keys-only walk: " << elapsed.count() << " ms");
      BOOST_TEST(!satisfied);
      BOOST_TEST(checker.used_keys().empty());
      BOOST_TEST(elapsed < large_walk_budget);
   }
   {
      // the satisfying account carries the lowest weight, so it is visited last
      authority auth = make_ascending_key_authority(key_count, 2);
      auth.threshold = 1;
      auth.accounts  = {{approver, 1}};
      auto checker   = make_auth_checker(lookup, 2, {}, {approver});
      bool satisfied = false;
      const std::chrono::milliseconds elapsed = time_walk([&]() { satisfied = checker.satisfied(auth); });
      BOOST_TEST_MESSAGE("mixed walk: " << elapsed.count() << " ms");
      BOOST_TEST(satisfied);
      BOOST_TEST(elapsed < large_walk_budget);
   }
}

BOOST_AUTO_TEST_SUITE_END()
