#include <fc/log/logger.hpp>
#include <fc/parallel/worker_task_queue.hpp>
#include <sysio/external_debugging_plugin/debug_envelope_event_sink.hpp>
#include <utility>

namespace sysio::external_debugging {

debug_envelope_event_sink::debug_envelope_event_sink(std::size_t pending_capacity, callback_type callback)
   : _pending_capacity(pending_capacity)
   , _queue(queue_type::create({.max_threads = 1, .max_pending_items = pending_capacity}, std::move(callback))) {}

debug_envelope_event_sink::~debug_envelope_event_sink() {
   stop();
}

void debug_envelope_event_sink::enqueue(const event_type& event) {
   if (_queue->try_push(event)) {
      return;
   }

   const auto total_dropped = _dropped_envelopes.fetch_add(1, std::memory_order_relaxed) + 1;
   wlog("external_debugging_plugin: event not admitted; dropping epoch={} endpoints={} batch_op={} "
        "pending={} capacity={} total_dropped={}",
        std::get<0>(event), opp::debugging::DebugOutpostEndpointsType_Name(std::get<1>(event)),
        std::get<2>(event).to_string(), _queue->size(), _pending_capacity, total_dropped);
}

void debug_envelope_event_sink::stop() {
   std::call_once(_stop_once, [this] {
      _queue->stop();
      const auto discarded = _queue->discard_pending();
      if (discarded == 0) {
         return;
      }

      const auto total_dropped = _dropped_envelopes.fetch_add(discarded, std::memory_order_relaxed) + discarded;
      wlog("external_debugging_plugin: discarding {} pending envelopes during shutdown; total_dropped={}", discarded,
           total_dropped);
   });
}

uint64_t debug_envelope_event_sink::dropped_envelopes() const {
   return _dropped_envelopes.load(std::memory_order_relaxed);
}

std::size_t debug_envelope_event_sink::pending_envelopes() const {
   return _queue->size();
}

bool debug_envelope_event_sink::running() const {
   return _queue->running();
}

debug_envelope_slot make_debug_envelope_slot(std::shared_ptr<debug_envelope_event_sink> sink) {
   return [sink = std::move(sink)](const debug_envelope_event_sink::event_type& event) { sink->enqueue(event); };
}

} // namespace sysio::external_debugging
