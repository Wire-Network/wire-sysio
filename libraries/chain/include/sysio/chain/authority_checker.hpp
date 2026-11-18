#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/authority.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/parallel_markers.hpp>

#include <fc/scoped_exit.hpp>

#include <boost/algorithm/cxx11/all_of.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace sysio { namespace chain {

namespace detail {
   /// The list of an authority that an @ref authority_entry indexes.
   enum class authority_entry_kind : uint8_t { key, account };

   /// One key or account entry of an authority, identified by its declaration index within its list.
   struct authority_entry {
      std::size_t          index  = 0;
      weight_type          weight = 0;
      authority_entry_kind kind   = authority_entry_kind::key;
   };

   /**
    * @brief Strict total order in which @ref authority_checker visits the entries of an authority
    *
    * Weight descending, then keys before accounts, then declaration index ascending. The visit order decides which
    * provided keys are marked used and which nested permissions are evaluated and cached, so it is part of consensus.
    */
   inline bool authority_entry_visited_before( const authority_entry& lhs, const authority_entry& rhs ) {
      if( lhs.weight != rhs.weight )
         return lhs.weight > rhs.weight;
      if( lhs.kind != rhs.kind )
         return lhs.kind == authority_entry_kind::key;
      return lhs.index < rhs.index;
   }

   /// The key and account entries of @p authority in the order @ref authority_entry_visited_before defines.
   template<typename AuthorityType>
   std::vector<authority_entry> authority_visit_order( const AuthorityType& authority ) {
      std::vector<authority_entry> entries;
      entries.reserve( authority.keys.size() + authority.accounts.size() );
      for( std::size_t i = 0; i < authority.keys.size(); ++i )
         entries.push_back( { i, authority.keys[i].weight, authority_entry_kind::key } );
      for( std::size_t i = 0; i < authority.accounts.size(); ++i )
         entries.push_back( { i, authority.accounts[i].weight, authority_entry_kind::account } );
      std::sort( entries.begin(), entries.end(), authority_entry_visited_before );
      return entries;
   }

