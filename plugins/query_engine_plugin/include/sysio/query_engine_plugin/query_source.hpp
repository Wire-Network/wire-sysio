#pragma once
#include <sysio/query_engine_plugin/query.hpp>

namespace sysio::query_engine {
/// Direct local-controller adapter. Reads must run in the chain executor's read-exclusive queue.
/// The synchronized read API owns its lifetime; no controller-owned reference escapes any method.
class local_table_source {
public:
   explicit local_table_source(const chain::controller& controller)
      : controller(controller) {}
   /// Copy each owner's ABI sequence and raw ABI bytes, bounded by the ABI size limit. This is the
   /// only description work a read callback performs; resolve_schema() finishes it on a worker.
   std::vector<table_schema> capture_abis(const ast_query&, query_budget&) const;
   /// capture_abis() followed by resolve_schema() for every owner, for callers outside a read callback.
   std::vector<table_schema> describe(const ast_query&, query_budget&) const;
   /// Capture every native page in one read callback, with ABI/state revalidation.
   captured_input capture(const typed_plan&, query_budget&) const;

private:
   const chain::controller& controller;
};
} // namespace sysio::query_engine
