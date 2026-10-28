#pragma once

/**
 * @file fc/move_only_function.hpp
 * @brief `fc::move_only_function<Signature>`: std::move_only_function where the standard
 *        library provides it, otherwise a type-erased polyfill.
 *
 * libc++ versions shipped with current AppleClang do not provide the C++23
 * std::move_only_function API, so portable code uses this alias instead of naming the
 * std template directly.
 *
 * The polyfill supports the subset the tree relies on: construction from any callable
 * invocable with the signature (including move-only captures), move-only semantics,
 * `explicit operator bool`, and a non-const `operator()`.  cv/ref/noexcept-qualified
 * signatures are not implemented.
 */

#include <version>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace fc {

namespace detail {

   template<typename Signature>
   class move_only_function_impl;

   template<typename R, typename... Args>
   class move_only_function_impl<R(Args...)> {
   public:
      move_only_function_impl() = default;
      move_only_function_impl(std::nullptr_t) noexcept {}

      template<typename F>
         requires (!std::is_same_v<std::decay_t<F>, move_only_function_impl> &&
                   std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
      move_only_function_impl(F&& f)
         : _callable(std::make_unique<callable<std::decay_t<F>>>(std::forward<F>(f))) {}

      move_only_function_impl(move_only_function_impl&&) noexcept = default;
      move_only_function_impl& operator=(move_only_function_impl&&) noexcept = default;
      move_only_function_impl(const move_only_function_impl&) = delete;
      move_only_function_impl& operator=(const move_only_function_impl&) = delete;

      R operator()(Args... args) { return _callable->invoke(std::forward<Args>(args)...); }
      explicit operator bool() const noexcept { return static_cast<bool>(_callable); }

   private:
      struct callable_base {
         virtual ~callable_base() = default;
         virtual R invoke(Args... args) = 0;
      };

      template<typename F>
      struct callable final : callable_base {
         template<typename Fn>
         explicit callable(Fn&& fn)
            : f(std::forward<Fn>(fn)) {}

         R invoke(Args... args) override { return f(std::forward<Args>(args)...); }

         F f;
      };

      std::unique_ptr<callable_base> _callable;
   };

} // namespace detail

template<typename Signature>
using move_only_function =
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
   std::move_only_function<Signature>;
#else
   detail::move_only_function_impl<Signature>;
#endif

} // namespace fc
