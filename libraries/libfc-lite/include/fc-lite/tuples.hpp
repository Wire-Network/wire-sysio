#pragma once

#include <string>
#include <array>
#include <tuple>
#include <type_traits>
#include <memory>
#include <utility>
#include <variant>

namespace fc {

template <typename T>
struct type_tag {
   using type = T;

   T create() {
      return T{};
   }

   std::shared_ptr<T> create_shared() {
      return std::make_shared<T>();
   }

   std::unique_ptr<T> create_unique() {
      return std::make_shared<T>();
   }
};


template <typename Tuple>
struct tuple_to_variant;

template <typename... Types>
struct tuple_to_variant<std::tuple<Types...>> {
   using type = std::variant<typename Types::second_type::type...>;
};

template <class Tuple>
using tuple_pairs_to_variant_t = typename tuple_to_variant<std::remove_cvref_t<Tuple>>::type;


template <auto& mapping, typename TypeEnum, TypeEnum Type, std::size_t Index = 0>
constexpr auto find_mapped_type() {
   if constexpr (Index < std::tuple_size_v<std::decay_t<decltype(mapping)>>) {
      constexpr auto pair = std::get<Index>(mapping);
      if constexpr (std::get<0>(pair) == Type) {
         constexpr auto tt = std::get<1>(pair);
         return typename decltype(tt)::type{};
      } else {
         return find_mapped_type<mapping, TypeEnum, Type, Index + 1>();
      }
   }
}

} // namespace fc
