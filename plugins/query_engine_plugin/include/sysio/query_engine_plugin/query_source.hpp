#pragma once
#include <sysio/query_engine_plugin/query.hpp>

namespace sysio::query_engine {
/// Direct local-controller adapter. Both methods must run in the chain executor's read-only queue.
/// The synchronized read API owns its lifetime; no controller-owned reference escapes either method.
class local_table_source {
public:
   explicit local_table_source(const chain::controller& controller)
      : controller(controller) {}
   /// Copy bounded ABI metadata and its identity before worker-side compilation.
   std::vector<table_schema> describe(const ast_query&, query_budget&) const;
   /// Capture every native page in one read callback, with ABI/state revalidation.
   captured_input capture(const typed_plan&, query_budget&) const;

private:
   const chain::controller& controller;
};
} // namespace sysio::query_engine
