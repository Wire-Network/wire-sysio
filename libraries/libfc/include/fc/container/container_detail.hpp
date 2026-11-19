#pragma once

#include <fc/variant.hpp>
#include <fc/io/raw_fwd.hpp>
#include <fc/io/raw_detail.hpp>

#include <boost/container/container_fwd.hpp>

#include <algorithm>
#include <concepts>
#include <utility>

namespace fc {

   namespace detail {

      /// Whether `Container` holds at most one element per key: its insert reports whether it inserted.
      template<typename Container>
      inline constexpr bool has_unique_keys_v = requires( Container& c, typename Container::value_type&& v ) {
         { c.insert( std::move( v ) ) } -> std::same_as<std::pair<typename Container::iterator, bool>>;
      };

      /**
       * Replaces the contents of the flat container `c` with the elements `append` adds to the sequence it is given.
       *
       * The result equals inserting the elements one at a time in the order `append` adds them: equivalent elements
       * keep that order, and a unique-key container keeps the first of them. The sequence is sorted once rather than
       * shifted for each element that arrives out of order. If `append` throws, `c` is left empty, keeping its storage.
       */
      template<typename Container, typename Append>
      void assign_flat_unsorted( Container& c, Append&& append ) {
         typename Container::sequence_type seq = c.extract_sequence();
         seq.clear();
         try {
            append( seq );
         } catch( ... ) {
            seq.clear();
            c.adopt_sequence( boost::container::ordered_unique_range, std::move( seq ) );
            throw;
         }
         const typename Container::value_compare comp = c.value_comp();
         std::stable_sort( seq.begin(), seq.end(), comp );
         if constexpr( has_unique_keys_v<Container> ) {
            // Sorted, an element is equivalent to the last one kept exactly when it does not order after it.
            const auto equivalent = [&comp]( const auto& kept, const auto& next ) { return !comp( kept, next ); };
            seq.erase( std::unique( seq.begin(), seq.end(), equivalent ), seq.end() );
            c.adopt_sequence( boost::container::ordered_unique_range, std::move( seq ) );
         } else {
            c.adopt_sequence( boost::container::ordered_range, std::move( seq ) );
         }
      }

   }

   namespace raw {

      namespace detail {

         template<template<typename...> class Set, typename Stream, typename T, typename... U>
         inline void pack_set( Stream& s, const Set<T, U...>& value ) {
            FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
            pack( s, unsigned_int((uint32_t)value.size()) );
            for( const auto& item : value ) {
               pack( s, item );
            }
         }

         template<template<typename...> class Set, typename Stream, typename T, typename... U>
         inline void unpack_set( Stream& s, Set<T, U...>& value ) {
            unsigned_int size; unpack( s, size );
            FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
            value.clear();
            for( uint32_t i = 0; i < size.value; ++i ) {
               auto tmp = ::fc::detail::construct_maybe_with_allocator<T>( value.get_allocator() );
               unpack( s, tmp );
               value.insert( value.end(), std::move(tmp) );
            }
         }

         /// Reserve small sets; otherwise grow storage as elements are decoded. Elements are sorted once decoded.
         template<template<typename...> class Set, typename Stream, typename T, typename... U>
         inline void unpack_flat_set( Stream& s, Set<T, U...>& value ) {
            unsigned_int size; unpack( s, size );
            FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
            ::fc::detail::assign_flat_unsorted( value, [&]( auto& seq ) {
               if( is_small_allocation<Set<T, U...>>( size.value ) )
                  seq.reserve( size.value );
               for( uint32_t i = 0; i < size.value; ++i ) {
                  T tmp = ::fc::detail::construct_maybe_with_allocator<T>( value.get_allocator() );
                  unpack( s, tmp );
                  seq.emplace_back( std::move(tmp) );
               }
            } );
         }

         template<template<typename...> class Map, typename Stream, typename K, typename V, typename... U>
         inline void pack_map( Stream& s, const Map<K, V, U...>& value ) {
            FC_ASSERT( value.size() <= MAX_NUM_ARRAY_ELEMENTS );
            pack( s, unsigned_int((uint32_t)value.size()) );
            for( const auto& item : value ) {
               pack( s, item );
            }
         }

         template<template<typename...> class Map, typename Stream, typename K, typename V, typename... U>
         inline void unpack_map( Stream& s, Map<K, V, U...>& value ) {
            unsigned_int size; unpack( s, size );
            FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
            value.clear();
            for( uint32_t i = 0; i < size.value; ++i ) {
               auto tmp = ::fc::detail::default_construct_pair_maybe_with_allocator<K, V>( value.get_allocator() );
               unpack( s, tmp );
               value.insert( value.end(), std::move(tmp) );
            }
         }