   /// Entries of one authority visited between consecutive calls to the checktime callable.
   inline constexpr std::size_t entries_per_checktime = 256;

} /// namespace detail

   /**
    * @brief This class determines whether a set of signing keys are sufficient to satisfy an authority or not
    *
    * To determine whether an authority is satisfied or not, we first determine which keys have approved of a message, and
    * then determine whether that list of keys is sufficient to satisfy the authority. This class takes a list of keys and
    * provides the @ref satisfied method to determine whether that list of keys satisfies a provided authority.
    *
    * The checktime callable is called once per authority evaluated and once every
    * @ref detail::entries_per_checktime entries visited within one authority.
    *
    * @tparam F A callable which takes a single argument of type @ref AccountPermission and returns a pointer to
    * the corresponding authority if it exists, or a nullptr.
    */
   template<typename PermissionToAuthorityFunc>
   class authority_checker {
      private:
         PermissionToAuthorityFunc            permission_to_authority;
         std::function<void()>                checktime;
         vector<public_key_type>              provided_keys; // Making this a flat_set<public_key_type> causes runtime problems with utilities::filter_data_by_marker for some reason. TODO: Figure out why.
         flat_set<permission_level>           provided_permissions;
         vector<bool>                         _used_keys;
         uint16_t                             recursion_depth_limit;

      public:
         /**
          * @param checktime called during the walk to enforce the caller's deadline; throws to abort the walk. Must
          *                  not be empty.
          */
         authority_checker( PermissionToAuthorityFunc            permission_to_authority,
                            uint16_t                             recursion_depth_limit,
                            const flat_set<public_key_type>&     provided_keys,
                            const flat_set<permission_level>&    provided_permissions,
                            std::function<void()>                checktime
                         )
         :permission_to_authority(permission_to_authority)
         ,checktime( std::move(checktime) )
         ,provided_keys(provided_keys.begin(), provided_keys.end())
         ,provided_permissions(provided_permissions)
         ,_used_keys(provided_keys.size(), false)
         ,recursion_depth_limit(recursion_depth_limit)
         {
            SYS_ASSERT( static_cast<bool>(this->checktime), authorization_exception, "checktime cannot be empty" );
         }

         enum permission_cache_status {
            being_evaluated,
            permission_unsatisfied,
            permission_satisfied
         };

         /// Evaluation status of each permission reached by a walk, plus the provided permissions.
         using permission_cache_type = std::map<permission_level, permission_cache_status>;

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
         // Overloaded key resolution: returns reference for public_key_type, value for shared_public_key.
         // Callers bind with `const auto&` which either aliases or extends the temporary's lifetime.
         static const public_key_type& resolve_key(const public_key_type& k) { return k; }
         static public_key_type resolve_key(const shared_public_key& k) { return k.to_public_key(); }

         permission_cache_type* initialize_permission_cache( permission_cache_type& cached_permissions ) {
            for( const auto& p : provided_permissions ) {
               cached_permissions.emplace_hint( cached_permissions.end(), p, permission_satisfied );
            }
            return &cached_permissions;
         }

         /// Calls checktime after every @ref detail::entries_per_checktime entries visited within one authority.
         void checktime_after_entry( std::size_t entries_visited ) const {
            if( entries_visited % detail::entries_per_checktime == 0 )
               checktime();
         }

         /**
          * Evaluates @p authority at nesting level @p depth. Entries are visited in the order of
          * @ref detail::authority_entry_visited_before until their weight reaches the threshold. The keys marked used
          * while evaluating an authority that is not satisfied are unmarked again.
          */
         template<typename AuthorityType>
         bool satisfied( const AuthorityType& authority, permission_cache_type& cached_permissions, uint16_t depth ) {
            checktime();

            // Fast paths for keys-only authorities (no account references, no recursion possible)
            // Avoid the visit order allocation and rollback overhead entirely
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
               // Threshold-1 multi-key (e.g. 1-of-3): find a matching key with sufficient weight.
               // No rollback needed since we only mark a key when returning success.
               // The general path visits keys in descending weight order (declaration
               // order for equal weights), so when several provided keys could satisfy
               // the authority it marks the highest-weight one as used. Select the same
               // key here so the used/unused key sets are identical no matter which path
               // evaluates the authority.
               if( authority.threshold == 1 ) {
                  auto best = provided_keys.end();
                  uint32_t best_weight = 0;
                  for( std::size_t i = 0; i < authority.keys.size(); ++i ) {
                     const auto& k = authority.keys[i];
                     // only a strictly greater weight can replace the current best,
                     // keeping the earliest-declared key among equal weights
                     if( k.weight >= authority.threshold && k.weight > best_weight ) {
                        const auto& search_key = resolve_key(k.key);
                        auto itr = std::lower_bound( provided_keys.begin(), provided_keys.end(), search_key );
                        if( itr != provided_keys.end() && *itr == search_key ) {
                           best = itr;
                           best_weight = k.weight;
                        }
                     }
                     checktime_after_entry( i + 1 );
                  }
                  if( best != provided_keys.end() ) {
                     _used_keys[best - provided_keys.begin()] = true;
                     return true;
                  }
                  return false;
               }
            }

            // General case: save used keys state for rollback if this authority is not satisfied
            auto KeyReverter = fc::make_scoped_exit([this, keys = _used_keys] () mutable {
               _used_keys = keys;
            });

            weight_tally_visitor visitor(*this, cached_permissions, depth);

            // Check all permissions, from highest weight to lowest, seeing if provided authorization factors satisfies them or not
            const std::vector<detail::authority_entry> entries = detail::authority_visit_order( authority );
            for( std::size_t i = 0; i < entries.size(); ++i ) {
               const detail::authority_entry& entry = entries[i];
               const uint32_t total_weight = entry.kind == detail::authority_entry_kind::key
                                                ? visitor( authority.keys[entry.index] )
                                                : visitor( authority.accounts[entry.index] );
               // If we've got enough weight, to satisfy the authority, return!
               if( total_weight >= authority.threshold ) {
                  KeyReverter.cancel();
                  return true;
               }
               checktime_after_entry( i + 1 );
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
                     std::invoke_result_t<decltype(checker.permission_to_authority), const permission_level> auth = nullptr;
                     try {
                        auth = checker.permission_to_authority( permission.permission );
                     }
                     catch( const permission_query_exception& ) {}
                     //permission was either invalid (threw permission_query_exception), or wasn't found
                     if(!auth)
                        return total_weight;

                     // map iterators stay valid across the insertions of the nested evaluation
                     const auto cache_itr = cached_permissions.emplace( permission.permission, being_evaluated ).first;
                     const bool r = checker.satisfied( *auth, cached_permissions, recursion_depth + 1 );

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

   /**
    * Creates an @ref authority_checker. An empty @p checktime is replaced by a callable that does nothing.
    */
   template<typename PermissionToAuthorityFunc>
   auto make_auth_checker( PermissionToAuthorityFunc&&          pta,
                           uint16_t                             recursion_depth_limit,
                           const flat_set<public_key_type>&     provided_keys,
                           const flat_set<permission_level>&    provided_permissions = flat_set<permission_level>(),
                           std::function<void()>                checktime = std::function<void()>()
                         )
   {
      if( !checktime )
         checktime = []() {};
      return authority_checker< PermissionToAuthorityFunc>( std::forward<PermissionToAuthorityFunc>(pta),
                                                            recursion_depth_limit,
                                                            provided_keys,
                                                            provided_permissions,
                                                            std::move(checktime) );
   }

} } // namespace sysio::chain
