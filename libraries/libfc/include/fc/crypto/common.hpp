#pragma once
#include <fc/crypto/ripemd160.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/crypto/crypto_utils.hpp>
#include <fc/io/raw.hpp>
#include <fc/utility.hpp>
#include <fc/static_variant.hpp>

#include <concepts>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <cstring>

namespace fc { namespace crypto {

   template<typename T>
   struct eq_comparator {
      static bool apply(const T& a, const T& b) {
         return a.serialize() == b.serialize();
      }
   };

   template<typename ... Ts>
   struct eq_comparator<std::variant<Ts...>> {
      using variant_type = std::variant<Ts...>;
      struct visitor : public fc::visitor<bool> {
         explicit visitor(const variant_type &b)
            : _b(b) {}

         template<typename KeyType>
         bool operator()(const KeyType &a) const {
            const auto &b = std::get<KeyType>(_b);
            return eq_comparator<KeyType>::apply(a,b);
         }

         const variant_type &_b;
      };

      static bool apply(const variant_type& a, const variant_type& b) {
         return a.index() == b.index() && std::visit(visitor(b), a);
      }
   };

   // for specializations that do not meet SerializableForMemcmp concept
   template<typename T>
   struct less_comparator {};

   template<typename T>
   concept SerializableForMemcmp = requires(const T& obj) {
      { obj.serialize() } -> std::ranges::forward_range;
      { obj.serialize().data() } -> std::contiguous_iterator; // Ensures data is contiguous for memcmp
      { obj.serialize().size() } -> std::convertible_to<std::size_t>;
   } && std::is_trivially_copyable_v<std::ranges::range_value_t<decltype(std::declval<const T&>().serialize())>>;

   template<SerializableForMemcmp T>
   struct less_comparator<T> {
      static bool apply(const T& a, const T& b) {
         const auto& lhs = a.serialize();
         const auto& rhs = b.serialize();

         using V = std::ranges::range_value_t<decltype(lhs)>;

         if (lhs.size() != rhs.size()) {
            return lhs.size() < rhs.size();
         }

         return std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(V)) < 0;
      }
   };

   template<typename ... Ts>
   struct less_comparator<std::variant<Ts...>> {
      using variant_type = std::variant<Ts...>;
      struct visitor : public fc::visitor<bool> {
         visitor(const variant_type &b)
            : _b(b) {}

         template<typename KeyType>
         bool operator()(const KeyType &a) const {
            const auto &b = std::template get<KeyType>(_b);
            return less_comparator<KeyType>::apply(a,b);
         }

         const variant_type &_b;
      };

      static bool apply(const variant_type& a, const variant_type& b) {
         return a.index() < b.index() || (a.index() == b.index() && std::visit(visitor(b), a));
      }
   };

   template<typename Data>
   struct shim {
      using data_type = Data;

      shim()
      {}

      shim(data_type&& data)
         :_data(std::move(data))
      {}

      shim(const data_type& data)
      :_data(data)
      {}

      const data_type& serialize() const {
         return _data;
      }

      data_type _data{};
   };

} }

FC_REFLECT_TEMPLATE((typename T), fc::crypto::checksum_data<T>, (data)(check) )
FC_REFLECT_TEMPLATE((typename T), fc::crypto::shim<T>, (_data) )
