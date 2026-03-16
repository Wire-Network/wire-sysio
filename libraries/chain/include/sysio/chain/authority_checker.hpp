#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/parallel_markers.hpp>

#include <fc/scoped_exit.hpp>

#include <algorithm>
#include <boost/algorithm/cxx11/all_of.hpp>

#include <functional>

namespace sysio { namespace chain {

namespace detail {
   using meta_permission_key = std::tuple<uint32_t, int>;
   using meta_permission_value = std::function<uint32_t()>;
   using meta_permission_map = boost::container::flat_multimap<meta_permission_key, meta_permission_value, std::greater<>>;

} /// namespace detail

   /**
    * @brief This class determines whether a set of signing keys are sufficient to satisfy an authority or not
    *
    * To determine whether an authority is satisfied or not, we first determine which keys have approved of a message, and
    * then determine whether that list of keys is sufficient to satisfy the authority. This class takes a list of keys and
    * provides the @ref satisfied method to determine whether that list of keys satisfies a provided authority.
    *
    * @tparam F A callable which takes a single argument of type @ref AccountPermission and returns a pointer to
    * the corresponding authority if it exists, or a nullptr.
    */
   template<typename PermissionToAuthorityFunc>
   class authority_checker {
      private:
         PermissionToAuthorityFunc            permission_to_authority;
         const std::function<void()>&         checktime;
         vector<public_key_type>              provided_keys; // Making this a flat_set<public_key_type> causes runtime problems with utilities::filter_data_by_marker for some reason. TODO: Figure out why.
         flat_set<permission_level>           provided_permissions;
         vector<bool>                         _used_keys;
         uint16_t                             recursion_depth_limit;

      public:
         authority_checker( PermissionToAuthorityFunc            permission_to_authority,
                            uint16_t                             recursion_depth_limit,
                            const flat_set<public_key_type>&     provided_keys,
                            const flat_set<permission_level>&    provided_permissions,
                            const std::function<void()>&         checktime
                         )
         :permission_to_authority(permission_to_authority)
         ,checktime( checktime )
         ,provided_keys(provided_keys.begin(), provided_keys.end())
         ,provided_permissions(provided_permissions)
         ,_used_keys(provided_keys.size(), false)
         ,recursion_depth_limit(recursion_depth_limit)
         {
            SYS_ASSERT( static_cast<bool>(checktime), authorization_exception, "checktime cannot be empty" );
         }

         enum permission_cache_status {
            being_evaluated,
            permission_unsatisfied,
            permission_satisfied
         };

         typedef flat_map<permission_level, permission_cache_status> permission_cache_type;

         bool satisfied( const permission_level& permission, permission_cache_type* cached_perms = nullptr ) {
            permission_cache_type cached_permissions;

            if( cached_perms == nullptr )
               cached_perms = initialize_permission_cache( cached_permissions );

            weight_tally_visitor visitor(*this, *cached_perms, 0);
            return ( visitor(permission_level_weight{permission, 1}) > 0 );
         }

         template<typename AuthorityType>
         bool satisfied( const AuthorityType& authority, permission_cache_type* cached_perms = nullptr ) {
            permission_cache_type cached_permissions;

            if( cached_perms == nullptr )
               cached_perms = initialize_permission_cache( cached_permissions );

            return satisfied( authority, *cached_perms, 0 );
         }

         bool all_keys_used() const { return boost::algorithm::all_of_equal(_used_keys, true); }

         flat_set<public_key_type> used_keys() const {
            auto range = filter_data_by_marker(provided_keys, _used_keys, true);
            return {range.begin(), range.end()};
         }
         flat_set<public_key_type> unused_keys() const {
            auto range = filter_data_by_marker(provided_keys, _used_keys, false);
            return {range.begin(), range.end()};
         }

         static std::optional<permission_cache_status>
         permission_status_in_cache( const permission_cache_type& permissions,
                                     const permission_level& level )
         {
            auto itr = permissions.find( level );
            if( itr != permissions.end() )
               return itr->second;

            itr = permissions.find( {level.actor, permission_name()} );
            if( itr != permissions.end() )
               return itr->second;

            return std::optional<permission_cache_status>();
         }

      private:
         // Overloaded key resolution: no-op for public_key_type, converts for shared_public_key
         static const public_key_type& resolve_key(const public_key_type& k) { return k; }
         static public_key_type resolve_key(const shared_public_key& k) { return k.to_public_key(); }

         permission_cache_type* initialize_permission_cache( permission_cache_type& cached_permissions ) {
            for( const auto& p : provided_permissions ) {
               cached_permissions.emplace_hint( cached_permissions.end(), p, permission_satisfied );
            }
            return &cached_permissions;
         }

