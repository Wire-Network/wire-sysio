#pragma once
#include <sysio/query_engine_plugin/query.hpp>

#include <boost/program_options.hpp>

namespace sysio::query_engine {
/// Register all immutable resource limits with their named defaults.
void add_options(boost::program_options::options_description&);
/// Read checked unsigned options and enforce relationships between budgets.
query_config parse_config(const boost::program_options::variables_map&);
} // namespace sysio::query_engine
