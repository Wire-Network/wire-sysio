// SPDX-License-Identifier: MIT
#pragma once

// Polyfill for std::expected / std::unexpected (C++23)
// libstdc++-14 ships the header but guards it behind __cplusplus >= 202302L,
// which clang-18 does not set even with -std=c++23/gnu++23.
// This header provides a minimal subset used in the codebase.

// Try the real header first; only use the polyfill when the feature is absent.
#if __has_include(<expected>)
#include <expected>
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
// std::expected is available — nothing else to do.
#else

#include <type_traits>
#include <utility>
#include <variant>

namespace std {

template <typename E>
class unexpected {
public:
   constexpr unexpected(const unexpected&) = default;
   constexpr unexpected(unexpected&&) = default;

   template <typename Err = E>
      requires(!std::is_same_v<std::remove_cvref_t<Err>, unexpected> &&
               !std::is_same_v<std::remove_cvref_t<Err>, std::in_place_t> &&
               std::is_constructible_v<E, Err>)
   constexpr explicit unexpected(Err&& e) : _val(std::forward<Err>(e)) {}

   constexpr const E& error() const& noexcept { return _val; }
   constexpr E& error() & noexcept { return _val; }
   constexpr const E&& error() const&& noexcept { return std::move(_val); }
   constexpr E&& error() && noexcept { return std::move(_val); }

private:
   E _val;
};

template <typename E>
unexpected(E) -> unexpected<E>;

template <typename T, typename E>
class expected {
public:
   using value_type = T;
   using error_type = E;
   using unexpected_type = unexpected<E>;

   // Value constructors
   constexpr expected() requires std::is_default_constructible_v<T>
      : _storage(T{}) {}

   constexpr expected(const expected&) = default;
   constexpr expected(expected&&) = default;

   template <typename U = T>
      requires(!std::is_same_v<std::remove_cvref_t<U>, expected> &&
               !std::is_same_v<std::remove_cvref_t<U>, unexpected<E>> &&
               std::is_constructible_v<T, U>)
   constexpr expected(U&& v) : _storage(std::forward<U>(v)) {}

   // Unexpected constructor
   constexpr expected(const unexpected<E>& u) : _storage(u) {}
   constexpr expected(unexpected<E>&& u) : _storage(std::move(u)) {}

   constexpr expected& operator=(const expected&) = default;
   constexpr expected& operator=(expected&&) = default;

   // Observers
   constexpr bool has_value() const noexcept { return std::holds_alternative<T>(_storage); }
   constexpr explicit operator bool() const noexcept { return has_value(); }

   constexpr T& value() & { return std::get<T>(_storage); }
   constexpr const T& value() const& { return std::get<T>(_storage); }
   constexpr T&& value() && { return std::get<T>(std::move(_storage)); }

   constexpr T& operator*() & noexcept { return std::get<T>(_storage); }
   constexpr const T& operator*() const& noexcept { return std::get<T>(_storage); }
   constexpr T&& operator*() && noexcept { return std::get<T>(std::move(_storage)); }

   constexpr T* operator->() noexcept { return &std::get<T>(_storage); }
   constexpr const T* operator->() const noexcept { return &std::get<T>(_storage); }

   constexpr const E& error() const& { return std::get<unexpected<E>>(_storage).error(); }
   constexpr E& error() & { return std::get<unexpected<E>>(_storage).error(); }
   constexpr E&& error() && { return std::get<unexpected<E>>(std::move(_storage)).error(); }

private:
   std::variant<T, unexpected<E>> _storage;
};

} // namespace std

#endif
