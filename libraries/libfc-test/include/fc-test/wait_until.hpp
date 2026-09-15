#pragma once

#include <chrono>
#include <thread>

namespace fc::test {

/// Sleep between predicate checks while a wait is outstanding.
inline constexpr auto poll = std::chrono::milliseconds(1);
/// Longest any wait may take; a regression then fails the case instead of hanging the binary.
inline constexpr auto wait_budget = std::chrono::seconds(5);

/// Poll until @p done() or wait_budget elapses; false on timeout.
template <typename Predicate>
inline bool wait_until(Predicate done) {
   const auto deadline = std::chrono::steady_clock::now() + wait_budget;
   while (!done()) {
      if (std::chrono::steady_clock::now() >= deadline)
         return false;
      std::this_thread::sleep_for(poll);
   }
   return true;
}

} // namespace fc::test
