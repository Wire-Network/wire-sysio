#pragma once

#include <array>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fc {

template <typename K, typename T, std::size_t N>
class lut {
   std::array<std::pair<K, T>, N> elements_{};

public:
   lut()                            = delete;
   lut(const lut& other)            = default;
   lut(lut&& other)                 = default;
   lut& operator=(const lut& other) = default;
   lut& operator=(lut&& other)      = default;

   explicit constexpr lut(std::array<std::pair<K, T>, N> elements)
      : elements_(elements) {}

   template <typename... Args>
   explicit constexpr lut(Args&&... args)
      requires(sizeof...(Args) == N) && (std::is_convertible_v<Args, std::pair<K, T>> && ...)
      : lut(std::array<std::pair<K, T>, N>{std::forward<Args>(args)...}) {}

   [[nodiscard]] constexpr std::size_t size() const {
      return N;
   }

   constexpr const T& lookup(const K &key) const {
      for (const auto& elem : elements_) {
         if (key == elem.first) {
            return elem.second;
         }
      }
      // throw is allowed in constexpr if not reached.
      throw std::out_of_range("key not found in lut");
   }

   constexpr const T& operator[](const K &key) const {
      return lookup(key);
   }

   constexpr auto keys() const {
      std::array<K, N> k{};
      for (std::size_t i = 0; i < N; ++i) {
         k[i] = elements_[i].first;
      }
      return k;
   }

   constexpr auto values() const {
      std::array<T, N> v{};
      for (std::size_t i = 0; i < N; ++i) {
         v[i] = elements_[i].second;
      }
      return v;
   }
};

template <typename K, typename T, typename... Pairs>
lut(std::pair<K, T>, Pairs...) -> lut<K, T, 1 + sizeof...(Pairs)>;

template <typename K, typename T, std::size_t N>
lut(std::array<std::pair<K, T>, N>) -> lut<K, T, N>;

} // namespace fc