         template<typename AuthorityType>
         bool satisfied( const AuthorityType& authority, permission_cache_type& cached_permissions, uint16_t depth ) {
            // Fast paths for keys-only authorities (no account references, no recursion possible)
            // Avoids meta_permission_map allocation and rollback overhead entirely
            if( authority.accounts.empty() ) {
               // Single-key authority: direct check
               if( authority.keys.size() == 1 && authority.threshold <= authority.keys[0].weight ) {
                  const auto& search_key = resolve_key(authority.keys[0].key);
                  auto itr = std::lower_bound( provided_keys.begin(), provided_keys.end(), search_key );
                  if( itr != provided_keys.end() && *itr == search_key ) {
                     _used_keys[itr - provided_keys.begin()] = true;
                     return true;
                  }
                  return false;
               }
               // Threshold-1 multi-key (e.g. 1-of-3): find any matching key with sufficient weight
               // No rollback needed since we only mark a key when returning success
               if( authority.threshold == 1 ) {
                  for( const auto& k : authority.keys ) {
                     if( k.weight >= authority.threshold ) {
                        const auto& search_key = resolve_key(k.key);
                        auto itr = std::lower_bound( provided_keys.begin(), provided_keys.end(), search_key );
                        if( itr != provided_keys.end() && *itr == search_key ) {
                           _used_keys[itr - provided_keys.begin()] = true;
                           return true;
                        }
                     }
                  }
                  return false;
               }
            }

            // General case: save used keys state for rollback if this authority is not satisfied
            auto KeyReverter = fc::make_scoped_exit([this, keys = _used_keys] () mutable {
               _used_keys = keys;
            });

            weight_tally_visitor visitor(*this, cached_permissions, depth);

            // Sort key permissions and account permissions together into a single set of meta_permissions
            detail::meta_permission_map permissions;

            auto emplace_permission = [&permissions, &visitor](int priority, const auto& mp) {
               permissions.emplace(
                     std::make_tuple(mp.weight, priority),
                     [&mp, &visitor]() {
                        return visitor(mp);
                     }
               );
            };

            permissions.reserve(authority.keys.size() + authority.accounts.size());
            std::for_each(authority.accounts.begin(), authority.accounts.end(), std::bind(emplace_permission, 1, std::placeholders::_1));
            std::for_each(authority.keys.begin(), authority.keys.end(), std::bind(emplace_permission, 2, std::placeholders::_1));

            // Check all permissions, from highest weight to lowest, seeing if provided authorization factors satisfies them or not
            for( const auto& p: permissions )
               // If we've got enough weight, to satisfy the authority, return!
               if( p.second() >= authority.threshold ) {
                  KeyReverter.cancel();
                  return true;
               }
            return false;
         }

         struct weight_tally_visitor {
            using result_type = uint32_t;

            authority_checker&     checker;
            permission_cache_type& cached_permissions;
            uint16_t               recursion_depth;
            uint32_t               total_weight = 0;

            weight_tally_visitor(authority_checker& checker, permission_cache_type& cached_permissions, uint16_t recursion_depth)
            :checker(checker)
            ,cached_permissions(cached_permissions)
            ,recursion_depth(recursion_depth)
            {}

            template<typename KeyWeight, typename = std::enable_if_t<detail::is_any_of_v<KeyWeight, shared_key_weight, key_weight>>>
            uint32_t operator()(const KeyWeight& permission) {
               const auto& search_key = authority_checker::resolve_key(permission.key);
               auto itr = std::lower_bound( checker.provided_keys.begin(), checker.provided_keys.end(), search_key );
               if( itr != checker.provided_keys.end() && *itr == search_key ) {
                  checker._used_keys[itr - checker.provided_keys.begin()] = true;
                  total_weight += permission.weight;
               }
               return total_weight;
            }

            uint32_t operator()(const permission_level_weight& permission) {
               auto status = authority_checker::permission_status_in_cache( cached_permissions, permission.permission );
               if( !status ) {
                  if( recursion_depth < checker.recursion_depth_limit ) {
                     bool r = false;

                     std::invoke_result_t<decltype(checker.permission_to_authority), const permission_level> auth = nullptr;
                     try {
                        auth = checker.permission_to_authority( permission.permission );
                     }
                     catch( const permission_query_exception& ) {}
                     //permission was either invalid (threw permission_query_exception), or wasn't found
                     if(!auth)
                        return total_weight;

                     cached_permissions.emplace( permission.permission, being_evaluated );
                     r = checker.satisfied( *auth, cached_permissions, recursion_depth + 1 );

                     // Re-lookup after recursive call (flat_map iterators may be invalidated)
                     auto cache_itr = cached_permissions.find( permission.permission );
                     if( r ) {
                        total_weight += permission.weight;
                        cache_itr->second = permission_satisfied;
                     } else {
                        cache_itr->second = permission_unsatisfied;
                     }
                  }
               } else if( *status == permission_satisfied ) {
                  total_weight += permission.weight;
               }
               return total_weight;
            }
         };

   }; /// authority_checker

   template<typename PermissionToAuthorityFunc>
   auto make_auth_checker( PermissionToAuthorityFunc&&          pta,
                           uint16_t                             recursion_depth_limit,
                           const flat_set<public_key_type>&     provided_keys,
                           const flat_set<permission_level>&    provided_permissions = flat_set<permission_level>(),
                           const std::function<void()>&         _checktime = std::function<void()>()
                         )
   {
      auto noop_checktime = []() {};
      const auto& checktime = ( static_cast<bool>(_checktime) ? _checktime : noop_checktime );
      return authority_checker< PermissionToAuthorityFunc>( std::forward<PermissionToAuthorityFunc>(pta),
                                                            recursion_depth_limit,
                                                            provided_keys,
                                                            provided_permissions,
                                                            checktime );
   }

} } // namespace sysio::chain
