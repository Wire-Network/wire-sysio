#pragma once

#include <fc/exception/exception.hpp>
#include <fc/scoped_exit.hpp>

#include <cstddef>
#include <optional>

#include <pthread.h>
#include <sys/resource.h>

namespace fc {

/// Bytes in the calling thread's stack.
/// @throws fc::assert_exception when the stack cannot be queried.
inline std::size_t current_thread_stack_size() {
#if defined(__APPLE__)
   return pthread_get_stacksize_np(pthread_self());
#else
   pthread_attr_t attributes;
   if (pthread_getattr_np(pthread_self(), &attributes) != 0)
      FC_THROW_EXCEPTION(fc::assert_exception, "Failed to read the thread's attributes");
   auto destroy = fc::make_scoped_exit([&attributes]() { pthread_attr_destroy(&attributes); });
   std::size_t size = 0;
   if (pthread_attr_getstacksize(&attributes, &size) != 0)
      FC_THROW_EXCEPTION(fc::assert_exception, "Failed to read the thread's stack size");
   return size;
#endif
}

/// The soft RLIMIT_STACK to set so the main thread's stack can grow to `required` bytes under `limits`, or empty when
/// the soft limit already allows it.
/// @throws fc::assert_exception when the hard limit is below `required`.
inline std::optional<rlim_t> raised_soft_stack_limit(const rlimit& limits, rlim_t required) {
   if (limits.rlim_cur == RLIM_INFINITY || limits.rlim_cur >= required)
      return {};
   constexpr rlim_t bytes_per_kib = 1024;
   if (limits.rlim_max != RLIM_INFINITY && limits.rlim_max < required)
      FC_THROW_EXCEPTION(fc::assert_exception, "The stack hard limit (ulimit -Hs) is {} KiB, below the {} KiB required",
                         limits.rlim_max / bytes_per_kib, required / bytes_per_kib);
   return required;
}

/// Raises the soft RLIMIT_STACK so the main thread's stack can grow to `required` bytes.
///
/// Linux grows the main thread's stack up to the soft limit in force when it grows, so raising it at startup is enough.
/// Other platforms size the main thread's stack when the process starts; there this does nothing.
/// @throws fc::assert_exception when the hard limit is below `required`, or the limit cannot be read or set.
inline void raise_main_thread_stack_limit([[maybe_unused]] rlim_t required) {
#if defined(__linux__)
   rlimit limits{};
   if (getrlimit(RLIMIT_STACK, &limits) != 0)
      FC_THROW_EXCEPTION(fc::assert_exception, "Failed to read the stack limit");
   if (const std::optional<rlim_t> soft = raised_soft_stack_limit(limits, required)) {
      limits.rlim_cur = *soft;
      if (setrlimit(RLIMIT_STACK, &limits) != 0)
         FC_THROW_EXCEPTION(fc::assert_exception, "Failed to raise the stack soft limit to {} bytes", *soft);
   }
#endif
}

} // namespace fc
