#pragma once

#include <fc/log/logger.hpp>
#include <string>

namespace sysio::trace_api {

// Shared "trace_api" logger.  Configured by the plugin at startup via
// fc::logger::update(logger_name, _log) so operators control verbosity
// through the "trace_api" entry in logging.json.  All trace_api_plugin
// log call sites should use this logger (fc_wlog(_log, ...), etc.) so
// that filtering applies uniformly across the plugin.
inline const std::string logger_name{"trace_api"};
inline fc::logger _log;

} // namespace sysio::trace_api
