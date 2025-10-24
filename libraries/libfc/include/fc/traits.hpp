#pragma once

#include <boost/type_index.hpp>
#include <string>
#include <array>
#include <string_view>
#include <iostream>

namespace fc {
template <typename T> struct pretty_type {
  std::string name() {
    // auto& info = boost::typeindex::type_id_with_cvr<T>().type_info();
    // auto id = boost::typeindex::type_id_with_cvr<T>();
    // std::string name = id.name();
    // return name;
    auto name = boost::core::demangle(typeid(this).name());
    auto offset = name.rfind("::");
    if (offset != std::string::npos)
      name.erase(0, offset + 2);
    name = name.substr(0, name.find('>'));
    return name;
  };
};

template <typename T> struct type_tag {
  using type = T;
};

template <typename T> struct get_second_type;

template <typename T, typename U>
struct get_second_type<std::pair<T, fc::type_tag<U>>> {
  using type = U;
};

template <typename Tuple> struct tuple_to_variant;

template <typename... Types> struct tuple_to_variant<std::tuple<Types...>> {
  using type =
      std::variant<typename Types::second_type::type...>;
};

template <class Tuple>
using tuple_pairs_to_variant_t = typename tuple_to_variant<std::remove_cvref_t<Tuple>>::type;


template <auto &Map, class K, std::size_t I = 0>
consteval auto get_type_tag_by_key(K key) {
  if constexpr (I == Map.size()) {
    static_assert(I != I, "message_type not found in map");
  } else if (Map[I].first == key) {
    return Map[I].second; // a type_tag<T>
  } else {
    return get_type_tag_by_key<Map, I + 1>(key);
  }
}

template <auto &Map, std::size_t I = 0, class K>
consteval auto get_value_by_key(K key) {
  if constexpr (I == Map.size()) {
    static_assert(I != I, "Key not found in value map");
  } else if (Map[I].first == key) {
    return Map[I].second; // value holder (see below)
  } else {
    return get_value_by_key<Map, I + 1>(key);
  }
}

} // namespace fc