         /// Reserve small maps; otherwise grow storage as entries are decoded. Entries are sorted once decoded.
         template<template<typename...> class Map, typename Stream, typename K, typename V, typename... U>
         inline void unpack_flat_map( Stream& s, Map<K, V, U...>& value ) {
            unsigned_int size; unpack( s, size );
            FC_ASSERT( size.value <= MAX_NUM_ARRAY_ELEMENTS );
            ::fc::detail::assign_flat_unsorted( value, [&]( auto& seq ) {
               if( is_small_allocation<Map<K, V, U...>>( size.value ) )
                  seq.reserve( size.value );
               for( uint32_t i = 0; i < size.value; ++i ) {
                  std::pair<K, V> tmp =
                     ::fc::detail::default_construct_pair_maybe_with_allocator<K, V>( value.get_allocator() );
                  unpack( s, tmp );
                  seq.emplace_back( std::move(tmp) );
               }
            } );
         }

      }

   }

   namespace detail {

      template<template<typename...> class Set, typename T, typename... U>
      void to_variant_from_set( const Set<T, U...>& s, fc::variant& vo ) {
         FC_ASSERT( s.size() <= MAX_NUM_ARRAY_ELEMENTS );
         variants vars;
         vars.reserve( s.size() );
         for( const auto& item : s ) {
            vars.emplace_back( item );
         }
         vo = std::move( vars );
      }

      template<template<typename...> class Set, typename T, typename... U>
      void from_variant_to_set( const fc::variant& v, Set< T, U... >& s ) {
         const variants& vars = v.get_array();
         FC_ASSERT( vars.size() <= MAX_NUM_ARRAY_ELEMENTS );
         s.clear();
         for( const auto& var : vars ) {
            auto item = construct_maybe_with_allocator<T>( s.get_allocator() );
            var.as<T>( item );
            s.insert( s.end(), std::move(item) );
         }
      }

      /// Elements are sorted once read.
      template<template<typename...> class Set, typename T, typename... U>
      void from_variant_to_flat_set( const fc::variant& v, Set< T, U... >& s ) {
         const variants& vars = v.get_array();
         FC_ASSERT( vars.size() <= MAX_NUM_ARRAY_ELEMENTS );
         assign_flat_unsorted( s, [&]( auto& seq ) {
            seq.reserve( vars.size() );
            for( const auto& var : vars ) {
               T item = construct_maybe_with_allocator<T>( s.get_allocator() );
               var.as<T>( item );
               seq.emplace_back( std::move(item) );
            }
         } );
      }

      template<template<typename...> class Map, typename K, typename V, typename... U >
      void to_variant_from_map( const Map< K, V, U... >& m, fc::variant& vo ) {
         FC_ASSERT( m.size() <= MAX_NUM_ARRAY_ELEMENTS );
         variants vars;
         vars.reserve( m.size() );
         for( const auto& item : m ) {
            vars.emplace_back( item );
         }
         vo = std::move( vars );
      }

      template<template<typename...> class Map, typename K, typename V, typename... U>
      void from_variant_to_map( const variant& v, Map<K, V, U...>& m ) {
         const variants& vars = v.get_array();
         FC_ASSERT( vars.size() <= MAX_NUM_ARRAY_ELEMENTS );
         m.clear();
         for( const auto& var : vars ) {
            auto item = default_construct_pair_maybe_with_allocator<K, V>( m.get_allocator() );
            var.as< std::pair<K,V> >( item );
            m.insert( m.end(), std::move(item) );
         }
      }

      /// Entries are sorted once read.
      template<template<typename...> class Map, typename K, typename V, typename... U>
      void from_variant_to_flat_map( const variant& v, Map<K, V, U...>& m ) {
         const variants& vars = v.get_array();
         FC_ASSERT( vars.size() <= MAX_NUM_ARRAY_ELEMENTS );
         assign_flat_unsorted( m, [&]( auto& seq ) {
            seq.reserve( vars.size() );
            for( const auto& var : vars ) {
               std::pair<K, V> item = default_construct_pair_maybe_with_allocator<K, V>( m.get_allocator() );
               var.as< std::pair<K,V> >( item );
               seq.emplace_back( std::move(item) );
            }
         } );
      }

   }

}
