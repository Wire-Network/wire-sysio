#pragma once
//
// Single point of entry for spdlog within fc. Defines the project-wide
// SPDLOG_ACTIVE_LEVEL and SPDLOG_LEVEL_NAMES overrides BEFORE pulling
// any <spdlog/...> header so spdlog's own #ifndef guards skip the defaults.
//
// Header rule: anything in fc that needs spdlog should #include <fc/spdlog.hpp>
// rather than <spdlog/...> directly. Exception: a .cpp that needs a specific
// sink/extra header (e.g. <spdlog/sinks/rotating_file_sink.h>) may include
// the specific header AFTER <fc/spdlog.hpp> -- the macros are already set.
//
// Why it matters: SPDLOG_ACTIVE_LEVEL gates the SPDLOG_LOGGER_<LEVEL> macros.
// If a TU pulls a spdlog header before the macro is set, spdlog falls back to
// SPDLOG_LEVEL_INFO and TRACE/DEBUG calls in that TU compile out to (void)0.
// Routing all spdlog includes through this shim eliminates that hazard.
//
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#define SPDLOG_LEVEL_NAMES  { "trace", "debug", "info", "warn", "error", "crit", "off" }

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>
