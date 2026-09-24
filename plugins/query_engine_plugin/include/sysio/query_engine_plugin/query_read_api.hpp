#pragma once
#include <sysio/query_engine_plugin/query_source.hpp>
#include <sysio/query_engine_plugin/query_task.hpp>

#include <chrono>
#include <thread>

namespace sysio::query_engine {
/// Synchronous worker-facing reads. Synchronization with block application stays inside this API;
/// callbacks copy owned ABI/row data and never execute SQL or wait for query workers.
class query_read_api {
public:
   using read_scheduler = std::function<void(std::function<void()>)>;
   /// Time left in the chain read window the calling read callback runs in. The plugin binds it to
   /// producer_plugin's window deadline; a scheduler without read windows leaves it empty. A read the
   /// window cuts short is queued again for the next window while the request deadline allows.
   using read_window_remaining = std::function<std::chrono::microseconds()>;
   /// A nonempty application_thread rejects callers whose blocking wait would prevent read dispatch.
   query_read_api(std::shared_ptr<local_table_source>, read_scheduler, std::thread::id application_thread = {},
                  read_window_remaining = {});
   /// Copy raw ABI bytes through the internally scheduled read boundary, then resolve them on the caller's worker.
   std::vector<table_schema> describe(std::shared_ptr<const ast_query>, std::shared_ptr<query_task>) const;
   /// Copy every primary page in one synchronized read, preserving coherent state across owners.
   captured_input capture(std::shared_ptr<const typed_plan>, std::shared_ptr<query_task>) const;
   /// Reject a blocking call on the application thread before it enters a queue.
   void assert_can_wait() const;
   /// Invalidate the source after workers stop; queued cancelled reads cannot touch the controller.
   void stop();

private:
   struct impl;
   std::shared_ptr<impl> state;
};
} // namespace sysio::query_engine
