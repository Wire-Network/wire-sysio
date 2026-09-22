#pragma once
#include <sysio/query_engine_plugin/query.hpp>

#include <future>

namespace sysio::query_engine {
/// One queued execution. Read callbacks retain this task after timeout so admission outlives all raw reads.
struct query_task {
   std::string query;
   std::shared_ptr<query_budget> budget;
   std::promise<query_result> completion;
   std::atomic<bool> completed{false};
   bool succeeded = false;
   uint64_t returned_rows = 0;
   uint64_t scanned_rows = 0, raw_bytes = 0, accounted_bytes = 0;
   std::function<void(const query_task&)> retire;
   /// The engine installs retirement only after admission; no controller ownership escapes this lifetime.
   ~query_task() {
      if (retire)
         retire(*this);
   }
};
} // namespace sysio::query_engine
