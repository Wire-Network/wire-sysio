#pragma once

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <type_traits>

namespace fc {
template <class Items, class ToErase>
std::size_t erase_all(Items& items, const ToErase& to_erase) {
   using value_t = std::remove_cvref_t<decltype(*std::begin(items))>;

   std::unordered_set<value_t> kill;
   kill.reserve(std::ranges::size(to_erase));
   for (const auto& v : to_erase) kill.insert(v);

   const auto old_size = std::ranges::size(items);

   if constexpr (requires { items.remove_if([](const value_t&) { return false; }); }) {
      // list/forward_list-like
      items.remove_if([&](const value_t& x) { return kill.contains(x); });
   } else {
      // vector/deque/string-like: erase-remove
      auto it = std::remove_if(std::begin(items), std::end(items),
                               [&](const value_t& x) { return kill.contains(x); });
      items.erase(it, std::end(items));
   }

   return old_size - std::ranges::size(items);
}

template <class Map, class Keys>
std::size_t erase_all_keys(Map& m, const Keys& keys) {
   using key_t = typename Map::key_type;

   std::size_t removed = 0;

   // If Keys holds elements convertible to key_type, erase directly.
   if constexpr (requires { static_cast<key_t>(*std::begin(keys)); }) {
      for (const auto& k : keys) {
         removed += static_cast<std::size_t>(m.erase(static_cast<key_t>(k)));
      }
   } else {
      // Otherwise, try erasing by transparent/heterogeneous key type directly.
      // (Works if Map::erase accepts the key's type.)
      for (const auto& k : keys) {
         removed += static_cast<std::size_t>(m.erase(k));
      }
   }

   return removed;
}

template<std::ranges::input_range R, class T>
bool contains(const R& range, const T& value) {
   return std::ranges::find(range, value) != std::ranges::end(range);
}
}