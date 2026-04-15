#pragma once
#include <fc/spdlog.hpp>

#include <memory>
#include <string_view>

namespace fc {

/// Default log line pattern used when no explicit format is configured.
/// Includes a custom %k flag rendering fc::get_thread_name().
inline constexpr std::string_view DEFAULT_LOG_PATTERN =
   "%^%-5l %Y-%m-%dT%T.%f %-9!k %20!s:%-5# %-20!! ] %v%$";

/// Build a spdlog::pattern_formatter preconfigured with fc's custom %k
/// (thread-name) flag, UTC time, and the given pattern string (defaulting
/// to DEFAULT_LOG_PATTERN). Intended to be attached to a sink via
/// sink->set_formatter(fc::make_pattern_formatter()).
std::unique_ptr<spdlog::formatter> make_pattern_formatter(std::string_view pattern = DEFAULT_LOG_PATTERN);

} // namespace fc
