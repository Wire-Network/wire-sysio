#pragma once

#include <string>
#include <array>
#include <string_view>
#include <tuple>

#include <algorithm>
#include <array>
#include <functional>
#include <stdexcept>
#include <utility>
#include <utility>

namespace fc {
template<typename T, typename K>
using lut_key_getter = std::function<K(T&)>;

template <typename K, typename T, std::size_t N>
class lut {
   std::array<std::pair<K, T>, N> elements_{};
   std::array<T,N> values_{};
   std::array<K,N> keys_{};

public:
   lut()                            = delete;
   lut(const lut& other)            = delete;
   lut(lut&& other)                 = delete;
   lut& operator=(const lut& other) = delete;
   lut& operator=(lut&& other)      = delete;


   constexpr lut(std::initializer_list<std::pair<K, T>> elements) {
      std::move(elements.begin(), elements.end(), elements_.begin());

      std::transform(elements_.begin(),elements_.end(),values_.begin(), [&] (auto& elem) {return std::get<1>(elem);});
      std::transform(elements_.begin(),elements_.end(),keys_.begin(), [&] (auto& elem) {return std::get<0>(elem);});

   }

   [[nodiscard]] constexpr std::size_t size() const {
      return N;
   }

   constexpr T lookup(const K &key) const {
      for (auto &[testKey, value] : elements_) {
         if (key == testKey) {
            return value;
         }
      }

      // static_assert(1 != 1, "key not found");
      // throw std::range_error("key not found");
   }

   constexpr T operator[](const K &key) const {
      return lookup(key);
   }

   constexpr const std::array<K,N>& keys() const {
      return keys_;
   }

   constexpr const std::array<T,N>& values() const {
      return values_;
   }


};
} // namespace fc
